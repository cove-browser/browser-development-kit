// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/extensions/extensions_container.h"

#include <map>
#include <set>
#include <utility>

#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/task/sequenced_task_runner.h"
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/extensions/extension_menu.h"
#include "bdk/engine/extensions/extension_ui_platform.h"
#include "bdk/engine/extensions/extension_ui_util.h"
#include "bdk/engine/ui_request.h"
#include "chrome/browser/extensions/extension_keybinding_registry.h"
#include "chrome/browser/extensions/extension_view_host.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/tab_list/tab_list_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/extensions/extension_action_delegate.h"
#include "chrome/browser/ui/extensions/extension_action_view_model.h"
#include "chrome/browser/ui/toolbar/toolbar_actions_model.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/accelerators/accelerator.h"

namespace bdk {

namespace {

std::map<BrowserWindowInterface*, BdkExtensionsContainer*>& Containers() {
  static base::NoDestructor<
      std::map<BrowserWindowInterface*, BdkExtensionsContainer*>>
      containers;
  return *containers;
}

// The platform half of an action's view model: popups are BDK's, and nothing
// else is shown. The application owns keyboard shortcuts (they arrive as
// bdk_extension_command_perform), so commands are not registered, and BDK has
// no extensions menu that stays open.
class BdkActionDelegate : public ExtensionActionDelegate {
 public:
  BdkActionDelegate(BdkExtensionsContainer* container,
                    extensions::ExtensionId id)
      : container_(container), id_(std::move(id)) {}

  // ExtensionActionDelegate:
  void AttachToModel(ExtensionActionViewModel* model) override {}
  void DetachFromModel() override {}
  void RegisterCommand() override {}
  void UnregisterCommand() override {}
  bool IsShowingPopup() const override {
    ExtensionPopup* popup = container_->popup();
    return popup && !popup->is_closed() && popup->extension_id() == id_;
  }
  void HidePopup() override {
    if (IsShowingPopup()) {
      container_->HideActivePopup();
    }
  }
  gfx::NativeView GetPopupNativeViewForTesting() override {
    return gfx::NativeView();
  }
  void TriggerPopup(std::unique_ptr<extensions::ExtensionViewHost> host,
                    PopupShowAction show_action,
                    bool by_user,
                    ShowPopupCallback callback) override {
    // Inspecting the popup (kShowAndInspect) belongs to the action's context
    // menu, which BDK does not show.
    container_->ShowPopup(
        std::move(host),
        by_user ? PopupTrigger::kUserClick : PopupTrigger::kOpenPopupApi,
        std::move(callback));
  }
  void ShowContextMenuAsFallback() override {}
  void CloseExtensionsMenuIfOpen() override {}

 private:
  const raw_ptr<BdkExtensionsContainer> container_;
  const extensions::ExtensionId id_;
};

// The title-bar buttons of the registered windows. A window shows one button
// for all its contexts (one per profile it shows pages of); it is visible while
// any of them has an extension with an action.
class TitlebarButtons {
 public:
  static TitlebarButtons& Get() {
    static base::NoDestructor<TitlebarButtons> buttons;
    return *buttons;
  }

  // Makes sure the window has a button and counts `owner` among its users.
  // False when the window cannot take one.
  bool Acquire(bdk_native_handle_t native,
               const void* owner,
               base::RepeatingCallback<void(bdk_native_handle_t)> clicked) {
    Entry& entry = entries_[native.pointer];
    if (!entry.button) {
      entry.button = TitlebarMenuButton::Create(native, std::move(clicked));
      if (!entry.button) {
        entries_.erase(native.pointer);
        return false;
      }
      entry.button->SetVisible(false);
    }
    entry.owners.insert(owner);
    return true;
  }

  void SetVisibleFor(bdk_native_handle_t native,
                     const void* owner,
                     bool visible) {
    auto it = entries_.find(native.pointer);
    if (it == entries_.end()) {
      return;
    }
    if (visible) {
      it->second.visible_for.insert(owner);
    } else {
      it->second.visible_for.erase(owner);
    }
    it->second.button->SetVisible(!it->second.visible_for.empty());
  }

  void Release(bdk_native_handle_t native, const void* owner) {
    auto it = entries_.find(native.pointer);
    if (it == entries_.end()) {
      return;
    }
    it->second.owners.erase(owner);
    it->second.visible_for.erase(owner);
    if (it->second.owners.empty()) {
      entries_.erase(it);
    } else {
      it->second.button->SetVisible(!it->second.visible_for.empty());
    }
  }

