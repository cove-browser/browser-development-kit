// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/engine_contents.h"

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/values.h"
#include "bdk/core/runtime/automation_internal.h"
#include "bdk/engine/devtools_frontend.h"
#include "bdk/engine/host_context.h"
#include "bdk/engine/host_view.h"
#include "bdk/engine/identity.h"
#include "bdk/engine/json_util.h"
#include "bdk/engine/pages/page_routes.h"
#include "bdk/engine/providers/translation_service.h"
#include "bdk/engine/services/interstitial_prompts.h"
#include "bdk/engine/services/password_prompts.h"
#include "bdk/engine/services/user_scripts.h"
#include "bdk/engine/session_state.h"
#include "bdk/engine/testing.h"
#include "bdk/engine/ui_dialogs.h"
#include "bdk/engine/ui_find.h"
#include "bdk/engine/ui_permissions.h"
#include "bdk/engine/ui_presenters.h"
#include "bdk/engine/ui_request.h"
#include "chrome/browser/history/history_tab_helper.h"
#include "chrome/browser/media/webrtc/media_capture_devices_dispatcher.h"
#include "chrome/browser/profiles/profile.h"
#include "components/input/native_web_keyboard_event.h"
#include "content/public/browser/context_menu_params.h"
#include "content/public/browser/host_zoom_map.h"
#include "content/public/browser/keyboard_event_processing_result.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_view_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/url_constants.h"
#include "net/base/net_errors.h"
#include "third_party/blink/public/common/context_menu_data/edit_flags.h"
#include "third_party/blink/public/common/page/page_zoom.h"
#include "third_party/blink/public/common/user_agent/user_agent_metadata.h"
#include "third_party/blink/public/mojom/frame/fullscreen.mojom.h"
#include "third_party/blink/public/mojom/window_features/window_features.mojom.h"
#include "ui/base/clipboard/scoped_clipboard_writer.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"
#include "url/gurl.h"

