// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/extensions/side_panel_host.h"

#include <memory>
#include <string>
#include <utility>

#include "base/base64.h"
#include "base/check.h"
#include "base/functional/bind.h"
#include "base/i18n/rtl.h"
#include "base/memory/ref_counted_memory.h"
#include "base/scoped_observation.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/values.h"
#include "bdk/core/include/bdk/bdk_ids.h"
#include "bdk/core/runtime/core_internal.h"
#include "bdk/engine/bdk_browser_window.h"
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/extensions/side_panel_view.h"
#include "bdk/engine/host_view.h"
#include "bdk/engine/json_util.h"
#include "bdk/engine/testing.h"
#include "bdk/engine/ui_presenters.h"
#include "chrome/browser/extensions/extension_tab_util.h"
#include "chrome/browser/extensions/extension_view_host.h"
#include "chrome/browser/extensions/extension_view_host_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/side_panel/side_panel_entry_key.h"
#include "chrome/browser/ui/side_panel/side_panel_registry.h"
#include "chrome/common/extensions/api/side_panel.h"
#include "chrome/common/pref_names.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/browser/extension_registry_observer.h"
#include "extensions/browser/image_loader.h"
#include "extensions/common/constants.h"
#include "extensions/common/extension.h"
#include "extensions/common/manifest_handlers/icons_handler.h"
#include "ui/base/page_transition_types.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/image/image.h"
#include "url/gurl.h"

namespace bdk {

namespace {

// The surface's name in [ui.surfaces] terms, for the presenter records.
constexpr char kSurface[] = "extension_side_panels";

// Runs `task` later, counted as BDK work so a test pumps until it ran.
void PostEngineTask(base::OnceClosure task) {
  NoteTaskPosted();
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](base::OnceClosure task) {
                       NoteTaskRun();
                       std::move(task).Run();
                     },
                     std::move(task)));
}

// What the extension's panel path means: an http(s) URL as it is, anything
// else a path in the extension (as ExtensionSidePanelCoordinator reads it).
GURL PanelUrl(const extensions::Extension& extension, const std::string& path) {
  GURL absolute(path);
  return absolute.SchemeIsHTTPOrHTTPS() ? absolute
                                        : extension.ResolveExtensionURL(path);
}

bool OptionsEnable(const extensions::api::side_panel::PanelOptions& options) {
  return options.enabled.value_or(false) && options.path.has_value();
}

// The pref key the panel's width is remembered under (a dictionary Chromium's
// own side panel keeps in the profile, one width per entry).
std::string WidthKey(const extensions::ExtensionId& id) {
  return SidePanelEntryKey(SidePanelEntryId::kExtension, id).ToString();
}

}  // namespace

