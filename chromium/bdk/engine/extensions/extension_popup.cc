// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/extensions/extension_popup.h"

#include <algorithm>
#include <optional>
#include <utility>

#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/extensions/extension_ui_util.h"
#include "bdk/engine/host_view.h"
#include "bdk/engine/json_util.h"
#include "chrome/browser/extensions/extension_view_host.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "extensions/common/extension.h"

namespace bdk {

namespace {

// How long the request waits for the first size the page asks for, once it has
// loaded, before it is raised with the default size.
constexpr base::TimeDelta kFirstSizeWait = base::Milliseconds(150);

PopupRecord& Record() {
  static base::NoDestructor<PopupRecord> record;
  return *record;
}

const char* TriggerName(PopupTrigger trigger) {
  switch (trigger) {
    case PopupTrigger::kUserClick:
      return "userClick";
    case PopupTrigger::kOpenPopupApi:
      return "openPopupAPI";
    case PopupTrigger::kCommand:
      return "command";
  }
  return "userClick";
}

gfx::Size Clamp(const gfx::Size& size) {
  return gfx::Size(
      std::clamp(size.width(), extensions::kExtensionPopupMinSize.width(),
                 extensions::kExtensionPopupMaxSize.width()),
      std::clamp(size.height(), extensions::kExtensionPopupMinSize.height(),
                 extensions::kExtensionPopupMaxSize.height()));
}

}  // namespace

PopupRecord PopupRecordForTesting() {
  return Record();
}

// Watches the page the popup was opened for: when it navigates, is hidden or
// goes, the popup has nothing to be a popup of.
class ExtensionPopup::OpenerObserver : public content::WebContentsObserver {
 public:
  OpenerObserver(content::WebContents* contents, ExtensionPopup* popup)
      : content::WebContentsObserver(contents), popup_(popup) {}

  void Stop() { Observe(nullptr); }

  // content::WebContentsObserver:
  void DidFinishNavigation(content::NavigationHandle* handle) override {
    if (handle->IsInPrimaryMainFrame() && handle->HasCommitted() &&
        !handle->IsSameDocument()) {
      popup_->Close("navigated");
    }
  }
  void OnVisibilityChanged(content::Visibility visibility) override {
    if (visibility == content::Visibility::HIDDEN) {
      popup_->Close("hidden");
    }
  }
  void WebContentsDestroyed() override { popup_->Close("pageGone"); }

 private:
  const raw_ptr<ExtensionPopup> popup_;
};

ExtensionPopup::ExtensionPopup(
    std::unique_ptr<extensions::ExtensionViewHost> host,
    base::WeakPtr<EngineContents> opener,
    PopupTrigger trigger,
    ShowPopupCallback shown,
    ClosedCallback closed)
    : extension_id_(host->extension_id()),
      trigger_(trigger),
      opener_(std::move(opener)),
      shown_callback_(std::move(shown)),
      closed_callback_(std::move(closed)),
      size_(extensions::kExtensionPopupMinSize),
      host_(std::move(host)) {
  ++Record().opened;
}

ExtensionPopup::~ExtensionPopup() {
  // Whoever owns the popup is destroying it: nobody is left to tell.
  closed_callback_.Reset();
  Close("destroyed");
  // The host goes while the popup, its view, is whole: it calls back as it
  // goes. The native view outlives it.
  host_.reset();
}

void ExtensionPopup::Start() {
  content::WebContents* contents = host_->host_contents();
  native_ = PopupNative::Create(contents);
  host_->set_view(this);
  Observe(contents);
  registry_observation_.Observe(
      extensions::ExtensionRegistry::Get(host_->browser_context()));
  if (EngineContents* page = opener()) {
    opener_observer_ =
        std::make_unique<OpenerObserver>(page->web_contents(), this);
  }
  // window.close() and Escape close the host through this handler (the host
  // asserts that one is set before it can be asked to close). Chromium may
  // destroy the host right after it runs, so the popup goes later.
  host_->SetCloseHandler(base::BindOnce(
      [](base::WeakPtr<ExtensionPopup> self, extensions::ExtensionHost*) {
        if (self) {
          self->Close("windowClose");
        }
      },
      weak_factory_.GetWeakPtr()));
  host_->CreateRendererSoon();
  // A host that had loaded already would not tell us.
  if (host_->has_loaded_once()) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&ExtensionPopup::OnLoaded, weak_factory_.GetWeakPtr()));
  }
}

EngineContents* ExtensionPopup::opener() const {
  return opener_.get();
}