namespace bdk {

namespace {

constexpr uint32_t kMainFrame = BDK_EVENT_FLAG_MAIN_FRAME;

std::vector<EngineContents*>& Registry() {
  static base::NoDestructor<std::vector<EngineContents*>> registry;
  return *registry;
}

std::string Disposition(WindowOpenDisposition disposition) {
  switch (disposition) {
    case WindowOpenDisposition::kNewBackgroundTab:
      return "background";
    case WindowOpenDisposition::kNewWindow:
      return "newWindow";
    case WindowOpenDisposition::kNewPopup:
      return "popup";
    case WindowOpenDisposition::kOffTheRecord:
      return "privateWindow";
    default:
      return "foreground";
  }
}

std::string TerminationReason(base::TerminationStatus status) {
  switch (status) {
    case base::TERMINATION_STATUS_PROCESS_WAS_KILLED:
      return "killed";
    case base::TERMINATION_STATUS_OOM:
      return "oom";
    case base::TERMINATION_STATUS_LAUNCH_FAILED:
      return "launchFailed";
    case base::TERMINATION_STATUS_NORMAL_TERMINATION:
      return "exited";
    default:
      return "crashed";
  }
}

// The menu the default presenter shows and a custom presenter is offered,
// for what the page reported under the pointer.
base::ListValue MenuItems(const content::ContextMenuParams& params,
                          content::WebContents* contents) {
  base::ListValue items;
  auto add = [&items](const char* id, const char* title, bool enabled) {
    items.Append(base::DictValue()
                     .Set("id", id)
                     .Set("kind", "command")
                     .Set("title", title)
                     .Set("enabled", enabled));
  };
  auto separator = [&items] {
    if (!items.empty()) {
      items.Append(base::DictValue().Set("id", "").Set("kind", "separator"));
    }
  };
  if (params.link_url.is_valid()) {
    add("openLinkInNewPage", "Open Link in New Page", true);
    add("copyLink", "Copy Link Address", true);
  }
  if (params.is_editable || !params.selection_text.empty()) {
    separator();
    const int flags = params.edit_flags;
    if (params.is_editable) {
      add("cut", "Cut", (flags & blink::ContextMenuDataEditFlags::kCanCut));
    }
    add("copy", "Copy", (flags & blink::ContextMenuDataEditFlags::kCanCopy));
    if (params.is_editable) {
      add("paste", "Paste",
          (flags & blink::ContextMenuDataEditFlags::kCanPaste));
    }
    add("selectAll", "Select All",
        (flags & blink::ContextMenuDataEditFlags::kCanSelectAll));
  }
  if (!params.link_url.is_valid() && !params.is_editable &&
      params.selection_text.empty()) {
    content::NavigationController& controller = contents->GetController();
    add("back", "Back", controller.CanGoBack());
    add("forward", "Forward", controller.CanGoForward());
    add("reload", "Reload", true);
  }
  if (DevToolsEnabled()) {
    separator();
    add("inspect", "Inspect", true);
  }
  return items;
}

// The menu the host view shows for `items`, as the engine offered them or as
// the application arranged them (BDKMenuItem: "isEnabled", "kind",
// "children"). The host view's menu is flat, so a submenu's items are shown in
// its place. An entry that is not an object is skipped.
void AppendMenuItems(const base::ListValue& items, std::vector<MenuItem>& out) {
  for (const base::Value& v : items) {
    const base::DictValue* d = v.GetIfDict();
    if (!d) {
      continue;
    }
    const std::string* kind = d->FindString("kind");
    if (kind && *kind == "separator") {
      out.push_back(MenuItem());
      continue;
    }
    if (kind && *kind == "submenu") {
      if (const base::ListValue* children = d->FindList("children")) {
        AppendMenuItems(*children, out);
      }
      continue;
    }
    const std::string* id = d->FindString("id");
    if (!id || id->empty()) {
      continue;
    }
    MenuItem item;
    item.id = *id;
    if (const std::string* title = d->FindString("title")) {
      item.title = *title;
    }
    item.enabled = d->FindBool("isEnabled")
                       .value_or(d->FindBool("enabled").value_or(true));
    out.push_back(std::move(item));
  }
}

}  // namespace

EngineProfile::EngineProfile() = default;
EngineProfile::~EngineProfile() = default;

Profile* ProfileOf(const bdkcore::Contents& contents) {
  return static_cast<EngineProfile*>(contents.profile->data.get())->profile;
}

EngineContents::EngineContents(
    const std::shared_ptr<bdkcore::Contents>& contents,
    std::unique_ptr<content::WebContents> web_contents)
    : core_(contents), host_view_(HostView::Create()) {
  Registry().push_back(this);
  host_view_->SetWindowChangedCallback(base::BindRepeating(
      &EngineContents::OnHostViewWindowChanged, weak_factory_.GetWeakPtr()));
  dialogs_ = std::make_unique<PageDialogs>(*this);
  find_ = std::make_unique<PageFind>(*this);
  Attach(std::move(web_contents));
}

EngineContents::~EngineContents() {
  std::erase(Registry(), this);
  // Dialogs and find UI go while the page is whole.
  find_.reset();
  dialogs_.reset();
  // A front end's window goes first; then the inspected page learns that its
  // DevTools closed. The bindings belong to the front end's WebUI, which goes
  // with the page.
  if (devtools_window_) {
    std::move(devtools_window_).Run();
  }
  if (devtools_) {
    if (EngineContents* inspected =
            devtools_->inspected()
                ? EngineContents::FromWebContents(devtools_->inspected())
                : nullptr) {
      inspected->devtools_page_.reset();
      inspected->NotifyDevTools(false);
    }
  }
  devtools_.reset();
  Observe(nullptr);
  if (web_contents_) {
    web_contents_->SetDelegate(nullptr);
  }
  // The host view lets go of the page's view before the page goes.
  host_view_.reset();
  if (web_contents_) {
    // Nothing of ours refers to the page when its tab destroys it.
    content::WebContents* contents = web_contents_.ExtractAsDangling();
    if (HostContext* context = HostContexts::Get().ContextOf(contents)) {
      context->Destroy(contents);
    }
  }
}

// static
EngineContents* EngineContents::From(const bdkcore::Contents& contents) {
  return static_cast<EngineContents*>(contents.data.get());
}

// static
EngineContents* EngineContents::FromWebContents(
    content::WebContents* contents) {
  for (EngineContents* page : Registry()) {
    if (page->web_contents_ == contents) {
      return page;
    }
  }
  return nullptr;
}

// static
const std::vector<EngineContents*>& EngineContents::All() {
  return Registry();
}

void EngineContents::Attach(
    std::unique_ptr<content::WebContents> web_contents) {
  content::WebContents* fresh = web_contents.get();
  std::unique_ptr<content::WebContents> old;
  if (web_contents_) {
    // What the old page asked for is withdrawn with it.
    dialogs_->CancelDialogs(web_contents_, /*reset_state=*/true);
    find_->OnNavigated();
    web_contents_->SetDelegate(nullptr);
    // The page keeps its tab; the tab model swaps the contents.
    old = HostContexts::Get()
              .ContextOf(web_contents_)
              ->Replace(web_contents_, std::move(web_contents));
  } else {
    // A new page is a background tab of its profile's parking context: no
    // window of Chromium's is shown, and no registered window yet holds it.
    HostContexts::Get()
        .Parking(ProfileOf(*core()))
        .Add(std::move(web_contents));
  }
  web_contents_ = fresh;
  // The Browser took the delegate with the tab.
  web_contents_->SetDelegate(this);
  // Settings that belong to the page, not to one WebContents, before it is
  // shown or loads.
  if (muted_) {
    web_contents_->SetAudioMuted(true);
  }
  Observe(web_contents_);
  // Visits are recorded in history, except in a private profile (the helper
  // has no history service there). Creating it again does nothing.
  HistoryTabHelper::CreateForWebContents(web_contents_);
  // A page of a profile that has user scripts gets them now, before its first
  // document loads, so documentStart scripts run in it; a replaced WebContents
  // (restore, discard) needs them again. A profile with none costs nothing.
  ApplyUserScriptsToPage(web_contents_);
  AttachTranslation(web_contents_);
  // A DevTools front end moves its bindings to the new front-end WebContents
  // before the old one goes.
  if (devtools_) {
    devtools_->SetFrontend(web_contents_);
  }
  host_view_->SetWebContents(web_contents_);
  if (!user_agent_.empty()) {
    SetUserAgent(user_agent_);
  }
  // Prompts and find follow the page's WebContents.
  InstallPermissionPrompts(*this, web_contents_);
  find_->Attach(web_contents_);
  // `old` goes after the host view shows the new page.
}

void EngineContents::MoveTo(HostContext& context) {
  HostContext* from = HostContexts::Get().ContextOf(web_contents_);
  if (from == &context) {
    return;
  }
  HostContexts::Get().Move(web_contents_, context);
  // The Browser took the delegate with the tab.
  web_contents_->SetDelegate(this);
  if (focused_) {
    ActivateTab();
  }
}

void EngineContents::Park() {
  MoveTo(HostContexts::Get().Parking(ProfileOf(*core())));
}

void EngineContents::Reconcile() {
  const bdk_native_handle_t native = host_view_->Window();
  const bdkcore::Window* window = HostContexts::Get().FindWindow(native);
  if (!window) {
    // In no registered window: the page stays where it is, a background tab
    // of its last context.
    return;
  }
  MoveTo(HostContexts::Get().ForWindow(window, native, ProfileOf(*core())));
}

bool EngineContents::IsInWindow(const bdkcore::Window* window) {
  HostContext* context = HostContexts::Get().ContextOf(web_contents_);
  return context && context->window() == window;
}

void EngineContents::OnHostViewWindowChanged() {
  if (reconcile_pending_) {
    return;
  }
  reconcile_pending_ = true;
  // Not from inside the application's own call (a view added to its window).
  NoteTaskPosted();
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](base::WeakPtr<EngineContents> self) {
                       NoteTaskRun();
                       if (self) {
                         self->reconcile_pending_ = false;
                         self->Reconcile();
                       }
                     },
                     weak_factory_.GetWeakPtr()));
}

