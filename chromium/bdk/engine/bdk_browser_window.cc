// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/bdk_browser_window.h"

#include <string_view>
#include <utility>

#include "base/functional/bind.h"
#include "base/notimplemented.h"
#include "base/observer_list.h"
#include "bdk/engine/extensions/side_panel_host.h"
#include "bdk/engine/host_window.h"
#include "bdk/engine/services/autofill_prompts.h"
#include "bdk/engine/window_registry.h"
#include "chrome/browser/themes/theme_service.h"
#include "chrome/browser/ui/autofill/autofill_bubble_handler.h"
#include "chrome/browser/ui/browser_active_state_manager/browser_active_state_manager.h"
#include "chrome/browser/ui/browser_command_controller.h"
#include "chrome/browser/ui/browser_window/public/browser_window_features.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/exclusive_access/exclusive_access_context.h"
#include "chrome/browser/ui/find_bar/find_bar.h"
#include "chrome/browser/ui/location_bar/location_bar.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/bubble_anchor_util_views.h"
#include "components/tabs/public/tab_interface.h"
#include "components/web_modal/web_contents_modal_dialog_host.h"
#include "content/public/browser/keyboard_event_processing_result.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/mojom/window_show_state.mojom.h"
#include "ui/color/color_provider_manager.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/range/range.h"
#include "ui/native_theme/native_theme.h"

namespace bdk {

namespace {

// Chromium's exclusive access (fullscreen, pointer lock) as far as a host
// context takes part in it. BDK asks the application for a page's fullscreen
// itself, as a UI request from the page's delegate; Chromium's own entry
// points raise the window request an extension's windows.update needs. What a
// service reads (is the window fullscreen, which tab is active) comes from the
// window.
class HostExclusiveAccessContext : public ExclusiveAccessContext {
 public:
  HostExclusiveAccessContext(BrowserWindowInterface* browser,
                             BdkBrowserWindow* window)
      : browser_(browser), window_(window) {}

  // ExclusiveAccessContext:
  Profile* GetProfile() override { return browser_->GetProfile(); }
  bool IsFullscreen() const override { return window_->IsFullscreen(); }
  // Only the extension windows API reaches these (a tab's fullscreen is the
  // page's own request): the application is asked for the window's.
  void EnterFullscreen(const url::Origin& origin,
                       ExclusiveAccessBubbleType bubble_type,
                       FullscreenTabParams fullscreen_tab_params) override {
    window_->RequestFullscreen(true);
  }
  void ExitFullscreen() override { window_->RequestFullscreen(false); }
  void UpdateExclusiveAccessBubble(
      const ExclusiveAccessBubbleParams& params,
      ExclusiveAccessBubbleHideCallback first_hide_callback) override {
    NOTIMPLEMENTED_LOG_ONCE();
  }
  bool IsExclusiveAccessBubbleDisplayed() const override { return false; }
  void OnExclusiveAccessUserInput() override {}
  content::WebContents* GetWebContentsForExclusiveAccess() override {
    tabs::TabInterface* tab = browser_->GetActiveTabInterface();
    return tab ? tab->GetContents() : nullptr;
  }
  bool CanUserEnterFullscreen() const override { return true; }
  bool CanUserExitFullscreen() const override { return true; }

 private:
  const raw_ptr<BrowserWindowInterface> browser_;
  const raw_ptr<BdkBrowserWindow> window_;
};

// Where tab-modal dialogs go. Nothing is positioned yet: the page's host view
// is the anchor a later change supplies (print preview needs it).
class HostModalDialogHost : public web_modal::WebContentsModalDialogHost {
 public:
  ~HostModalDialogHost() override {
    for (web_modal::ModalDialogHostObserver& observer : observers_) {
      observer.OnHostDestroying();
    }
  }

  // web_modal::WebContentsModalDialogHost:
  gfx::NativeView GetHostView() const override { return gfx::NativeView(); }
  gfx::Point GetDialogPosition(const gfx::Size& size) override {
    return gfx::Point();
  }
  gfx::Size GetMaximumDialogSize() override { return gfx::Size(); }
  void AddObserver(web_modal::ModalDialogHostObserver* observer) override {
    observers_.AddObserver(observer);
  }
  void RemoveObserver(web_modal::ModalDialogHostObserver* observer) override {
    observers_.RemoveObserver(observer);
  }