  // For tests: the window's button, or null.
  TitlebarMenuButton* ButtonOf(bdk_native_handle_t native) {
    auto it = entries_.find(native.pointer);
    return it == entries_.end() ? nullptr : it->second.button.get();
  }

 private:
  friend class base::NoDestructor<TitlebarButtons>;
  TitlebarButtons() = default;
  ~TitlebarButtons() = default;

  struct Entry {
    std::unique_ptr<TitlebarMenuButton> button;
    std::set<const void*> owners;
    std::set<const void*> visible_for;
  };
  std::map<const void*, Entry> entries_;
};

}  // namespace

// Fires commands.onCommand as an extension's shortcut does, through
// Chromium's own keybinding registry: it grants activeTab for the window's
// active tab and sends the event with that tab. No accelerator is registered
// (the application owns the keys), so it only borrows the dispatch.
class CommandDispatcher : public extensions::ExtensionKeybindingRegistry {
 public:
  explicit CommandDispatcher(BrowserWindowInterface* browser)
      : extensions::ExtensionKeybindingRegistry(
            browser->GetProfile(),
            TabListInterface::From(browser),
            extensions::ExtensionKeybindingRegistry::ALL_EXTENSIONS) {}

  void Dispatch(const std::string& extension_id, const std::string& command) {
    CommandExecuted(extension_id, command, ui::Accelerator());
  }

 private:
  // extensions::ExtensionKeybindingRegistry:
  bool PopulateCommands(const extensions::Extension* extension,
                        ui::CommandMap* commands) override {
    return false;
  }
  bool RegisterAccelerator(const ui::Accelerator& accelerator,
                           const extensions::ExtensionId& extension_id,
                           const std::string& command_name) override {
    return false;
  }
};

TitlebarMenuButton* TitlebarButtonForTesting(bdk_native_handle_t native) {
  return TitlebarButtons::Get().ButtonOf(native);
}

// static
std::unique_ptr<BdkExtensionsContainer> BdkExtensionsContainer::Create(
    BrowserWindowInterface* browser,
    bdk_native_handle_t native) {
  std::shared_ptr<bdkcore::Runtime> rt = bdkcore::current_runtime();
  if (!rt ||
      (!bdkcore::ui_surface_available(*rt, BDK_SURFACE_EXTENSION_ACTIONS) &&
       !bdkcore::ui_surface_available(*rt, BDK_SURFACE_EXTENSION_POPUPS))) {
    return nullptr;
  }
  if (!ToolbarActionsModel::Get(browser->GetProfile())) {
    return nullptr;
  }
  return std::unique_ptr<BdkExtensionsContainer>(
      new BdkExtensionsContainer(browser, native));
}

// static
BdkExtensionsContainer* BdkExtensionsContainer::From(
    BrowserWindowInterface* browser) {
  auto it = Containers().find(browser);
  return it == Containers().end() ? nullptr : it->second;
}

BdkExtensionsContainer::BdkExtensionsContainer(BrowserWindowInterface* browser,
                                               bdk_native_handle_t native)
    : browser_(browser),
      native_(native),
      model_(std::make_unique<ExtensionsToolbarViewModel>(
          this,
          browser,
          ToolbarActionsModel::Get(browser->GetProfile()))),
      user_data_(browser->GetUnownedUserDataHost(), *model_) {
  Containers()[browser_] = this;
  model_observation_.Observe(model_.get());
  // The application may still be setting up the window (its anchor provider
  // comes after the registration): look at it later.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(&BdkExtensionsContainer::MaybeInstallTitlebarButton,
                     weak_factory_.GetWeakPtr()));
}

BdkExtensionsContainer::~BdkExtensionsContainer() {
  Containers().erase(browser_);
  if (has_titlebar_button_) {
    TitlebarButtons::Get().Release(native_, this);
  }
  // The popup and the view models go with the container.
  popup_.reset();
}

void BdkExtensionsContainer::ShowPopup(
    std::unique_ptr<extensions::ExtensionViewHost> host,
    PopupTrigger trigger,
    ShowPopupCallback shown) {
  tabs::TabInterface* tab = TabListInterface::From(browser_)->GetActiveTab();
  content::WebContents* contents = tab ? tab->GetContents() : nullptr;
  EngineContents* page =
      contents ? EngineContents::FromWebContents(contents) : nullptr;
  if (!page || !SurfaceAvailable(page->Core(), BDK_SURFACE_EXTENSION_POPUPS)) {
    if (shown) {
      std::move(shown).Run(nullptr);
    }
    return;
  }
  HideActivePopup();
  popup_ = std::make_unique<ExtensionPopup>(
      std::move(host), page->GetWeakPtr(), trigger, std::move(shown),
      base::BindOnce(&BdkExtensionsContainer::OnPopupClosed,
                     weak_factory_.GetWeakPtr()));
  popup_->Start();
}