void EngineContents::Focus() {
  LoadIfDiscarded();
  web_contents_->Focus();
  ActivateTab();
}

void EngineContents::ActivateTab() {
  HostContext* context = HostContexts::Get().ContextOf(web_contents_);
  if (context && !context->IsActive(web_contents_)) {
    context->Activate(web_contents_);
  }
}

void EngineContents::ReplaceWebContents(
    std::unique_ptr<content::WebContents> web_contents) {
  discarded_ = false;
  Attach(std::move(web_contents));
}

void EngineContents::Emit(uint32_t kind,
                          const std::string& url,
                          const std::string& detail,
                          int32_t net_error,
                          uint32_t flags) {
  if (auto c = core()) {
    // The application never sees the internal URL of one of its pages.
    bdkcore::emit_contents_event(c, kind, DisplayUrl(url), detail, net_error,
                                 flags);
  }
}

void EngineContents::Load(const std::string& url) {
  content::NavigationController::LoadURLParams params{GURL(url)};
  params.transition_type = ui::PAGE_TRANSITION_TYPED;
  if (!user_agent_.empty()) {
    params.override_user_agent =
        content::NavigationController::UA_OVERRIDE_TRUE;
  }
  discarded_ = false;
  web_contents_->GetController().LoadURLWithParams(params);
}

void EngineContents::Discard() {
  if (discarded_) {
    return;
  }
  std::string state = SerializeNavigation(web_contents_->GetController());
  RestoredNavigation restored;
  content::BrowserContext* context = web_contents_->GetBrowserContext();
  if (ParseNavigation(base::as_byte_span(state), context, &restored) !=
      ParseResult::kOk) {
    return;
  }
  // A discarded page has no renderer until it loads again.
  content::WebContents::CreateParams params(context);
  params.desired_renderer_state =
      content::WebContents::CreateParams::kNoRendererProcess;
  std::unique_ptr<content::WebContents> fresh =
      content::WebContents::Create(params);
  if (!restored.entries.empty()) {
    fresh->GetController().Restore(
        restored.current, content::RestoreType::kRestored, &restored.entries);
  }
  Attach(std::move(fresh));
  discarded_ = true;
}

void EngineContents::LoadIfDiscarded() {
  if (discarded_) {
    discarded_ = false;
    web_contents_->GetController().LoadIfNecessary();
  }
}

void EngineContents::Close() {
  // Runs the page's unload handlers; Chromium then calls CloseContents().
  closing_ = true;
  web_contents_->ClosePage();
}

void EngineContents::SetZoom(double zoom) {
  zoom_ = zoom;
  content::HostZoomMap::GetForWebContents(web_contents_.get())
      ->SetTemporaryZoomLevel(
          web_contents_->GetPrimaryMainFrame()->GetGlobalId(),
          blink::ZoomFactorToZoomLevel(zoom));
}

double EngineContents::Zoom() {
  return blink::ZoomLevelToZoomFactor(
      content::HostZoomMap::GetZoomLevel(web_contents_.get()));
}

void EngineContents::SetUserAgent(const std::string& user_agent) {
  user_agent_ = user_agent;
  web_contents_->SetUserAgentOverride(
      blink::UserAgentOverride::UserAgentOnly(user_agent),
      /*override_in_new_tabs=*/false);
  content::NavigationController& controller = web_contents_->GetController();
  for (int i = 0; i < controller.GetEntryCount(); ++i) {
    controller.GetEntryAtIndex(i)->SetIsOverridingUserAgent(
        !user_agent.empty());
  }
}

