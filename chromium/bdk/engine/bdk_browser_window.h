// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_BDK_BROWSER_WINDOW_H_
#define BDK_ENGINE_BDK_BROWSER_WINDOW_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "bdk/core/include/bdk/bdk_base.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/unload_controller.h"
#include "ui/gfx/geometry/rect.h"

class BrowserWindowInterface;
class ExclusiveAccessContext;
class LocationBar;

namespace web_modal {
class WebContentsModalDialogHost;
}  // namespace web_modal

namespace bdk {

class BdkExtensionsContainer;
class HostLocationBar;
class HostWindow;
class SidePanelController;

// Chromium's `BrowserWindow` for a host context (architecture.md section
// 4.2): the window of a `Browser` that holds BDK's pages as tabs. It has no
// views and no toolbar. Where the context belongs to a registered window, the
// window's identity, bounds and activation come from the application's native
// window; the parking context has none. Modelled on upstream's
// `WebUIBrowserWindow`: `AsBrowserView()` is null and most calls do nothing.
// Every call that does nothing and that a service may one day need logs once
// (NOTIMPLEMENTED_LOG_ONCE), so a test run shows which ones were reached.
class BdkBrowserWindow : public BrowserWindow {
 public:
  explicit BdkBrowserWindow(BrowserWindowInterface* browser);
  ~BdkBrowserWindow() override;

  // Gives the window the application's native window, or none (parking).
  void SetHostWindow(std::unique_ptr<HostWindow> host_window);
  HostWindow* host_window() { return host_window_.get(); }
  // The registered window (bdk_window.h) this context belongs to, which the
  // requests of the window's methods are raised for. 0 for parking.
  void SetRegisteredWindow(bdk_window_t window) { registered_window_ = window; }
  bdk_window_t registered_window() const { return registered_window_; }

  // Chromium's `Browser::TabStripEmpty` calls `Close()` whenever the tab strip
  // empties, which happens at every move out of a context. The engine makes
  // every tab operation of its own inside one of these, so that call is not
  // the window request an extension's windows.remove is.
  class ScopedTabOperation {
   public:
    // `window` may be null.
    explicit ScopedTabOperation(BdkBrowserWindow* window);
    ~ScopedTabOperation();
    ScopedTabOperation(const ScopedTabOperation&) = delete;
    ScopedTabOperation& operator=(const ScopedTabOperation&) = delete;

   private:
    base::WeakPtr<BdkBrowserWindow> window_;
  };

  // Asks the application to enter or leave fullscreen with the window (the
  // extension windows API reaches Chromium's exclusive access, which calls
  // the context's EnterFullscreen and ExitFullscreen).
  void RequestFullscreen(bool fullscreen);

  // Gives the window the extension side panel UI of its Browser (what
  // `SidePanelUI::From()` returns). Once the Browser exists.
  void InstallSidePanelUI();
  SidePanelController* side_panel_controller() {
    return side_panel_controller_.get();
  }

  // The native window became or stopped being the key window.
  void OnActiveChanged(bool active);

  // Gives the window the handler of the autofill bubbles (what
  // `GetAutofillBubbleHandler()` returns, and what the card bubbles find in
  // the Browser's user data). Once the Browser exists: the handler registers
  // itself with it.
  void InstallAutofillBubbleHandler();

  // Gives the window of a registered native window its extensions container
  // (extensions/extensions_container.h): the object Chromium asks for popups,
  // so chrome.action.openPopup works. Once the Browser exists. Not made when
  // the configuration disables actions and popups.
  void InstallExtensionsContainer(bdk_native_handle_t native);
  BdkExtensionsContainer* extensions_container() const {
    return extensions_container_.get();
  }

