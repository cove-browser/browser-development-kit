// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_EXTENSIONS_EXTENSIONS_CONTAINER_H_
#define BDK_ENGINE_EXTENSIONS_EXTENSIONS_CONTAINER_H_

#include <memory>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "bdk/core/include/bdk/bdk_base.h"
#include "bdk/engine/extensions/extension_popup.h"
#include "bdk/engine/extensions/extension_ui_platform.h"
#include "chrome/browser/ui/extensions/extensions_container.h"
#include "chrome/browser/ui/extensions/extensions_toolbar_view_model.h"
#include "ui/base/unowned_user_data/scoped_unowned_user_data.h"

class BrowserWindowInterface;

namespace extensions {
class ExtensionViewHost;
}  // namespace extensions

namespace bdk {

class CommandDispatcher;

// The `ExtensionsContainer` of a host context's window (extensions.md section
// 4.2, T-EXT-02): what Chromium's non-UI code asks when it needs the
// extensions UI of a window. It is Chromium's cross-platform
// `ExtensionsToolbarViewModel` (the model Android's toolbar is built on) with
// a delegate that shows popups as BDK's own (extension_popup.h) and no toolbar
// of its own, so:
//
// * `chrome.action.openPopup` finds a container for the window, which makes
//   `ExtensionActionViewModel::TriggerPopupForAPI` ask this class for a popup.
// * Activating an action shows its popup here (action_service.cc).
// * At most one popup is open per window. A new one, another page becoming the
//   active one and `HideActivePopup` close it; a navigation of the page it was
//   opened for closes it from the popup itself.
//
// It also owns what only a window that shows extensions needs: the object that
// fires `commands.onCommand` for a page, and the default title-bar button when
// the application registered no extensions anchor (extensions.md section 4.7:
// with no anchor the menu is reached from an accessory button at the trailing
// edge of the window's title bar, shown while the profile has an extension
// with an action).
//
// Owned by the window's `BdkBrowserWindow`, which makes it once the Browser
// exists and destroys it before the Browser's window features go.
class BdkExtensionsContainer : public ExtensionsToolbarViewModel::Delegate,
                               public ExtensionsToolbarViewModel::Observer {
 public:
  // Null when the window cannot show extensions: no toolbar model for the
  // profile, or the configuration disables both actions and popups. `native`
  // is the registered window the context belongs to.
  static std::unique_ptr<BdkExtensionsContainer> Create(
      BrowserWindowInterface* browser,
      bdk_native_handle_t native);

  ~BdkExtensionsContainer() override;

  // The container of `browser`'s window, or null.
  static BdkExtensionsContainer* From(BrowserWindowInterface* browser);

  BrowserWindowInterface* browser() const { return browser_; }
  ExtensionsToolbarViewModel& model() { return *model_; }

  // Shows `host` as the popup of the window's active page, closing the popup
  // that is open. `shown` runs with the host when it is on screen, or with
  // null when it closed first or could not be shown.
  void ShowPopup(std::unique_ptr<extensions::ExtensionViewHost> host,
                 PopupTrigger trigger,
                 ShowPopupCallback shown);
  ExtensionPopup* popup() const { return popup_.get(); }

  // Fires commands.onCommand for the window's active page, granting activeTab
  // for it first, as the command's shortcut would.
  void DispatchCommand(const std::string& extension_id,
                       const std::string& command);

  // Whether the window's profile has an extension with an action.
  bool HasAnyActions() const;

  // ExtensionsToolbarViewModel::Delegate:
  std::unique_ptr<ExtensionActionViewModel> CreateActionViewModel(
      const ToolbarActionsModel::ActionId& action_id,
      ExtensionsContainer* extensions_container) override;
  void HideActivePopup() override;
  void CloseExtensionsMenuIfOpen() override;
  bool CanShowToolbarActionPopupForAPICall(
      const ToolbarActionsModel::ActionId& action_id) override;
  void ToggleExtensionsMenu() override;

  // ExtensionsToolbarViewModel::Observer:
  void OnActionsInitialized() override;
  void OnActionAdded(const ToolbarActionsModel::ActionId& action_id) override;
  void OnActionRemoved(const ToolbarActionsModel::ActionId& action_id) override;
  void OnActionUpdated(const ToolbarActionsModel::ActionId& action_id) override;
  void OnPinnedActionsChanged() override;
  void OnActiveWebContentsChanged(bool is_same_document,
                                  content::WebContents* web_contents) override;

 private:
  BdkExtensionsContainer(BrowserWindowInterface* browser,
                         bdk_native_handle_t native);

  // Puts the title-bar button on the window when the application registered no
  // extensions anchor and actions are in default mode.
  void MaybeInstallTitlebarButton();
  void UpdateTitlebarButton();
  void OnPopupClosed(ExtensionPopup* popup);

  const raw_ptr<BrowserWindowInterface> browser_;
  const bdk_native_handle_t native_;
  std::unique_ptr<ExtensionsToolbarViewModel> model_;
  ui::ScopedUnownedUserData<ExtensionsContainer> user_data_;
  base::ScopedObservation<ExtensionsToolbarViewModel,
                          ExtensionsToolbarViewModel::Observer>
      model_observation_{this};
  std::unique_ptr<ExtensionPopup> popup_;
  std::unique_ptr<CommandDispatcher> commands_;
  bool has_titlebar_button_ = false;
  base::WeakPtrFactory<BdkExtensionsContainer> weak_factory_{this};
};

// The title-bar button of the registered window `native`, or null when it has
// none (the application registered an anchor, or the window has no title bar).
TitlebarMenuButton* TitlebarButtonForTesting(bdk_native_handle_t native);

}  // namespace bdk

#endif  // BDK_ENGINE_EXTENSIONS_EXTENSIONS_CONTAINER_H_