void EngineContents::SetMuted(bool muted) {
  muted_ = muted;
  web_contents_->SetAudioMuted(muted);
}

// content::WebContentsDelegate -----------------------------------------------

content::WebContents* EngineContents::OpenURLFromTab(
    content::WebContents* source,
    const content::OpenURLParams& params,
    base::OnceCallback<void(content::NavigationHandle&)>
        navigation_handle_callback) {
  if (params.disposition == WindowOpenDisposition::kCurrentTab) {
    base::WeakPtr<content::NavigationHandle> handle =
        source->GetController().LoadURLWithParams(
            content::NavigationController::LoadURLParams(params));
    if (handle && navigation_handle_callback) {
      std::move(navigation_handle_callback).Run(*handle);
    }
    return source;
  }
  // Anything else would have opened a tab or a window: the application
  // decides where the new page goes.
  std::unique_ptr<content::WebContents> created = content::WebContents::Create(
      content::WebContents::CreateParams(source->GetBrowserContext()));
  content::WebContents* raw = created.get();
  base::WeakPtr<content::NavigationHandle> handle =
      raw->GetController().LoadURLWithParams(
          content::NavigationController::LoadURLParams(params));
  if (handle && navigation_handle_callback) {
    std::move(navigation_handle_callback).Run(*handle);
  }
  EngineContents* page =
      RequestNewWindow(std::move(created), "newPage", params.disposition,
                       params.url, nullptr, params.user_gesture);
  return page ? page->web_contents() : nullptr;
}

content::WebContents* EngineContents::AddNewContents(
    content::WebContents* source,
    std::unique_ptr<content::WebContents> new_contents,
    const GURL& target_url,
    WindowOpenDisposition disposition,
    const blink::mojom::WindowFeatures& window_features,
    bool user_gesture,
    bool* was_blocked) {
  const bool popup = disposition == WindowOpenDisposition::kNewPopup;
  EngineContents* page =
      RequestNewWindow(std::move(new_contents), popup ? "popup" : "newPage",
                       disposition, target_url, &window_features, user_gesture);
  return page ? page->web_contents() : nullptr;
}

EngineContents* EngineContents::RequestNewWindow(
    std::unique_ptr<content::WebContents> new_contents,
    const std::string& kind,
    WindowOpenDisposition disposition,
    const GURL& target_url,
    const blink::mojom::WindowFeatures* features,
    bool user_gesture) {
  std::shared_ptr<bdkcore::Contents> opener = core();
  std::shared_ptr<bdkcore::Runtime> rt = opener ? opener->rt.lock() : nullptr;
  if (!rt || rt->shutting || !HostContexts::CanHost(ProfileOf(*opener))) {
    return nullptr;
  }
  auto page = std::make_shared<bdkcore::Contents>();
  page->rt = rt;
  page->profile = opener->profile;
  page->handle = bdkcore::table_make(bdkcore::T_CONTENTS, page);
  auto engine_page =
      std::make_unique<EngineContents>(page, std::move(new_contents));
  EngineContents* raw = engine_page.get();
  page->data = std::move(engine_page);
  rt->contents.push_back(page);

  base::DictValue payload;
  payload.Set("kind", kind);
  payload.Set("disposition", Disposition(disposition));
  payload.Set("targetURL", DisplayUrl(target_url));
  // Handles are 64-bit: JSON numbers cannot carry them exactly.
  payload.Set("newContents", base::NumberToString(page->handle));
  payload.Set("opener", base::NumberToString(opener->handle));
  payload.Set("userGesture", user_gesture);
  if (features) {
    base::DictValue f;
    if (features->has_x)
      f.Set("x", features->bounds.x());
    if (features->has_y)
      f.Set("y", features->bounds.y());
    if (features->has_width)
      f.Set("width", features->bounds.width());
    if (features->has_height)
      f.Set("height", features->bounds.height());
    payload.Set("features", std::move(f));
  }

  RaiseUiRequest(opener, BDK_SURFACE_NEW_WINDOWS, payload,
                 base::BindOnce(
                     [](std::weak_ptr<bdkcore::Contents> weak_page,
                        const UiResult& result) {
                       std::shared_ptr<bdkcore::Contents> p = weak_page.lock();
                       if (!p || p->dead) {
                         return;
                       }
                       if (result.outcome == bdkcore::UiOutcome::kCompleted &&
                           result.completion == "accept") {
                         return;  // the application placed the new page
                       }
                       // Rejected, cancelled, withdrawn, or no handler: BDK has
                       // no window of its own to put the page in (the default
                       // presenter is T-UIR-03, M3).
                       if (auto runtime = p->rt.lock()) {
                         bdkcore::destroy_contents(runtime, p);
                       }
                     },
                     std::weak_ptr<bdkcore::Contents>(page)));
  return raw;
}

void EngineContents::ToggleDevTools() {
  if (DevToolsOpen()) {
    CloseDevTools();
    return;
  }
  OpenDevTools();
}