// One open extension side panel. Owned by the controller of the context whose
// window shows it. Its page is an ExtensionViewHost of type
// kExtensionSidePanel, shown in a SidePanelView that is this object's
// `ExtensionView`.
class SidePanelHost : public extensions::ExtensionView,
                      public SidePanelFrame::Client,
                      public extensions::ExtensionRegistryObserver,
                      public content::WebContentsObserver {
 public:
  SidePanelHost(SidePanelController* controller,
                const extensions::Extension& extension,
                const GURL& url,
                tabs::TabInterface* page_tab);
  ~SidePanelHost() override;

  // Creates the page and the view, and raises the request once the icon is
  // read.
  void Start();
  // Takes the panel down: withdraws the request, removes the view, sends
  // sidePanel.onClosed. The host does not outlive the call.
  void Close();

  const extensions::ExtensionId& extension_id() const { return extension_id_; }
  const GURL& url() const { return url_; }
  bool page_scoped() const { return page_scoped_; }
  content::WebContents* page() const { return page_contents_; }
  content::WebContents* contents() const {
    return host_ ? host_->host_contents() : nullptr;
  }
  bool visible() const { return visible_; }
  bool request_open() const { return request_ && !request_->finished; }
  bool opened_dispatched() const { return opened_dispatched_; }
  bool default_presenter() const { return frame_ != nullptr; }
  SidePanelFrame* frame() { return frame_.get(); }
  std::optional<int> tab_id() const { return tab_id_; }

  // Whether the panel is shown: a page-scoped panel is hidden while its page is
  // not the window's active page.
  void SetVisible(bool visible);
  // The extension changed the panel's page.
  void Navigate(const GURL& url);

  base::WeakPtr<SidePanelHost> GetWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }

  // extensions::ExtensionView:
  gfx::NativeView GetNativeView() override;
  void ResizeDueToAutoResize(content::WebContents* web_contents,
                             const gfx::Size& new_size) override {}
  void RenderFrameCreated(content::RenderFrameHost* frame) override {}
  bool HandleKeyboardEvent(content::WebContents* source,
                           const input::NativeWebKeyboardEvent& event) override;
  void OnLoaded() override;

  // SidePanelFrame::Client:
  void CloseClicked() override;
  void WidthChanged(int width) override;

  // extensions::ExtensionRegistryObserver:
  void OnExtensionUnloaded(content::BrowserContext* browser_context,
                           const extensions::Extension* extension,
                           extensions::UnloadedExtensionReason reason) override;
  void OnShutdown(extensions::ExtensionRegistry* registry) override;

  // content::WebContentsObserver, on the page a page-scoped panel is for:
  void WebContentsDestroyed() override;

 private:
  void OnIconLoaded(const gfx::Image& icon);
  void RaiseRequest();
  void OnRequestFinished(bdkcore::UiOutcome outcome,
                         const std::string& completion,
                         const std::string& args);
  void OnApplicationUpdate(const std::string& update_json);
  void ShowDefault();
  // Asks the controller to close this panel, later.
  void RequestClose();
  void SetSide(bool trailing);
  Profile* profile() const;

  raw_ptr<SidePanelController> controller_;
  const extensions::ExtensionId extension_id_;
  const std::string title_;
  GURL url_;
  const bool page_scoped_;
  // The page a page-scoped panel is for, which the panel closes with.
  base::WeakPtr<tabs::TabInterface> page_tab_;
  raw_ptr<content::WebContents> page_contents_ = nullptr;
  std::optional<int> tab_id_;
  bdk_contents_t page_handle_ = 0;
  bdk_profile_t profile_handle_ = 0;
  int window_id_ = 0;
  std::string icon_;
  std::unique_ptr<extensions::ExtensionViewHost> host_;
  std::unique_ptr<SidePanelView> view_;
  std::unique_ptr<SidePanelFrame> frame_;
  std::shared_ptr<bdkcore::Request> request_;
  base::CallbackListSubscription detach_subscription_;
  base::ScopedObservation<extensions::ExtensionRegistry,
                          extensions::ExtensionRegistryObserver>
      registry_observation_{this};
  bool visible_ = true;
  bool opened_dispatched_ = false;
  bool closing_ = false;
  base::WeakPtrFactory<SidePanelHost> weak_factory_{this};
};

SidePanelHost::SidePanelHost(SidePanelController* controller,
                             const extensions::Extension& extension,
                             const GURL& url,
                             tabs::TabInterface* page_tab)
    : controller_(controller),
      extension_id_(extension.id()),
      title_(extension.name()),
      url_(url),
      page_scoped_(page_tab != nullptr),
      page_tab_(page_tab ? page_tab->GetWeakPtr()
                         : base::WeakPtr<tabs::TabInterface>()) {
  window_id_ = extensions::ExtensionTabUtil::GetWindowId(controller->browser());
  tabs::TabInterface* tab =
      page_tab ? page_tab : controller->browser()->GetActiveTabInterface();
  CHECK(tab);
  EngineContents* page = EngineContents::FromWebContents(tab->GetContents());
  CHECK(page);
  page_handle_ = page->handle();
  if (std::shared_ptr<bdkcore::Contents> core = page->Core()) {
    profile_handle_ = core->profile ? core->profile->handle : 0;
  }
  if (page_tab) {
    page_contents_ = page_tab->GetContents();
    tab_id_ = extensions::ExtensionTabUtil::GetTabId(page_contents_);
    Observe(page_contents_);
    // The panel goes with the tab: a tab that leaves the window (a move) or
    // is destroyed takes its panel with it.
    detach_subscription_ = page_tab->RegisterWillDetach(base::BindRepeating(
        [](base::WeakPtr<SidePanelHost> self, tabs::TabInterface* tab,
           tabs::TabInterface::DetachReason reason) {
          if (self) {
            self->RequestClose();
          }
        },
        weak_factory_.GetWeakPtr()));
  }
  registry_observation_.Observe(
      extensions::ExtensionRegistry::Get(controller->browser()->GetProfile()));
}

