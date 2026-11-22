// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_EXTENSIONS_SIDE_PANEL_HOST_H_
#define BDK_ENGINE_EXTENSIONS_SIDE_PANEL_HOST_H_

#include <array>
#include <memory>
#include <optional>
#include <string>

#include "base/callback_list.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "chrome/browser/extensions/api/side_panel/side_panel_service.h"
#include "chrome/browser/ui/side_panel/side_panel_ui.h"
#include "extensions/common/extension_id.h"
#include "ui/base/unowned_user_data/scoped_unowned_user_data.h"

class BrowserWindowInterface;

namespace bdk {

class BdkBrowserWindow;
class SidePanelHost;

// What the engine reports of the extension side panel open in a window, for
// the test hook (extensions/panel_testing.cc).
struct SidePanelState {
  bool open = false;
  extensions::ExtensionId extension_id;
  std::string url;
  bool page_scoped = false;
  // Whether the panel is visible: a page-scoped panel is hidden while its page
  // is not the window's active page.
  bool visible = false;
  // Whether BDK's default presenter shows it, and where, in the window's
  // content view (points, origin top left); zero when it does not.
  bool default_presenter = false;
  std::array<double, 4> frame = {0, 0, 0, 0};
  // Whether the request for it is still open with the application.
  bool request_open = false;
  // Whether sidePanel.onOpened has been sent.
  bool opened_dispatched = false;
};

// Extension side panels for a host context (T-EXT-03, extensions.md 4.3).
// Chromium creates its side panel only for a Browser that has a BrowserView:
// `BrowserWindowFeatures` makes the SidePanelCoordinator, the
// ExtensionSidePanelManager and its views-based coordinators only then.
// `chrome.sidePanel.open` (and an action that opens the panel) ends at
// `SidePanelUI::From(browser)->Show(key)`, an abstract interface, so BDK
// supplies the implementation for its host contexts and needs no patch: the
// panel is an extension view like a popup, hosted in a BDK view.
//
// The controller is the context's SidePanelUI. It shows one extension panel
// per window at a time, as Chrome does. A panel is a request of the
// `extension_side_panels` surface that stays open while the view is on screen
// (conventions.md, "Extensions (M5)", item 3):
//  - no handler, or "use default": BDK's frame attaches the view to the
//    trailing edge of the page's window;
//  - a handler: the application places the secure view and tells BDK with a
//    {"placed":true,"side":...} update, and {"action":"close"} or a denial
//    closes the panel;
//  - the extension, another panel, the page or the window closing it
//    withdraws the request.
// A page-scoped panel stays alive while its page is not the window's active
// page and is told it is not visible ({"visible":false}); the page returning
// makes it visible again.
class SidePanelController : public SidePanelUI,
                            public extensions::SidePanelService::Observer {
 public:
  SidePanelController(BrowserWindowInterface* browser,
                      BdkBrowserWindow* window);
  ~SidePanelController() override;

  // SidePanelUI:
  using SidePanelUI::Close;
  using SidePanelUI::Show;
  void Show(SidePanelEntryId entry_id,
            std::optional<SidePanelOpenTrigger> open_trigger,
            bool suppress_animations) override;
  void Show(SidePanelEntryKey entry_key,
            std::optional<SidePanelOpenTrigger> open_trigger,
            bool suppress_animations) override;
  void ShowFrom(SidePanelEntryKey entry_key,
                gfx::Rect starting_bounds_in_browser_coordinates) override;
  void Close(SidePanelEntryHideReason hide_reason,
             bool suppress_animations) override;
  void Toggle(SidePanelEntryKey key,
              SidePanelOpenTrigger open_trigger) override;
  std::optional<SidePanelEntryId> GetCurrentEntryId() const override;
  int GetCurrentEntryDefaultContentWidth() const override;
  bool IsSidePanelShowing() const override;
  bool IsSidePanelEntryShowing(
      const SidePanelEntryKey& entry_key) const override;
  bool IsSidePanelEntryShowing(const SidePanelEntryKey& entry_key,
                               bool for_tab) const override;
  base::CallbackListSubscription RegisterSidePanelShown(
      ShownCallback callback) override;
  void OnActiveTabChanged(content::WebContents* old_contents,
                          content::WebContents* new_contents,
                          bool tab_removed_for_deletion) override;
  content::WebContents* GetWebContentsForTest(SidePanelEntryId id) override;
  void DisableAnimationsForTesting() override {}
  void SetNoDelaysForTesting(bool no_delays_for_testing) override {}

  // extensions::SidePanelService::Observer:
  void OnPanelOptionsChanged(const extensions::ExtensionId& extension_id,
                             const extensions::api::side_panel::PanelOptions&
                                 updated_options) override;
  void OnSidePanelServiceShutdown() override;

  // The state of the panel open in this context, for tests.
  SidePanelState State() const;
  // Resizes the default presenter's frame as dragging its divider does.
  // Returns false when the default presenter does not show the panel.
  bool DragDividerForTesting(int delta);

 private:
  friend class SidePanelHost;

  // Opens the panel of the extension for the active page, replacing the one
  // that is open.
  void ShowExtensionPanel(const extensions::ExtensionId& extension_id);
  // Closes the open panel: withdraws its request and takes its view down.
  void ClosePanel();
  // Closes the open panel if it is `panel` (a panel asks to be closed from
  // its own call stack, so the close comes later).
  void ClosePanelIf(base::WeakPtr<SidePanelHost> panel);
  BdkBrowserWindow* window() const { return window_; }
  BrowserWindowInterface* browser() const { return browser_; }

  raw_ptr<BrowserWindowInterface> browser_;
  raw_ptr<BdkBrowserWindow> window_;
  std::unique_ptr<SidePanelHost> panel_;
  base::RepeatingCallbackList<void()> shown_callbacks_;
  base::ScopedObservation<extensions::SidePanelService,
                          extensions::SidePanelService::Observer>
      service_observation_{this};
  std::optional<ui::ScopedUnownedUserData<SidePanelUI>> scoped_user_data_;
  base::WeakPtrFactory<SidePanelController> weak_factory_{this};
};

}  // namespace bdk

#endif  // BDK_ENGINE_EXTENSIONS_SIDE_PANEL_HOST_H_