 private:
  base::ObserverList<web_modal::ModalDialogHostObserver> observers_;
};

// A find bar that shows nothing. Find UI becomes a UI request with the find
// service; until then a find session runs and its results are kept by the
// tab's find helper, which is what a service reads.
class HostFindBar : public FindBar {
 public:
  // FindBar:
  FindBarController* GetFindBarController() const override {
    return controller_;
  }
  void SetFindBarController(FindBarController* controller) override {
    controller_ = controller;
  }
  void Show(bool animate, bool focus) override { visible_ = true; }
  void Hide(bool animate) override { visible_ = false; }
  void SetFocusAndSelection() override {}
  void ClearResults(
      const find_in_page::FindNotificationDetails& results) override {}
  void StopAnimation() override {}
  void MoveWindowIfNecessary() override {}
  void SetFindTextAndSelectedRange(const std::u16string& find_text,
                                   const gfx::Range& selected_range) override {
    text_ = find_text;
  }
  std::u16string_view GetFindText() const override { return text_; }
  gfx::Range GetSelectedRange() const override { return gfx::Range(); }
  void UpdateUIForFindResult(
      const find_in_page::FindNotificationDetails& result,
      const std::u16string& find_text) override {}
  void AudibleAlert() override {}
  bool IsFindBarVisible() const override { return visible_; }
  void RestoreSavedFocus() override {}
  bool HasGlobalFindPasteboard() const override { return false; }
  void UpdateFindBarForChangedWebContents() override {}
  bool CanPopulateFromSelectedText() override { return false; }
  const FindBarTesting* GetFindBarTesting() const override { return nullptr; }
  bool HasFocus() const override { return false; }
  void CloseOverlappingBubbles() override {}
  views::Widget* GetHostWidget() override { return nullptr; }

 private:
  raw_ptr<FindBarController> controller_ = nullptr;
  std::u16string text_;
  bool visible_ = false;
};

}  // namespace

// The location bar Chromium expects every window to have (it saves state into
// it when a tab goes to the background). There is no address field and no
// omnibox: every query is answered with the stub's nothing. Permission prompts
// will anchor on a rectangle this object supplies once the permission service
// is hooked up; the page's host view is the anchor.
class HostLocationBar : public LocationBar {
 public:
  HostLocationBar(BrowserWindowInterface* browser, CommandUpdater* updater)
      : LocationBar(updater), browser_(browser) {}
  ~HostLocationBar() override = default;

  // LocationBar:
  void FocusLocation(bool is_user_initiated,
                     bool clear_focus_if_failed) override {}
  void FocusSearch() override {}
  void UpdateFocusBehavior(bool toolbar_visible) override {}
  void UpdateContentSettingsIcons() override {}
  void SaveStateToContents(content::WebContents* contents) override {}
  void Revert() override {}
  OmniboxView* GetOmniboxView() override { return nullptr; }
  OmniboxPopupView* GetOmniboxPopupView() override { return nullptr; }
  OmniboxController* GetOmniboxController() override { return nullptr; }
  bool ShouldCloseOmniboxPopup(ui::MouseEvent* event) override { return false; }
  content::WebContents* GetWebContents() override {
    return browser_->GetTabStripModel()->GetActiveWebContents();
  }
  LocationBarModel* GetLocationBarModel() override { return nullptr; }
  std::optional<bubble_anchor_util::AnchorConfiguration> GetChipAnchor()
      override {
    return std::nullopt;
  }
  ChipController* GetChipController() override { return nullptr; }
  void AnnounceAlert(const std::u16string& announcement) override {}
  void OnChanged() override {}
  void UpdateWithoutTabRestore() override {}
  ui::TrackedElement* GetAnchorOrNull() override { return nullptr; }
  BrowserWindowInterface* GetBrowser() override { return browser_; }
  Profile* GetProfile() override { return browser_->GetProfile(); }
  bool IsInitialized() const override { return true; }
  bool IsVisible() const override { return false; }
  bool IsDrawn() const override { return false; }
  bool IsFullscreen() const override { return false; }
  bool IsEditingOrEmpty() const override { return false; }
  bool IsMouseHovered() const override { return false; }
  bool IsFocusWithin() const override { return false; }
  void InvalidateLayout() override {}
  gfx::Rect Bounds() const override { return gfx::Rect(); }
  gfx::Rect BoundsInScreen() const override { return gfx::Rect(); }
  gfx::Size MinimumSize() const override { return gfx::Size(); }
  gfx::Size PreferredSize() const override { return gfx::Size(); }
  void Update(content::WebContents* contents) override {}
  void ResetTabState(content::WebContents* contents) override {}
  bool HasSecurityStateChanged() override { return false; }
  LocationBarTesting* GetLocationBarForTesting() override { return nullptr; }