// The host is gone by now: the controller closes a panel before it destroys
// it, and the host keeps pointing at this object until then.
SidePanelHost::~SidePanelHost() = default;

Profile* SidePanelHost::profile() const {
  return controller_->browser()->GetProfile();
}

void SidePanelHost::Start() {
  const extensions::Extension* extension =
      extensions::ExtensionRegistry::Get(profile())
          ->enabled_extensions()
          .GetByID(extension_id_);
  if (!extension) {
    RequestClose();
    return;
  }
  host_ = extensions::ExtensionViewHostFactory::CreateSidePanelHost(
      *extension, url_, page_tab_ ? nullptr : controller_->browser(),
      page_tab_.get());
  host_->set_view(this);
  // window.close() in the panel closes it, later (the host is mid-call).
  host_->SetCloseHandler(base::BindOnce(
      [](base::WeakPtr<SidePanelHost> self, extensions::ExtensionHost*) {
        if (self) {
          self->RequestClose();
        }
      },
      weak_factory_.GetWeakPtr()));
  view_ = SidePanelView::Create();
  view_->SetContents(host_->host_contents());
  host_->CreateRendererSoon();

  // The request carries the extension's icon as a data: URL.
  const extensions::ExtensionResource resource =
      extensions::IconsInfo::GetIconResource(
          extension, extension_misc::EXTENSION_ICON_SMALL,
          ExtensionIconSet::Match::kBigger);
  if (resource.empty()) {
    OnIconLoaded(gfx::Image());
    return;
  }
  extensions::ImageLoader::Get(profile())->LoadImageAsync(
      extension, resource,
      gfx::Size(extension_misc::EXTENSION_ICON_SMALL,
                extension_misc::EXTENSION_ICON_SMALL),
      base::BindOnce(&SidePanelHost::OnIconLoaded, weak_factory_.GetWeakPtr()));
}

void SidePanelHost::OnIconLoaded(const gfx::Image& icon) {
  if (closing_) {
    return;
  }
  if (!icon.IsEmpty()) {
    if (scoped_refptr<base::RefCountedMemory> png = icon.As1xPNGBytes()) {
      icon_ = "data:image/png;base64," +
              base::Base64Encode(base::span<const uint8_t>(*png));
    }
  }
  RaiseRequest();
}

void SidePanelHost::RaiseRequest() {
  std::shared_ptr<bdkcore::Runtime> rt = bdkcore::current_runtime();
  if (!rt || rt->shutting) {
    RequestClose();
    return;
  }
  base::DictValue payload;
  payload.Set("extensionId", extension_id_);
  payload.Set("title", title_);
  if (!icon_.empty()) {
    payload.Set("icon", icon_);
  }
  payload.Set("url", url_.spec());
  payload.Set("scope", page_scoped() ? "page" : "window");
  if (page_scoped()) {
    payload.Set("contents", base::NumberToString(page_handle_));
  }

  auto request = std::make_shared<bdkcore::Request>();
  request->surface = BDK_SURFACE_EXTENSION_SIDE_PANELS;
  // A page-scoped panel is the page's, and goes with it. A window-scoped one
  // belongs to no page.
  request->contents = page_scoped() ? page_handle_ : 0;
  request->profile = profile_handle_;
  request->window = controller_->window()->registered_window();
  request->payload = ToJson(payload);
  request->secure_view = view_->NativeHandle();
  base::WeakPtr<SidePanelHost> self = weak_factory_.GetWeakPtr();
  request->on_finish = [self](bdkcore::UiOutcome outcome,
                              const std::string& completion,
                              const std::string& args) {
    if (self) {
      self->OnRequestFinished(outcome, completion, args);
    }
  };
  request->on_update = [self](const std::string& update) {
    if (self) {
      self->OnApplicationUpdate(update);
    }
  };
  request_ = bdkcore::raise_ui_request(rt, std::move(request));
}