void ExtensionPopup::Close(const char* reason) {
  if (closed_) {
    return;
  }
  closed_ = true;
  ++Record().closed;
  Record().last_close_reason = reason;
  if (request_) {
    // The application is told the view is gone (a withdrawal); the request
    // ends here, and OnRequestEnded ignores it.
    std::shared_ptr<bdkcore::Request> request = std::move(request_);
    if (!request->finished) {
      bdkcore::withdraw_ui_request(*request);
    }
  }
  if (scripted_dismiss_) {
    std::move(scripted_dismiss_).Run();
  }
  if (native_) {
    native_->ClosePanel();
  }
  if (shown_callback_) {
    std::move(shown_callback_).Run(nullptr);
  }
  if (opener_observer_) {
    opener_observer_->Stop();
  }
  Observe(nullptr);
  registry_observation_.Reset();
  if (closed_callback_) {
    std::move(closed_callback_).Run(this);
  }
}

void ExtensionPopup::OnBlur() {
  if (closed_ || !shown_ || IsInspected()) {
    return;
  }
  Close("blur");
}

bool ExtensionPopup::IsInspected() const {
  content::WebContents* contents = host_ ? host_->host_contents() : nullptr;
  return contents && content::DevToolsAgentHost::HasFor(contents) &&
         content::DevToolsAgentHost::GetOrCreateFor(contents)->IsAttached();
}

// extensions::ExtensionView:

gfx::NativeView ExtensionPopup::GetNativeView() {
  return host_->host_contents()->GetNativeView();
}

void ExtensionPopup::ResizeDueToAutoResize(content::WebContents* web_contents,
                                           const gfx::Size& new_size) {
  if (closed_) {
    return;
  }
  const gfx::Size size = Clamp(new_size);
  if (size_known_ && size == size_) {
    return;
  }
  size_ = size;
  size_known_ = true;
  Record().last_size = SizeJson();
  native_->SetSize(size_.width(), size_.height());
  if (request_ && !request_->finished) {
    if (std::shared_ptr<bdkcore::Runtime> rt = bdkcore::current_runtime()) {
      bdkcore::ui_send_update(rt, request_,
                              "{\"preferredSize\":" + SizeJson() + "}");
    }
  }
  if (loaded_ && !requested_) {
    RaiseRequest();
  }
}

void ExtensionPopup::RenderFrameCreated(
    content::RenderFrameHost* render_frame_host) {
  // The popup sizes itself within Chromium's bounds: the renderer lays the
  // page out and reports the size it wants (ResizeDueToAutoResize).
  if (closed_ ||
      render_frame_host != host_->host_contents()->GetPrimaryMainFrame()) {
    return;
  }
  if (content::RenderWidgetHostView* view = render_frame_host->GetView()) {
    view->EnableAutoResize(extensions::kExtensionPopupMinSize,
                           extensions::kExtensionPopupMaxSize);
  }
}

bool ExtensionPopup::HandleKeyboardEvent(
    content::WebContents* source,
    const input::NativeWebKeyboardEvent& event) {
  // Keys the page did not take are not the application's: nothing here
  // redispatches them to its main menu.
  return false;
}

void ExtensionPopup::OnLoaded() {
  if (closed_ || loaded_) {
    return;
  }
  loaded_ = true;
  if (size_known_) {
    RaiseRequest();
    return;
  }
  base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&ExtensionPopup::RaiseRequest, weak_factory_.GetWeakPtr()),
      kFirstSizeWait);
}

// The request:

std::string ExtensionPopup::SizeJson() const {
  return ToJson(base::DictValue()
                    .Set("width", size_.width())
                    .Set("height", size_.height()));
}

void ExtensionPopup::RaiseRequest() {
  if (closed_ || requested_) {
    return;
  }
  requested_ = true;
  EngineContents* page = opener();
  std::shared_ptr<bdkcore::Runtime> rt = bdkcore::current_runtime();
  if (!page || !rt || !host_->extension()) {
    Close("unavailable");
    return;
  }
  payload_.Set("extensionId", extension_id_);
  payload_.Set("extensionName", host_->extension()->name());
  payload_.Set("url", host_->GetLastCommittedURL().spec());
  payload_.Set("trigger", TriggerName(trigger_));
  payload_.Set("preferredSize", base::DictValue()
                                    .Set("width", size_.width())
                                    .Set("height", size_.height()));

  OpenRequest open;
  open.secure_view = native_->SecureView();
  if (std::shared_ptr<bdkcore::Window> window =
          RegisteredWindowOf(*rt, page->host_view().Window())) {
    open.window = window->handle;
  }
  open.on_update = base::BindRepeating(&ExtensionPopup::OnApplicationUpdate,
                                       weak_factory_.GetWeakPtr());
  request_ =
      RaiseUiRequest(page->Core(), BDK_SURFACE_EXTENSION_POPUPS, payload_,
                     base::BindOnce(&ExtensionPopup::OnRequestEnded,
                                    weak_factory_.GetWeakPtr()),
                     /*anchor_view=*/nullptr, std::nullopt, std::move(open));
  if (!request_) {
    Close("unavailable");
  }
}