 private:
  const raw_ptr<BrowserWindowInterface> browser_;
};

BdkBrowserWindow::BdkBrowserWindow(BrowserWindowInterface* browser)
    : browser_(browser),
      exclusive_access_context_(
          std::make_unique<HostExclusiveAccessContext>(browser, this)),
      modal_dialog_host_(std::make_unique<HostModalDialogHost>()) {}

BdkBrowserWindow::~BdkBrowserWindow() {
  // Closes an open panel while the Browser is whole.
  side_panel_controller_.reset();
  autofill_bubble_handler_.reset();
  // A window implementation tears the Browser's window-scoped features down
  // before it goes (WebUIBrowserWindow and BrowserWidget do the same).
  browser_->GetFeatures().TearDownPreBrowserWindowDestruction();
}

void BdkBrowserWindow::InstallAutofillBubbleHandler() {
  autofill_bubble_handler_ = CreateAutofillBubbleHandler(browser_);
}

void BdkBrowserWindow::InstallSidePanelUI() {
  side_panel_controller_ =
      std::make_unique<SidePanelController>(browser_, this);
}

void BdkBrowserWindow::SetHostWindow(std::unique_ptr<HostWindow> host_window) {
  host_window_ = std::move(host_window);
}

BdkBrowserWindow::ScopedTabOperation::ScopedTabOperation(
    BdkBrowserWindow* window) {
  if (window) {
    window_ = window->weak_factory_.GetWeakPtr();
    ++window->tab_operations_;
  }
}

BdkBrowserWindow::ScopedTabOperation::~ScopedTabOperation() {
  if (window_) {
    --window_->tab_operations_;
  }
}

void BdkBrowserWindow::RaiseRequest(
    base::DictValue request,
    base::OnceCallback<void(HostWindow&)> apply) {
  if (!registered_window_ || !host_window_) {
    return;
  }
  RaiseWindowRequest(
      registered_window_, std::move(request),
      base::BindOnce(
          [](base::WeakPtr<BdkBrowserWindow> self,
             base::OnceCallback<void(HostWindow&)> apply, bool use_default) {
            // The application handled or rejected it, or the context went.
            if (use_default && self && self->host_window_) {
              std::move(apply).Run(*self->host_window_);
            }
          },
          weak_factory_.GetWeakPtr(), std::move(apply)));
}

void BdkBrowserWindow::RequestFullscreen(bool fullscreen) {
  RaiseRequest(WindowRequest(fullscreen ? "enterFullscreen" : "exitFullscreen"),
               base::BindOnce(
                   [](bool fullscreen, HostWindow& window) {
                     window.SetFullscreen(fullscreen);
                   },
                   fullscreen));
}

void BdkBrowserWindow::OnActiveChanged(bool active) {
  if (active) {
    BrowserActiveStateManager::From(browser_)->DidBecomeActive();
  } else {
    BrowserActiveStateManager::From(browser_)->DidBecomeInactive();
  }
}

// BrowserWindow --------------------------------------------------------------

gfx::NativeWindow BdkBrowserWindow::GetNativeWindow() const {
  return host_window_ ? host_window_->NativeWindow() : gfx::NativeWindow();
}

bool BdkBrowserWindow::IsOnCurrentWorkspace() const {
  return host_window_ != nullptr;
}

bool BdkBrowserWindow::IsVisibleOnScreen() const {
  return host_window_ && host_window_->IsVisible();
}

void BdkBrowserWindow::SetTopControlsShownRatio(
    content::WebContents* web_contents,
    float ratio) {}

bool BdkBrowserWindow::DoBrowserControlsShrinkRendererSize(
    const content::WebContents* contents) const {
  return false;
}

ui::NativeTheme* BdkBrowserWindow::GetNativeTheme() {
  return ui::NativeTheme::GetInstanceForNativeUi();
}

const ui::ThemeProvider* BdkBrowserWindow::GetThemeProvider() const {
  return &ThemeService::GetThemeProviderForProfile(browser_->GetProfile());
}

const ui::ColorProvider* BdkBrowserWindow::GetColorProvider() const {
  return ui::ColorProviderManager::Get().GetColorProviderFor(
      ui::NativeTheme::GetInstanceForNativeUi()->GetColorProviderKey(nullptr));
}

int BdkBrowserWindow::GetTopControlsHeight() const {
  return 0;
}

void BdkBrowserWindow::SetTopControlsGestureScrollInProgress(bool in_progress) {
}

std::vector<StatusBubble*> BdkBrowserWindow::GetStatusBubbles() {
  return {};
}

void BdkBrowserWindow::UpdateTitleBar() {}

void BdkBrowserWindow::UpdateLoadingAnimations(bool is_visible) {}

void BdkBrowserWindow::OnActiveTabChanged(content::WebContents* old_contents,
                                          content::WebContents* new_contents,
                                          int index,
                                          int reason) {}

void BdkBrowserWindow::OnTabDetached(content::WebContents* contents,
                                     bool was_active) {}

gfx::Size BdkBrowserWindow::GetContentsSize() const {
  return host_window_ ? host_window_->Bounds().size() : gfx::Size();
}

void BdkBrowserWindow::SetContentsSize(const gfx::Size& size) {
  NOTIMPLEMENTED_LOG_ONCE();
}

autofill::AutofillBubbleHandler* BdkBrowserWindow::GetAutofillBubbleHandler() {
  return autofill_bubble_handler_.get();
}

LocationBar* BdkBrowserWindow::GetLocationBar() const {
  if (!location_bar_) {
    location_bar_ = std::make_unique<HostLocationBar>(
        browser_, chrome::BrowserCommandController::From(browser_));
  }
  return location_bar_.get();
}

ui::AcceleratorProvider* BdkBrowserWindow::GetAcceleratorProvider() {
  return nullptr;
}

PinnedToolbarActions* BdkBrowserWindow::GetPinnedToolbarActions() {
  return nullptr;
}

void BdkBrowserWindow::SetFocusToLocationBar(bool is_user_initiated) {}

void BdkBrowserWindow::UpdateReloadStopState(bool is_loading, bool force) {}

void BdkBrowserWindow::UpdateToolbar(content::WebContents* contents) {}

bool BdkBrowserWindow::UpdateToolbarSecurityState() {
  return false;
}

void BdkBrowserWindow::UpdateCustomTabBarVisibility(bool visible,
                                                    bool animate) {}

void BdkBrowserWindow::ResetToolbarTabState(content::WebContents* contents) {}

void BdkBrowserWindow::FocusToolbar() {}

void BdkBrowserWindow::ToolbarSizeChanged(bool is_animating) {}

void BdkBrowserWindow::TabDraggingStatusChanged(bool is_dragging) {}

void BdkBrowserWindow::LinkOpeningFromGesture(
    WindowOpenDisposition disposition) {}

void BdkBrowserWindow::FocusAppMenu() {}

bool BdkBrowserWindow::IsTabStripEditable() const {
  return true;
}

void BdkBrowserWindow::DisableTabStripEditingForTesting() {}

bool BdkBrowserWindow::IsToolbarVisible() const {
  return false;
}

bool BdkBrowserWindow::IsToolbarShowing() const {
  return false;
}

bool BdkBrowserWindow::IsLocationBarVisible() const {
  return false;
}

void BdkBrowserWindow::ShowUpdateChromeDialog() {
  NOTIMPLEMENTED_LOG_ONCE();
}

void BdkBrowserWindow::ShowIntentPickerBubble(
    std::vector<apps::IntentPickerAppInfo> app_info,
    bool show_stay_in_chrome,
    bool show_remember_selection,
    apps::IntentPickerBubbleType bubble_type,
    const std::optional<url::Origin>& initiating_origin,
    IntentPickerResponse callback) {
  NOTIMPLEMENTED_LOG_ONCE();
}

void BdkBrowserWindow::ShowBookmarkBubble(const GURL& url,
                                          bool already_bookmarked) {
  NOTIMPLEMENTED_LOG_ONCE();
}

ShowTranslateBubbleResult BdkBrowserWindow::ShowTranslateBubble(
    content::WebContents* contents,
    translate::TranslateStep step,
    const std::string& source_language,
    const std::string& target_language,
    translate::TranslateErrors error_type,
    bool is_user_gesture) {
  NOTIMPLEMENTED_LOG_ONCE();
  return ShowTranslateBubbleResult::kBrowserWindowNotValid;
}

DownloadBubbleUIController* BdkBrowserWindow::GetDownloadBubbleUIController() {
  NOTIMPLEMENTED_LOG_ONCE();
  return nullptr;
}

void BdkBrowserWindow::ConfirmBrowserCloseWithPendingDownloads(
    int download_count,
    UnloadController::DownloadCloseType dialog_type,
    base::OnceCallback<void(bool)> callback) {
  NOTIMPLEMENTED_LOG_ONCE();
  std::move(callback).Run(true);
}

void BdkBrowserWindow::ShowAppMenu() {}

void BdkBrowserWindow::PreHandleDragUpdate(const content::DropData& drop_data,
                                           const gfx::PointF& point) {}

void BdkBrowserWindow::PreHandleDragExit() {}

void BdkBrowserWindow::HandleDragEnded() {}

content::KeyboardEventProcessingResult BdkBrowserWindow::PreHandleKeyboardEvent(
    const input::NativeWebKeyboardEvent& event) {
  return content::KeyboardEventProcessingResult::NOT_HANDLED;
}

bool BdkBrowserWindow::HandleKeyboardEvent(
    const input::NativeWebKeyboardEvent& event) {
  return false;
}

std::unique_ptr<FindBar> BdkBrowserWindow::CreateFindBar() {
  return std::make_unique<HostFindBar>();
}

web_modal::WebContentsModalDialogHost*
BdkBrowserWindow::GetWebContentsModalDialogHost() {
  return modal_dialog_host_.get();
}

web_modal::WebContentsModalDialogHost*
BdkBrowserWindow::GetWebContentsModalDialogHostFor(
    content::WebContents* web_contents) {
  return modal_dialog_host_.get();
}

void BdkBrowserWindow::ShowAvatarBubbleFromAvatarButton(
    bool is_source_accelerator) {}

void BdkBrowserWindow::ShowHatsDialog(
    const std::string& site_id,
    const std::optional<std::string>& hats_histogram_name,
    const std::optional<uint64_t> hats_survey_ukm_id,
    base::OnceClosure success_callback,
    base::OnceClosure failure_callback,
    const SurveyBitsData& product_specific_bits_data,
    const SurveyStringData& product_specific_string_data) {
  std::move(failure_callback).Run();
}

ExclusiveAccessContext* BdkBrowserWindow::GetExclusiveAccessContext() {
  return exclusive_access_context_.get();
}

std::string BdkBrowserWindow::GetWorkspace() const {
  return std::string();
}

bool BdkBrowserWindow::IsVisibleOnAllWorkspaces() const {
  return false;
}

void BdkBrowserWindow::ShowEmojiPanel() {}

std::unique_ptr<content::EyeDropper> BdkBrowserWindow::OpenEyeDropper(
    content::RenderFrameHost* frame,
    content::EyeDropperListener* listener) {
  NOTIMPLEMENTED_LOG_ONCE();
  return nullptr;
}

void BdkBrowserWindow::ShowCaretBrowsingDialog() {
  NOTIMPLEMENTED_LOG_ONCE();
}

void BdkBrowserWindow::CreateTabSearchBubble() {}

void BdkBrowserWindow::CloseTabSearchBubble() {}

void BdkBrowserWindow::ShowIncognitoClearBrowsingDataDialog() {}

void BdkBrowserWindow::ShowIncognitoHistoryDisclaimerDialog() {}

bool BdkBrowserWindow::IsUnframedModeEnabled() const {
  return false;
}

bool BdkBrowserWindow::GetCanResize() {
  return true;
}

ui::mojom::WindowShowState BdkBrowserWindow::GetWindowShowState() const {
  if (IsMaximized()) {
    return ui::mojom::WindowShowState::kMaximized;
  }
  if (IsMinimized()) {
    return ui::mojom::WindowShowState::kMinimized;
  }
  if (IsFullscreen()) {
    return ui::mojom::WindowShowState::kFullscreen;
  }
  return ui::mojom::WindowShowState::kNormal;
}

void BdkBrowserWindow::ShowChromeLabs() {}

BrowserView* BdkBrowserWindow::AsBrowserView() {
  return nullptr;
}

// ui::BaseWindow -------------------------------------------------------------

bool BdkBrowserWindow::IsActive() const {
  return host_window_ && host_window_->IsActive();
}

bool BdkBrowserWindow::IsMaximized() const {
  return host_window_ && host_window_->IsMaximized();
}

bool BdkBrowserWindow::IsMinimized() const {
  return host_window_ && host_window_->IsMinimized();
}

bool BdkBrowserWindow::IsFullscreen() const {
  return host_window_ && host_window_->IsFullscreen();
}

gfx::Rect BdkBrowserWindow::GetRestoredBounds() const {
  return GetBounds();
}

ui::mojom::WindowShowState BdkBrowserWindow::GetRestoredState() const {
  return ui::mojom::WindowShowState::kNormal;
}

gfx::Rect BdkBrowserWindow::GetBounds() const {
  return host_window_ ? host_window_->Bounds() : gfx::Rect();
}

// The application owns its windows: Chromium cannot show, hide, close or
// resize one.
void BdkBrowserWindow::Show() {
  NOTIMPLEMENTED_LOG_ONCE();
}

void BdkBrowserWindow::Hide() {
  NOTIMPLEMENTED_LOG_ONCE();
}

bool BdkBrowserWindow::IsVisible() const {
  return IsVisibleOnScreen();
}

void BdkBrowserWindow::ShowInactive() {
  NOTIMPLEMENTED_LOG_ONCE();
}

// Chromium closes a window whose tab strip empties. A host context outlives
// its tabs: the engine closes it when its window goes, so that call, which the
// engine's own tab operations make (see ScopedTabOperation), does nothing. Any
// other close is an extension's windows.remove, and the application's window
// is the application's to close: a request.
void BdkBrowserWindow::Close() {
  if (tab_operations_ > 0) {
    return;
  }
  RaiseRequest(WindowRequest("close"),
               base::BindOnce([](HostWindow& window) { window.Close(); }));
}

// The window requests below are Chromium's windows.update (an extension) and
// window.moveTo and resizeTo (a page of a popup window, through
// EngineContents::SetContentsBounds). Each asks the application; with no
// handler, or when it answers "use default", BDK applies it to the native
// window.
void BdkBrowserWindow::Activate() {
  RaiseRequest(WindowRequest("activate"),
               base::BindOnce([](HostWindow& window) { window.Activate(); }));
}

void BdkBrowserWindow::Deactivate() {
  NOTIMPLEMENTED_LOG_ONCE();
}

void BdkBrowserWindow::Maximize() {
  // AppKit's zoom toggles, so a window that is zoomed already is left alone.
  if (IsMaximized()) {
    return;
  }
  RaiseRequest(WindowRequest("zoom"),
               base::BindOnce([](HostWindow& window) { window.ToggleZoom(); }));
}

void BdkBrowserWindow::Minimize() {
  RaiseRequest(WindowRequest("minimize"),
               base::BindOnce([](HostWindow& window) { window.Minimize(); }));
}

void BdkBrowserWindow::Restore() {
  if (IsMinimized()) {
    // Activating a minimised window restores it.
    Activate();
  } else if (IsMaximized()) {
    RaiseRequest(WindowRequest("zoom"), base::BindOnce([](HostWindow& window) {
                   window.ToggleZoom();
                 }));
  }
}

void BdkBrowserWindow::SetBounds(const gfx::Rect& bounds) {
  base::DictValue request = WindowRequest("setFrame");
  request.Set("frame", base::DictValue()
                           .Set("x", bounds.x())
                           .Set("y", bounds.y())
                           .Set("width", bounds.width())
                           .Set("height", bounds.height()));
  RaiseRequest(
      std::move(request),
      base::BindOnce(
          [](gfx::Rect bounds, HostWindow& window) { window.SetFrame(bounds); },
          bounds));
}

void BdkBrowserWindow::FlashFrame(bool flash) {
  NOTIMPLEMENTED_LOG_ONCE();
}

ui::ZOrderLevel BdkBrowserWindow::GetZOrderLevel() const {
  return ui::ZOrderLevel::kNormal;
}

void BdkBrowserWindow::SetZOrderLevel(ui::ZOrderLevel order) {}

void BdkBrowserWindow::DeleteBrowserWindow() {
  delete this;
}

}  // namespace bdk