void SidePanelHost::OnRequestFinished(bdkcore::UiOutcome outcome,
                                      const std::string& completion,
                                      const std::string& args) {
  if (closing_) {
    return;
  }
  switch (outcome) {
    case bdkcore::UiOutcome::kDefaulted:
      // No handler, or it deferred: BDK's frame shows the panel.
      ShowDefault();
      return;
    case bdkcore::UiOutcome::kCompleted: {
      // The application's container closed it ({"action":"close"}), or it
      // refused this panel (deny). Anything else is the application saying
      // the view is placed and needs no request.
      std::optional<base::DictValue> parsed = ParseJsonDict(args);
      const std::string* action =
          parsed ? parsed->FindString("action") : nullptr;
      if (completion == "deny" || (action && *action == "close")) {
        RequestClose();
      }
      return;
    }
    case bdkcore::UiOutcome::kWithdrawn:
      // BDK closed it.
      return;
    case bdkcore::UiOutcome::kRejected:
    case bdkcore::UiOutcome::kCancelled:
    case bdkcore::UiOutcome::kDisabled:
    case bdkcore::UiOutcome::kNone:
      RequestClose();
      return;
  }
}

void SidePanelHost::OnApplicationUpdate(const std::string& update_json) {
  if (closing_) {
    return;
  }
  std::optional<base::DictValue> update = ParseJsonDict(update_json);
  if (!update) {
    return;
  }
  // {"placed":true,"side":"leading"|"trailing"} once the application put the
  // view in its container, and {"side":...} when it moves. What
  // sidePanel.getLayout() answers is the side.
  const std::string* side = update->FindString("side");
  if (side && (*side == "leading" || *side == "trailing")) {
    SetSide(*side == "trailing");
  } else if (update->FindBool("placed").value_or(false)) {
    SetSide(true);
  }
}

void SidePanelHost::SetSide(bool trailing) {
  // Chromium's pref says whether the panel is on the right; "trailing" is the
  // right edge unless the layout is right to left.
  const bool right = trailing != base::i18n::IsRTL();
  profile()->GetPrefs()->SetBoolean(prefs::kSidePanelHorizontalAlignment,
                                    right);
}

void SidePanelHost::ShowDefault() {
  if (frame_ || closing_) {
    return;
  }
  std::shared_ptr<bdkcore::Window> window =
      bdkcore::window_of(controller_->window()->registered_window());
  if (!window || !view_) {
    RequestClose();
    return;
  }
  SidePanelFrame::Params params;
  params.window = window->native;
  params.view = view_->NativeHandle();
  params.title = title_;
  params.icon_data_url = icon_;
  params.width = kSidePanelDefaultWidth;
  const base::DictValue& widths =
      profile()->GetPrefs()->GetDict(prefs::kSidePanelIdToWidth);
  if (std::optional<int> width = widths.FindInt(WidthKey(extension_id_))) {
    params.width = *width;
  }
  frame_ = SidePanelFrame::Create(params, this);
  if (!frame_) {
    // A window of a kind the platform cannot attach to.
    RequestClose();
    return;
  }
  frame_->SetVisible(visible_);
  // The default presenter is on the trailing edge.
  SetSide(true);
  base::DictValue payload;
  payload.Set("extensionId", extension_id_);
  payload.Set("title", title_);
  payload.Set("url", url_.spec());
  RecordPresented(kSurface, payload);
}

void SidePanelHost::SetVisible(bool visible) {
  if (visible_ == visible || closing_) {
    return;
  }
  visible_ = visible;
  if (frame_) {
    frame_->SetVisible(visible);
  }
  if (request_open()) {
    base::DictValue update;
    update.Set("visible", visible);
    if (std::shared_ptr<bdkcore::Runtime> rt = bdkcore::current_runtime()) {
      bdkcore::ui_send_update(rt, request_, ToJson(update));
    }
  }
}