void ExtensionPopup::OnRequestEnded(const UiResult& result) {
  request_.reset();
  if (closed_) {
    return;  // The popup ended the request itself.
  }
  switch (result.outcome) {
    case bdkcore::UiOutcome::kDefaulted:
      // No handler, or `useDefault`: BDK shows the view.
      ShowDefault();
      return;
    case bdkcore::UiOutcome::kCompleted:
      // The application closed its container (accept {"action":"close"}).
      Close("application");
      return;
    case bdkcore::UiOutcome::kWithdrawn:
      Close("withdrawn");
      return;
    default:
      // Denied, dropped or the surface disabled: the popup does not show.
      Close("rejected");
      return;
  }
}

void ExtensionPopup::OnApplicationUpdate(const std::string& update_json) {
  if (closed_) {
    return;
  }
  std::optional<base::DictValue> update = ParseJsonDict(update_json);
  if (update && update->FindBool("placed").value_or(false)) {
    MarkShown(/*focus=*/true);
  }
}

void ExtensionPopup::ShowDefault() {
  if (closed_) {
    return;
  }
  base::DictValue payload = payload_.Clone();
  if (std::optional<Dismisser> dismiss =
          ScriptedPresent("extension_popups", payload,
                          base::BindOnce(&ExtensionPopup::OnScriptedAnswer,
                                         weak_factory_.GetWeakPtr()))) {
    // A test scripted the presenter: nothing is shown.
    scripted_dismiss_ = std::move(*dismiss);
    MarkShown(/*focus=*/false);
    return;
  }
  RecordPresented("extension_popups", payload);
  std::shared_ptr<bdkcore::Runtime> rt = bdkcore::current_runtime();
  EngineContents* page = opener();
  bdk_native_handle_t window =
      page ? page->host_view().Window() : bdk_native_handle_t{};
  std::optional<WindowRect> anchor;
  if (rt) {
    if (std::shared_ptr<bdkcore::Window> registered =
            RegisteredWindowOf(*rt, window)) {
      WindowRect rect;
      if (bdkcore::window_anchor(*registered, BDK_ANCHOR_EXTENSIONS,
                                 rect.data())) {
        anchor = rect;
      }
    }
  }
  native_->ShowInPanel(
      window, anchor,
      base::BindOnce(&ExtensionPopup::OnBlur, weak_factory_.GetWeakPtr()));
  MarkShown(/*focus=*/true);
}

void ExtensionPopup::OnScriptedAnswer(const base::DictValue& answer) {
  const std::string* action = answer.FindString("action");
  if (!action) {
    return;
  }
  if (*action == "blur") {
    OnBlur();
  } else if (*action == "close") {
    Close("script");
  }
}

void ExtensionPopup::MarkShown(bool focus) {
  if (closed_ || shown_) {
    return;
  }
  shown_ = true;
  ++Record().shown;
  if (focus) {
    host_->host_contents()->Focus();
  }
  if (shown_callback_) {
    std::move(shown_callback_).Run(host_.get());
  }
}

// content::WebContentsObserver:

void ExtensionPopup::OnWebContentsFocused(
    content::RenderWidgetHost* render_widget_host) {
  focused_once_ = true;
}

void ExtensionPopup::OnWebContentsLostFocus(
    content::RenderWidgetHost* render_widget_host) {
  // An application's container has no panel to tell us it stopped being key:
  // the page losing the focus it had is the same signal.
  if (focused_once_) {
    OnBlur();
  }
}

// extensions::ExtensionRegistryObserver:

void ExtensionPopup::OnExtensionUnloaded(
    content::BrowserContext* browser_context,
    const extensions::Extension* extension,
    extensions::UnloadedExtensionReason reason) {
  if (extension->id() == extension_id_) {
    Close("unloaded");
  }
}

}  // namespace bdk