void EngineContents::OpenDevTools() {
  std::shared_ptr<bdkcore::Contents> inspected = core();
  std::shared_ptr<bdkcore::Runtime> rt =
      inspected ? inspected->rt.lock() : nullptr;
  if (devtools_page_ || !DevToolsEnabled() || !rt || rt->shutting ||
      !HostContexts::CanHost(ProfileOf(*inspected))) {
    return;
  }
  const GURL url = DevToolsFrontend::FrontendURL();
  std::unique_ptr<content::WebContents> frontend = content::WebContents::Create(
      content::WebContents::CreateParams(web_contents_->GetBrowserContext()));
  frontend->GetController().LoadURL(url, content::Referrer(),
                                    ui::PAGE_TRANSITION_AUTO_TOPLEVEL,
                                    std::string());
  content::WebContents* frontend_contents = frontend.get();

  // The front end is a page like any other, so the application can place its
  // view anywhere (swift-api.md 14).
  auto page = std::make_shared<bdkcore::Contents>();
  page->rt = rt;
  page->profile = inspected->profile;
  page->handle = bdkcore::table_make(bdkcore::T_CONTENTS, page);
  auto engine_page =
      std::make_unique<EngineContents>(page, std::move(frontend));
  EngineContents* front = engine_page.get();
  page->data = std::move(engine_page);
  rt->contents.push_back(page);
  devtools_page_ = front->weak_factory_.GetWeakPtr();
  base::WeakPtr<EngineContents> weak_front = front->weak_factory_.GetWeakPtr();
  front->devtools_ = std::make_unique<DevToolsFrontend>(
      frontend_contents, web_contents_.get(),
      base::BindRepeating(
          [](base::WeakPtr<EngineContents> p) {
            // Never from inside the bindings' own call.
            base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
                FROM_HERE, base::BindOnce(&EngineContents::CloseFrontEnd, p));
          },
          weak_front));

  base::DictValue payload;
  // Handles are 64-bit: JSON numbers cannot carry them exactly.
  payload.Set("inspected", base::NumberToString(inspected->handle));
  payload.Set("frontend", base::NumberToString(page->handle));
  payload.Set("preferredDock", "undocked");
  payload.Set("url", url.spec());
  RaiseUiRequest(
      inspected, BDK_SURFACE_DEVTOOLS, payload,
      base::BindOnce(
          [](base::WeakPtr<EngineContents> inspected_page,
             std::weak_ptr<bdkcore::Contents> weak_frontend,
             const UiResult& result) {
            std::shared_ptr<bdkcore::Contents> f = weak_frontend.lock();
            if (!f || f->dead) {
              return;
            }
            EngineContents* front_page = EngineContents::From(*f);
            if (result.outcome == bdkcore::UiOutcome::kCompleted &&
                result.completion == "accept") {
              // The application placed the front end's view.
            } else if (result.outcome == bdkcore::UiOutcome::kDefaulted &&
                       front_page) {
              // The default presenter: an undocked window of BDK's own.
              // Closing it closes the front end.
              base::WeakPtr<EngineContents> weak = front_page->GetWeakPtr();
              front_page->devtools_window_ = UiPresenters::Get().ShowPageWindow(
                  front_page->host_view_.get(), "DevTools",
                  base::BindOnce(&EngineContents::CloseFrontEnd, weak));
            } else {
              // Rejected, disabled, cancelled or withdrawn: no front end.
              if (auto runtime = f->rt.lock()) {
                bdkcore::destroy_contents(runtime, f);
              }
              return;
            }
            if (inspected_page) {
              inspected_page->NotifyDevTools(true);
            }
          },
          GetWeakPtr(), std::weak_ptr<bdkcore::Contents>(page)));
}

void EngineContents::CloseDevTools() {
  if (devtools_page_) {
    devtools_page_->CloseFrontEnd();
  }
}

void EngineContents::CloseFrontEnd() {
  if (!devtools_window_) {
    RequestClose();
    return;
  }
  // Destroyed later: this may run inside the window's own close.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](std::weak_ptr<bdkcore::Contents> weak) {
                       std::shared_ptr<bdkcore::Contents> c = weak.lock();
                       std::shared_ptr<bdkcore::Runtime> rt =
                           c ? c->rt.lock() : nullptr;
                       if (c && rt && !c->dead) {
                         bdkcore::destroy_contents(rt, c);
                       }
                     },
                     core_));
}

std::string EngineContents::DevToolsFrontendId() const {
  if (!devtools_page_) {
    return std::string();
  }
  std::shared_ptr<bdkcore::Contents> f = devtools_page_->core();
  return f ? bdkcore::contents_id(f->handle) : std::string();
}

void EngineContents::NotifyDevTools(bool open) {
  std::shared_ptr<bdkcore::Contents> c = core();
  std::shared_ptr<bdkcore::Runtime> rt = c ? c->rt.lock() : nullptr;
  if (!rt || rt->shutting) {
    return;
  }
  bdkcore::emit_builtin_event_for(
      rt, "devtools.stateChanged", c->profile ? c->profile->handle : 0,
      c->handle,
      "{\"contents\":\"" + bdkcore::contents_id(c->handle) +
          "\",\"open\":" + (open ? "true" : "false") + "}");
}

bool EngineContents::CanOpenDevTools() {
  return SurfaceAvailable(core(), BDK_SURFACE_DEVTOOLS);
}