void SidePanelHost::Navigate(const GURL& url) {
  if (url == url_ || !host_) {
    return;
  }
  url_ = url;
  if (url != host_->host_contents()->GetLastCommittedURL()) {
    host_->host_contents()->GetController().LoadURL(
        url, content::Referrer(), ui::PAGE_TRANSITION_AUTO_TOPLEVEL,
        std::string());
  }
}

void SidePanelHost::RequestClose() {
  if (closing_) {
    return;
  }
  PostEngineTask(base::BindOnce(&SidePanelController::ClosePanelIf,
                                controller_->weak_factory_.GetWeakPtr(),
                                weak_factory_.GetWeakPtr()));
}

void SidePanelHost::Close() {
  if (closing_) {
    return;
  }
  closing_ = true;
  // The request first: the application learns the view is going before it is.
  if (request_ && !request_->finished) {
    bdkcore::withdraw_ui_request(*request_);
  }
  request_.reset();
  frame_.reset();
  view_.reset();
  // A page-scoped panel is not reopened when its page is shown again.
  if (tabs::TabInterface* tab = page_tab_.get()) {
    if (SidePanelRegistry* registry = SidePanelRegistry::From(tab)) {
      std::optional<SidePanelEntry*> active = registry->GetActiveEntry();
      if (active &&
          (*active)->key() ==
              SidePanelEntryKey(SidePanelEntryId::kExtension, extension_id_)) {
        registry->ResetActiveEntry();
      }
    }
  }
  // sidePanel.onClosed, only for a panel that was opened.
  if (opened_dispatched_) {
    if (extensions::SidePanelService* service =
            extensions::SidePanelService::Get(profile())) {
      service->DispatchOnClosedEvent(extension_id_, window_id_, tab_id_,
                                     url_.path());
    }
    opened_dispatched_ = false;
  }
  // The host still points at this object, which lives past it.
  host_.reset();
}

gfx::NativeView SidePanelHost::GetNativeView() {
  return host_->host_contents()->GetNativeView();
}

bool SidePanelHost::HandleKeyboardEvent(
    content::WebContents* source,
    const input::NativeWebKeyboardEvent& event) {
  // A key the panel did not use goes to the application's menu, as a page's.
  return HostView::RedispatchUnhandledKey(event);
}

void SidePanelHost::OnLoaded() {
  // sidePanel.onOpened, once the panel's page has loaded.
  if (opened_dispatched_ || closing_) {
    return;
  }
  if (extensions::SidePanelService* service =
          extensions::SidePanelService::Get(profile())) {
    service->DispatchOnOpenedEvent(extension_id_, window_id_, tab_id_,
                                   url_.path());
    opened_dispatched_ = true;
  }
}

void SidePanelHost::CloseClicked() {
  RequestClose();
}

void SidePanelHost::WidthChanged(int width) {
  // The person resized the panel: it opens at that width from now on.
  ScopedDictPrefUpdate update(profile()->GetPrefs(),
                              prefs::kSidePanelIdToWidth);
  update->Set(WidthKey(extension_id_), width);
}

void SidePanelHost::OnExtensionUnloaded(
    content::BrowserContext* browser_context,
    const extensions::Extension* extension,
    extensions::UnloadedExtensionReason reason) {
  if (extension->id() == extension_id_) {
    RequestClose();
  }
}

void SidePanelHost::OnShutdown(extensions::ExtensionRegistry* registry) {
  registry_observation_.Reset();
  RequestClose();
}

void SidePanelHost::WebContentsDestroyed() {
  // The page is going: its panel cannot outlive it, and the page's delegate
  // holds the page's address.
  Observe(nullptr);
  page_contents_ = nullptr;
  RequestClose();
}

SidePanelController::SidePanelController(BrowserWindowInterface* browser,
                                         BdkBrowserWindow* window)
    : browser_(browser), window_(window) {
  if (extensions::SidePanelService* service =
          extensions::SidePanelService::Get(browser->GetProfile())) {
    service_observation_.Observe(service);
  }
  // The side panel UI of the Browser, found through its user data.
  scoped_user_data_.emplace(browser->GetUnownedUserDataHost(), *this);
}