void BdkExtensionsContainer::OnPopupClosed(ExtensionPopup* popup) {
  if (popup_.get() != popup) {
    return;
  }
  // The popup closes from inside its host's callbacks: it goes afterwards.
  base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(FROM_HERE,
                                                             std::move(popup_));
}

void BdkExtensionsContainer::DispatchCommand(const std::string& extension_id,
                                             const std::string& command) {
  if (!commands_) {
    commands_ = std::make_unique<CommandDispatcher>(browser_);
  }
  commands_->Dispatch(extension_id, command);
}

bool BdkExtensionsContainer::HasAnyActions() const {
  return model_->AreActionsInitialized() && !model_->GetAllActionIds().empty();
}

std::unique_ptr<ExtensionActionViewModel>
BdkExtensionsContainer::CreateActionViewModel(
    const ToolbarActionsModel::ActionId& action_id,
    ExtensionsContainer* extensions_container) {
  return ExtensionActionViewModel::Create(
      action_id, browser_,
      std::make_unique<BdkActionDelegate>(this, action_id));
}

void BdkExtensionsContainer::HideActivePopup() {
  if (popup_) {
    popup_->Close("hidden");
  }
}

void BdkExtensionsContainer::CloseExtensionsMenuIfOpen() {}

bool BdkExtensionsContainer::CanShowToolbarActionPopupForAPICall(
    const ToolbarActionsModel::ActionId& action_id) {
  // chrome.action.openPopup fails when popups are disabled or one is open.
  std::shared_ptr<bdkcore::Runtime> rt = bdkcore::current_runtime();
  return rt &&
         bdkcore::ui_surface_available(*rt, BDK_SURFACE_EXTENSION_POPUPS) &&
         !(popup_ && !popup_->is_closed());
}

void BdkExtensionsContainer::ToggleExtensionsMenu() {}

void BdkExtensionsContainer::OnActionsInitialized() {
  UpdateTitlebarButton();
}

void BdkExtensionsContainer::OnActionAdded(
    const ToolbarActionsModel::ActionId& action_id) {
  UpdateTitlebarButton();
}

void BdkExtensionsContainer::OnActionRemoved(
    const ToolbarActionsModel::ActionId& action_id) {
  UpdateTitlebarButton();
}

void BdkExtensionsContainer::OnActionUpdated(
    const ToolbarActionsModel::ActionId& action_id) {}

void BdkExtensionsContainer::OnPinnedActionsChanged() {}

void BdkExtensionsContainer::OnActiveWebContentsChanged(
    bool is_same_document,
    content::WebContents* web_contents) {
  if (!is_same_document) {
    HideActivePopup();
  }
}

void BdkExtensionsContainer::MaybeInstallTitlebarButton() {
  std::shared_ptr<bdkcore::Runtime> rt = bdkcore::current_runtime();
  if (!rt || rt->shutting || has_titlebar_button_ ||
      bdkcore::ui_surface_mode(*rt, BDK_SURFACE_EXTENSION_ACTIONS) !=
          bdkcore::UiMode::kDefault) {
    return;
  }
  std::shared_ptr<bdkcore::Window> window = RegisteredWindowOf(*rt, native_);
  // An application that registered an extensions anchor draws the entry point
  // itself.
  if (!window || window->anchor_cb || window->kind != BDK_WINDOW_KIND_NORMAL) {
    return;
  }
  has_titlebar_button_ = TitlebarButtons::Get().Acquire(
      native_, this,
      base::BindRepeating(
          [](bdk_native_handle_t native, bdk_native_handle_t button) {
            std::shared_ptr<bdkcore::Runtime> rt = bdkcore::current_runtime();
            if (!rt || rt->shutting) {
              return;
            }
            if (std::shared_ptr<bdkcore::Window> window =
                    RegisteredWindowOf(*rt, native)) {
              std::string error;
              PresentExtensionsMenu(*rt, window, button, &error);
            }
          },
          native_));
  UpdateTitlebarButton();
}

void BdkExtensionsContainer::UpdateTitlebarButton() {
  if (has_titlebar_button_) {
    TitlebarButtons::Get().SetVisibleFor(native_, this, HasAnyActions());
  }
}

}  // namespace bdk