void EngineContents::OpenViewSource() {
  const GURL url(content::kViewSourceScheme + std::string(":") +
                 web_contents_->GetLastCommittedURL().spec());
  std::unique_ptr<content::WebContents> source = content::WebContents::Create(
      content::WebContents::CreateParams(web_contents_->GetBrowserContext()));
  source->GetController().LoadURL(url, content::Referrer(),
                                  ui::PAGE_TRANSITION_LINK, std::string());
  RequestNewWindow(std::move(source), "newPage",
                   WindowOpenDisposition::kNewForegroundTab, url, nullptr,
                   /*user_gesture=*/true);
}

void EngineContents::RequestClose() {
  CloseContents(web_contents_.get());
}

void EngineContents::CloseContents(content::WebContents* source) {
  if (close_reported_) {
    return;
  }
  close_reported_ = true;
  closing_ = false;
  if (auto c = core()) {
    c->closed = true;
    if (auto rt = c->rt.lock()) {
      bdkcore::withdraw_contents_requests(rt, c->handle);
    }
  }
  Emit(BDK_CONTENTS_EVENT_CLOSED, "", "{}", 0, 0);
}

bool EngineContents::HandleContextMenu(
    content::RenderFrameHost& render_frame_host,
    const content::ContextMenuParams& params) {
  std::shared_ptr<bdkcore::Contents> c = core();
  std::shared_ptr<bdkcore::Runtime> rt = c ? c->rt.lock() : nullptr;
  if (!rt || rt->shutting) {
    return true;
  }
  base::ListValue items = MenuItems(params, web_contents_.get());
  base::DictValue context;
  context.Set("linkURL", DisplayUrl(params.link_url));
  context.Set("srcURL", params.src_url.spec());
  context.Set("pageURL", DisplayUrl(params.page_url));
  context.Set("frameURL", DisplayUrl(params.frame_url));
  context.Set("selectionText", base::UTF16ToUTF8(params.selection_text));
  context.Set("isEditable", params.is_editable);
  context.Set("misspelledWord", base::UTF16ToUTF8(params.misspelled_word));
  base::ListValue suggestions;
  for (const std::u16string& s : params.dictionary_suggestions) {
    suggestions.Append(base::UTF16ToUTF8(s));
  }
  context.Set("suggestions", std::move(suggestions));
  base::DictValue payload;
  payload.Set("location",
              base::DictValue().Set("x", params.x).Set("y", params.y));
  payload.Set("context", std::move(context));
  payload.Set("items", items.Clone());

  // The anchor is the point the menu was asked for.
  RaiseUiRequest(
      c, BDK_SURFACE_CONTEXT_MENUS, payload,
      base::BindOnce(
          [](base::WeakPtr<EngineContents> self,
             content::ContextMenuParams params, base::ListValue items,
             const UiResult& result) {
            if (!self) {
              return;
            }
            double x = params.x;
            double y = params.y;
            const base::ListValue* shown = &items;
            if (result.outcome == bdkcore::UiOutcome::kCompleted &&
                result.completion == "accept") {
              // ui-requests.md section 5: accept {"action":"perform","item"}
              // runs one of the engine's items; accept {"action":"show",
              // "items","at"?} shows the application's arrangement of them.
              const std::string* action = result.args.FindString("action");
              if (action && *action == "perform") {
                if (const std::string* id = result.args.FindString("item")) {
                  self->PerformMenuItem(params, *id);
                }
                return;
              }
              const base::ListValue* arranged = result.args.FindList("items");
              if (!action || *action != "show" || !arranged) {
                return;
              }
              shown = arranged;
              if (const base::DictValue* at = result.args.FindDict("at")) {
                x = at->FindDouble("x").value_or(x);
                y = at->FindDouble("y").value_or(y);
              }
            } else if (result.outcome != bdkcore::UiOutcome::kDefaulted) {
              return;  // dismissed, rejected or gone
            }
            std::vector<MenuItem> menu;
            AppendMenuItems(*shown, menu);
            self->host_view_->ShowMenu(
                menu, x, y,
                base::BindOnce(&EngineContents::PerformMenuItem, self, params));
          },
          weak_factory_.GetWeakPtr(), params, std::move(items)),
      host_view_.get(),
      std::array<double, 2>{static_cast<double>(params.x),
                            static_cast<double>(params.y)});
  return true;
}

void EngineContents::PerformMenuItem(const content::ContextMenuParams& params,
                                     const std::string& id) {
  content::NavigationController& controller = web_contents_->GetController();
  if (id == "back" && controller.CanGoBack()) {
    controller.GoBack();
  } else if (id == "forward" && controller.CanGoForward()) {
    controller.GoForward();
  } else if (id == "reload") {
    controller.Reload(content::ReloadType::NORMAL, true);
  } else if (id == "cut") {
    web_contents_->Cut();
  } else if (id == "copy") {
    web_contents_->Copy();
  } else if (id == "paste") {
    web_contents_->Paste();
  } else if (id == "selectAll") {
    web_contents_->SelectAll();
  } else if (id == "copyLink" && params.link_url.is_valid()) {
    ui::ScopedClipboardWriter(ui::ClipboardBuffer::kCopyPaste)
        .WriteText(base::UTF8ToUTF16(params.link_url.spec()));
  } else if (id == "inspect" && DevToolsEnabled()) {
    OpenDevTools();
  } else if (id == "openLinkInNewPage" && params.link_url.is_valid()) {
    content::OpenURLParams open(params.link_url, content::Referrer(),
                                WindowOpenDisposition::kNewForegroundTab,
                                ui::PAGE_TRANSITION_LINK,
                                /*is_renderer_initiated=*/false);
    open.user_gesture = true;
    OpenURLFromTab(web_contents_.get(), open, {});
  }
}