SidePanelController::~SidePanelController() {
  ClosePanel();
}

void SidePanelController::Show(SidePanelEntryId entry_id,
                               std::optional<SidePanelOpenTrigger> open_trigger,
                               bool suppress_animations) {
  // Only extensions have panels in a host context.
}

void SidePanelController::Show(SidePanelEntryKey entry_key,
                               std::optional<SidePanelOpenTrigger> open_trigger,
                               bool suppress_animations) {
  if (entry_key.id() == SidePanelEntryId::kExtension &&
      entry_key.extension_id()) {
    ShowExtensionPanel(*entry_key.extension_id());
  }
}

void SidePanelController::ShowFrom(
    SidePanelEntryKey entry_key,
    gfx::Rect starting_bounds_in_browser_coordinates) {
  Show(entry_key, std::nullopt, /*suppress_animations=*/false);
}

void SidePanelController::Close(SidePanelEntryHideReason hide_reason,
                                bool suppress_animations) {
  ClosePanel();
}

void SidePanelController::Toggle(SidePanelEntryKey key,
                                 SidePanelOpenTrigger open_trigger) {
  if (IsSidePanelEntryShowing(key)) {
    ClosePanel();
  } else {
    Show(key, open_trigger, /*suppress_animations=*/false);
  }
}

std::optional<SidePanelEntryId> SidePanelController::GetCurrentEntryId() const {
  return panel_ ? std::make_optional(SidePanelEntryId::kExtension)
                : std::nullopt;
}

int SidePanelController::GetCurrentEntryDefaultContentWidth() const {
  return kSidePanelDefaultWidth;
}

bool SidePanelController::IsSidePanelShowing() const {
  return panel_ && panel_->visible();
}

bool SidePanelController::IsSidePanelEntryShowing(
    const SidePanelEntryKey& entry_key) const {
  return IsSidePanelShowing() &&
         entry_key.id() == SidePanelEntryId::kExtension &&
         entry_key.extension_id() == panel_->extension_id();
}

bool SidePanelController::IsSidePanelEntryShowing(
    const SidePanelEntryKey& entry_key,
    bool for_tab) const {
  return IsSidePanelEntryShowing(entry_key) && panel_->page_scoped() == for_tab;
}

base::CallbackListSubscription SidePanelController::RegisterSidePanelShown(
    ShownCallback callback) {
  return shown_callbacks_.Add(std::move(callback));
}

content::WebContents* SidePanelController::GetWebContentsForTest(
    SidePanelEntryId id) {
  return panel_ ? panel_->contents() : nullptr;
}

void SidePanelController::ShowExtensionPanel(
    const extensions::ExtensionId& extension_id) {
  std::shared_ptr<bdkcore::Runtime> rt = bdkcore::current_runtime();
  // [extensions.ui] side_panels = "disabled" (or extensions disabled): no panel
  // is created. A context with no native window (parking) has nowhere to put
  // one.
  if (!rt || rt->shutting ||
      !bdkcore::ui_surface_available(*rt, BDK_SURFACE_EXTENSION_SIDE_PANELS) ||
      !window_->registered_window() || !window_->host_window()) {
    return;
  }
  Profile* profile = browser_->GetProfile();
  const extensions::Extension* extension =
      extensions::ExtensionRegistry::Get(profile)->enabled_extensions().GetByID(
          extension_id);
  extensions::SidePanelService* service =
      extensions::SidePanelService::Get(profile);
  tabs::TabInterface* active = browser_->GetActiveTabInterface();
  if (!extension || !service || !active ||
      !EngineContents::FromWebContents(active->GetContents())) {
    return;
  }
  const int tab_id =
      extensions::ExtensionTabUtil::GetTabId(active->GetContents());

  // As Chromium decides which panel a Show() means: the active page's own
  // panel when the extension set options for it, else the window's.
  const extensions::api::side_panel::PanelOptions page_options =
      service->GetSpecificOptionsForTab(*extension, tab_id);
  const bool page_scoped = OptionsEnable(page_options);
  const extensions::api::side_panel::PanelOptions options =
      page_scoped ? page_options.Clone()
                  : service->GetOptions(*extension, std::nullopt);
  if (!OptionsEnable(options)) {
    return;
  }
  const GURL url = PanelUrl(*extension, *options.path);

  if (panel_ && panel_->extension_id() == extension_id &&
      panel_->page_scoped() == page_scoped &&
      (!page_scoped || panel_->tab_id() == tab_id)) {
    // Already open for this: show it again, at the extension's current page.
    panel_->Navigate(url);
    panel_->SetVisible(true);
    return;
  }
  // One extension panel is open per window at a time: this closes the other.
  ClosePanel();
  panel_ = std::make_unique<SidePanelHost>(this, *extension, url,
                                           page_scoped ? active : nullptr);
  panel_->Start();
  shown_callbacks_.Notify();
}