  // BrowserWindow:
  gfx::NativeWindow GetNativeWindow() const override;
  bool IsOnCurrentWorkspace() const override;
  bool IsVisibleOnScreen() const override;
  void SetTopControlsShownRatio(content::WebContents* web_contents,
                                float ratio) override;
  bool DoBrowserControlsShrinkRendererSize(
      const content::WebContents* contents) const override;
  ui::NativeTheme* GetNativeTheme() override;
  const ui::ThemeProvider* GetThemeProvider() const override;
  const ui::ColorProvider* GetColorProvider() const override;
  int GetTopControlsHeight() const override;
  void SetTopControlsGestureScrollInProgress(bool in_progress) override;
  std::vector<StatusBubble*> GetStatusBubbles() override;
  void UpdateTitleBar() override;
  void UpdateLoadingAnimations(bool is_visible) override;
  void OnActiveTabChanged(content::WebContents* old_contents,
                          content::WebContents* new_contents,
                          int index,
                          int reason) override;
  void OnTabDetached(content::WebContents* contents, bool was_active) override;
  gfx::Size GetContentsSize() const override;
  void SetContentsSize(const gfx::Size& size) override;
  autofill::AutofillBubbleHandler* GetAutofillBubbleHandler() override;
  LocationBar* GetLocationBar() const override;
  ui::AcceleratorProvider* GetAcceleratorProvider() override;
  PinnedToolbarActions* GetPinnedToolbarActions() override;
  void SetFocusToLocationBar(bool is_user_initiated) override;
  void UpdateReloadStopState(bool is_loading, bool force) override;
  void UpdateToolbar(content::WebContents* contents) override;
  bool UpdateToolbarSecurityState() override;
  void UpdateCustomTabBarVisibility(bool visible, bool animate) override;
  void ResetToolbarTabState(content::WebContents* contents) override;
  void FocusToolbar() override;
  void ToolbarSizeChanged(bool is_animating) override;
  void TabDraggingStatusChanged(bool is_dragging) override;
  void LinkOpeningFromGesture(WindowOpenDisposition disposition) override;
  void FocusAppMenu() override;
  bool IsTabStripEditable() const override;
  void DisableTabStripEditingForTesting() override;
  bool IsToolbarVisible() const override;
  bool IsToolbarShowing() const override;
  bool IsLocationBarVisible() const override;
  void ShowUpdateChromeDialog() override;
  void ShowIntentPickerBubble(
      std::vector<apps::IntentPickerAppInfo> app_info,
      bool show_stay_in_chrome,
      bool show_remember_selection,
      apps::IntentPickerBubbleType bubble_type,
      const std::optional<url::Origin>& initiating_origin,
      IntentPickerResponse callback) override;
  void ShowBookmarkBubble(const GURL& url, bool already_bookmarked) override;
  ShowTranslateBubbleResult ShowTranslateBubble(
      content::WebContents* contents,
      translate::TranslateStep step,
      const std::string& source_language,
      const std::string& target_language,
      translate::TranslateErrors error_type,
      bool is_user_gesture) override;
  DownloadBubbleUIController* GetDownloadBubbleUIController() override;
  void ConfirmBrowserCloseWithPendingDownloads(
      int download_count,
      UnloadController::DownloadCloseType dialog_type,
      base::OnceCallback<void(bool)> callback) override;
  void ShowAppMenu() override;
  void PreHandleDragUpdate(const content::DropData& drop_data,
                           const gfx::PointF& point) override;
  void PreHandleDragExit() override;
  void HandleDragEnded() override;
  content::KeyboardEventProcessingResult PreHandleKeyboardEvent(
      const input::NativeWebKeyboardEvent& event) override;
  bool HandleKeyboardEvent(const input::NativeWebKeyboardEvent& event) override;
  std::unique_ptr<::FindBar> CreateFindBar() override;
  web_modal::WebContentsModalDialogHost* GetWebContentsModalDialogHost()
      override;
  web_modal::WebContentsModalDialogHost* GetWebContentsModalDialogHostFor(
      content::WebContents* web_contents) override;
  void ShowAvatarBubbleFromAvatarButton(bool is_source_accelerator) override;
  void ShowHatsDialog(
      const std::string& site_id,
      const std::optional<std::string>& hats_histogram_name,
      const std::optional<uint64_t> hats_survey_ukm_id,
      base::OnceClosure success_callback,
      base::OnceClosure failure_callback,
      const SurveyBitsData& product_specific_bits_data,
      const SurveyStringData& product_specific_string_data) override;
  ExclusiveAccessContext* GetExclusiveAccessContext() override;
  std::string GetWorkspace() const override;
  bool IsVisibleOnAllWorkspaces() const override;
  void ShowEmojiPanel() override;
  std::unique_ptr<content::EyeDropper> OpenEyeDropper(
      content::RenderFrameHost* frame,
      content::EyeDropperListener* listener) override;
  void ShowCaretBrowsingDialog() override;
  void CreateTabSearchBubble() override;
  void CloseTabSearchBubble() override;
  void ShowIncognitoClearBrowsingDataDialog() override;
  void ShowIncognitoHistoryDisclaimerDialog() override;
  bool IsUnframedModeEnabled() const override;
  bool GetCanResize() override;
  ui::mojom::WindowShowState GetWindowShowState() const override;
  void ShowChromeLabs() override;
  BrowserView* AsBrowserView() override;

  // ui::BaseWindow:
  bool IsActive() const override;
  bool IsMaximized() const override;
  bool IsMinimized() const override;
  bool IsFullscreen() const override;
  gfx::Rect GetRestoredBounds() const override;
  ui::mojom::WindowShowState GetRestoredState() const override;
  gfx::Rect GetBounds() const override;
  void Show() override;
  void Hide() override;
  bool IsVisible() const override;
  void ShowInactive() override;
  void Close() override;
  void Activate() override;
  void Deactivate() override;
  void Maximize() override;
  void Minimize() override;
  void Restore() override;
  void SetBounds(const gfx::Rect& bounds) override;
  void FlashFrame(bool flash) override;
  ui::ZOrderLevel GetZOrderLevel() const override;
  void SetZOrderLevel(ui::ZOrderLevel order) override;

 protected:
  // BrowserWindow:
  void DeleteBrowserWindow() override;

 private:
  // Raises `request` (bdk_window.h) for the registered window and, when the
  // application leaves it to BDK, runs `apply` on the native window later. A
  // parking context has no window to ask for.
  void RaiseRequest(base::DictValue request,
                    base::OnceCallback<void(HostWindow&)> apply);

  const raw_ptr<BrowserWindowInterface> browser_;
  std::unique_ptr<HostWindow> host_window_;
  bdk_window_t registered_window_ = 0;
  int tab_operations_ = 0;
  std::unique_ptr<autofill::AutofillBubbleHandler> autofill_bubble_handler_;
  std::unique_ptr<BdkExtensionsContainer> extensions_container_;
  mutable std::unique_ptr<HostLocationBar> location_bar_;
  std::unique_ptr<ExclusiveAccessContext> exclusive_access_context_;
  std::unique_ptr<web_modal::WebContentsModalDialogHost> modal_dialog_host_;
  std::unique_ptr<SidePanelController> side_panel_controller_;
  // Must be the last member.
  base::WeakPtrFactory<BdkBrowserWindow> weak_factory_{this};
};

}  // namespace bdk

#endif  // BDK_ENGINE_BDK_BROWSER_WINDOW_H_