content::KeyboardEventProcessingResult EngineContents::PreHandleKeyboardEvent(
    content::WebContents* source,
    const input::NativeWebKeyboardEvent& event) {
  std::shared_ptr<bdkcore::Contents> c = core();
  if (c && bdkcore::reserve_key(*c, HostView::KeyEvent(event))) {
    return content::KeyboardEventProcessingResult::HANDLED;
  }
  return content::KeyboardEventProcessingResult::NOT_HANDLED;
}

bool EngineContents::HandleKeyboardEvent(
    content::WebContents* source,
    const input::NativeWebKeyboardEvent& event) {
  std::shared_ptr<bdkcore::Contents> c = core();
  if (c && c->key_unhandled) {
    bdkcore::report_unhandled_key(*c, HostView::KeyEvent(event));
    return true;
  }
  return HostView::RedispatchUnhandledKey(event);
}

bool EngineContents::CanEnterFullscreenModeForTab(
    content::RenderFrameHost* requesting_frame) {
  // A disabled surface denies: the Fullscreen API rejects.
  return SurfaceAvailable(core(), BDK_SURFACE_FULLSCREEN);
}

void EngineContents::SetFullscreen(bool fullscreen) {
  fullscreen_ = fullscreen;
  web_contents_->GetPrimaryMainFrame()
      ->GetRenderViewHost()
      ->GetWidget()
      ->SynchronizeVisualProperties();
}

void EngineContents::EnterFullscreenModeForTab(
    content::RenderFrameHost* requesting_frame,
    const blink::mojom::FullscreenOptions& options) {
  std::shared_ptr<bdkcore::Contents> c = core();
  std::shared_ptr<bdkcore::Runtime> rt = c ? c->rt.lock() : nullptr;
  if (!rt || rt->shutting) {
    return;
  }
  // Fullscreen while the application decides. Content has no asynchronous
  // refusal (only CanEnterFullscreenModeForTab, which must answer at once),
  // so a denial makes the page leave fullscreen again.
  SetFullscreen(true);
  RaiseUiRequest(
      c, BDK_SURFACE_FULLSCREEN,
      base::DictValue()
          .Set("origin", requesting_frame->GetLastCommittedOrigin().Serialize())
          .Set("isEntering", true),
      base::BindOnce(
          [](base::WeakPtr<EngineContents> self, const UiResult& result) {
            if (!self) {
              return;
            }
            if (!self->fullscreen_) {
              return;  // the page already left fullscreen
            }
            if (result.outcome == bdkcore::UiOutcome::kCompleted &&
                result.completion == "accept") {
              return;  // the application makes the page fill the screen
            }
            if (result.outcome == bdkcore::UiOutcome::kDefaulted) {
              self->host_view_->SetWindowFullscreen(true);
              return;
            }
            self->web_contents_->ExitFullscreen(/*will_cause_resize=*/true);
          },
          weak_factory_.GetWeakPtr()));
}

void EngineContents::ExitFullscreenModeForTab(content::WebContents* source) {
  if (!fullscreen_) {
    return;
  }
  SetFullscreen(false);
  host_view_->SetWindowFullscreen(false);
  std::shared_ptr<bdkcore::Contents> c = core();
  std::shared_ptr<bdkcore::Runtime> rt = c ? c->rt.lock() : nullptr;
  if (!rt || rt->shutting) {
    return;
  }
  // Tells a custom presenter to restore its layout; nothing to answer.
  RaiseUiRequest(c, BDK_SURFACE_FULLSCREEN,
                 base::DictValue()
                     .Set("origin", web_contents_->GetPrimaryMainFrame()
                                        ->GetLastCommittedOrigin()
                                        .Serialize())
                     .Set("isEntering", false),
                 UiFinish());
}

bool EngineContents::IsFullscreenForTabOrPending(
    const content::WebContents* web_contents) {
  return fullscreen_;
}

content::JavaScriptDialogManager* EngineContents::GetJavaScriptDialogManager(
    content::WebContents* source) {
  return dialogs_.get();
}

void EngineContents::RunFileChooser(
    content::RenderFrameHost* render_frame_host,
    scoped_refptr<content::FileSelectListener> listener,
    const blink::mojom::FileChooserParams& params) {
  dialogs_->RunFileChooser(render_frame_host, std::move(listener), params);
}

void EngineContents::EnumerateDirectory(
    content::WebContents* web_contents,
    scoped_refptr<content::FileSelectListener> listener,
    const base::FilePath& path) {
  dialogs_->EnumerateDirectory(std::move(listener), path);
}

// Camera and microphone go through Chrome's media capture logic, which asks
// the page's permission request manager (ui_permissions.h) before it opens a
// device.
void EngineContents::RequestMediaAccessPermission(
    content::WebContents* web_contents,
    const content::MediaStreamRequest& request,
    content::MediaResponseCallback callback) {
  MediaCaptureDevicesDispatcher::GetInstance()->ProcessMediaAccessRequest(
      web_contents, request, std::move(callback), /*extension=*/nullptr);
}