void SidePanelController::ClosePanel() {
  if (!panel_) {
    return;
  }
  std::unique_ptr<SidePanelHost> panel = std::move(panel_);
  panel->Close();
}

void SidePanelController::ClosePanelIf(base::WeakPtr<SidePanelHost> panel) {
  if (panel && panel_.get() == panel.get()) {
    ClosePanel();
  }
}

void SidePanelController::OnActiveTabChanged(content::WebContents* old_contents,
                                             content::WebContents* new_contents,
                                             bool tab_removed_for_deletion) {
  if (panel_ && panel_->page_scoped()) {
    // A page-scoped panel is shown only while its page is the window's active
    // page.
    panel_->SetVisible(new_contents == panel_->page());
  }
  // A page whose own panel was opened while it was in the background shows it
  // now: Chromium marks that panel active in the page's side panel registry.
  if (!new_contents || tab_removed_for_deletion) {
    return;
  }
  tabs::TabInterface* tab =
      tabs::TabInterface::MaybeGetFromContents(new_contents);
  SidePanelRegistry* registry = tab ? SidePanelRegistry::From(tab) : nullptr;
  std::optional<SidePanelEntry*> active =
      registry ? registry->GetActiveEntry() : std::nullopt;
  if (active && (*active)->key().id() == SidePanelEntryId::kExtension &&
      (*active)->key().extension_id() &&
      !(panel_ && panel_->page() == new_contents)) {
    ShowExtensionPanel(*(*active)->key().extension_id());
  }
}

void SidePanelController::OnPanelOptionsChanged(
    const extensions::ExtensionId& extension_id,
    const extensions::api::side_panel::PanelOptions& updated_options) {
  // Options for another extension, or for a tab or window the open panel is
  // not for, change nothing here (the panel of another page opens when that
  // page's panel is shown).
  if (!panel_ || panel_->extension_id() != extension_id ||
      updated_options.tab_id != panel_->tab_id()) {
    return;
  }
  if (updated_options.enabled.has_value() && !*updated_options.enabled) {
    ClosePanel();
    return;
  }
  if (updated_options.path) {
    const extensions::Extension* extension =
        extensions::ExtensionRegistry::Get(browser_->GetProfile())
            ->enabled_extensions()
            .GetByID(extension_id);
    if (extension) {
      panel_->Navigate(PanelUrl(*extension, *updated_options.path));
    }
  }
}

void SidePanelController::OnSidePanelServiceShutdown() {
  service_observation_.Reset();
  ClosePanel();
}

SidePanelState SidePanelController::State() const {
  SidePanelState state;
  if (!panel_) {
    return state;
  }
  state.open = true;
  state.extension_id = panel_->extension_id();
  state.url = panel_->url().spec();
  state.page_scoped = panel_->page_scoped();
  state.visible = panel_->visible();
  state.default_presenter = panel_->default_presenter();
  if (panel_->default_presenter()) {
    state.frame = panel_->frame()->FrameInWindow();
  }
  state.request_open = panel_->request_open();
  state.opened_dispatched = panel_->opened_dispatched();
  return state;
}

bool SidePanelController::DragDividerForTesting(int delta) {
  if (!panel_ || !panel_->frame()) {
    return false;
  }
  panel_->frame()->DragDivider(delta);
  return true;
}

}  // namespace bdk