bool EngineContents::CheckMediaAccessPermission(
    content::RenderFrameHost* render_frame_host,
    const url::Origin& security_origin,
    blink::mojom::MediaStreamType type) {
  return MediaCaptureDevicesDispatcher::GetInstance()
      ->CheckMediaAccessPermission(render_frame_host, security_origin, type,
                                   /*extension=*/nullptr);
}

// content::WebContentsObserver -----------------------------------------------

void EngineContents::DidStartNavigation(content::NavigationHandle* handle) {
  if (!handle->IsInPrimaryMainFrame()) {
    return;
  }
  uint32_t flags = kMainFrame;
  if (handle->IsSameDocument()) {
    flags |= BDK_EVENT_FLAG_SAME_DOCUMENT;
  }
  if (!handle->IsRendererInitiated() || handle->HasUserGesture()) {
    flags |= BDK_EVENT_FLAG_USER_INITIATED;
  }
  Emit(BDK_CONTENTS_EVENT_NAVIGATION_STARTED, handle->GetURL().spec(), "{}", 0,
       flags);
}

void EngineContents::DidFinishNavigation(content::NavigationHandle* handle) {
  if (!handle->IsInPrimaryMainFrame()) {
    return;
  }
  if (!handle->IsSameDocument()) {
    OnPrimaryNavigationFinished(*this, handle);  // security interstitials
    OnPageNavigatedForPasswords(web_contents_.get());
  }
  uint32_t flags = kMainFrame;
  if (handle->IsSameDocument()) {
    flags |= BDK_EVENT_FLAG_SAME_DOCUMENT;
  }
  if (!handle->IsRendererInitiated() || handle->HasUserGesture()) {
    flags |= BDK_EVENT_FLAG_USER_INITIATED;
  }
  const std::string url = handle->GetURL().spec();
  if (handle->HasCommitted() && !handle->IsSameDocument()) {
    dialogs_->OnNavigated();
    find_->OnNavigated();
  }
  if (!handle->IsSameDocument()) {
    last_navigation_failed_ = !handle->HasCommitted() || handle->IsErrorPage();
  }
  if (handle->HasCommitted() && !handle->IsErrorPage()) {
    if (zoom_ != 1.0 && !handle->IsSameDocument()) {
      SetZoom(zoom_);
    }
    Emit(BDK_CONTENTS_EVENT_NAVIGATION_COMMITTED, url, "{}", 0, flags);
    return;
  }
  const int net_error = handle->GetNetErrorCode();
  if (handle->IsErrorPage()) {
    flags |= BDK_EVENT_FLAG_ERROR_PAGE;
  }
  const char* code = net_error == net::ERR_ABORTED ? "aborted" : "netError";
  Emit(BDK_CONTENTS_EVENT_NAVIGATION_FAILED, url,
       ToJson(base::DictValue()
                  .Set("code", code)
                  .Set("description", net::ErrorToShortString(net_error))),
       net_error, flags);
}

void EngineContents::DidStopLoading() {
  // A failed navigation already ended with NAVIGATION_FAILED; the error page
  // it shows is not a finished load of the page that was asked for.
  if (last_navigation_failed_) {
    return;
  }
  Emit(BDK_CONTENTS_EVENT_LOAD_FINISHED,
       web_contents_->GetLastCommittedURL().spec(), "{}", 0, kMainFrame);
}

void EngineContents::TitleWasSet(content::NavigationEntry* entry) {
  // The error page's title is not the page's.
  if (!entry || last_navigation_failed_ ||
      entry != web_contents_->GetController().GetLastCommittedEntry()) {
    return;
  }
  Emit(BDK_CONTENTS_EVENT_TITLE_CHANGED, entry->GetURL().spec(),
       ToJson(base::DictValue().Set(
           "title", base::UTF16ToUTF8(web_contents_->GetTitle()))),
       0, kMainFrame);
}

void EngineContents::PrimaryMainFrameRenderProcessGone(
    base::TerminationStatus status) {
  Emit(BDK_CONTENTS_EVENT_RENDERER_GONE,
       web_contents_->GetLastCommittedURL().spec(),
       ToJson(base::DictValue().Set("reason", TerminationReason(status))), 0,
       kMainFrame);
}

void EngineContents::OnWebContentsFocused(
    content::RenderWidgetHost* render_widget_host) {
  LoadIfDiscarded();
  focused_ = true;
  ActivateTab();
  Emit(BDK_CONTENTS_EVENT_FOCUS_CHANGED, "",
       ToJson(base::DictValue().Set("focused", true)), 0, 0);
}

void EngineContents::OnWebContentsLostFocus(
    content::RenderWidgetHost* render_widget_host) {
  focused_ = false;
  Emit(BDK_CONTENTS_EVENT_FOCUS_CHANGED, "",
       ToJson(base::DictValue().Set("focused", false)), 0, 0);
}

void EngineContents::OnVisibilityChanged(content::Visibility visibility) {
  const bool visible = visibility == content::Visibility::VISIBLE;
  if (visible) {
    LoadIfDiscarded();
  }
  Emit(BDK_CONTENTS_EVENT_VISIBILITY_CHANGED, "",
       ToJson(base::DictValue().Set("visible", visible)), 0, 0);
}

}  // namespace bdk
