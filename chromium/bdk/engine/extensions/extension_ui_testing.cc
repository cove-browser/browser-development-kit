// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/extensions/extension_ui_testing.h"

#include <memory>
#include <string>
#include <utility>

#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "bdk/engine/bdk_browser_window.h"
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/extensions/extension_popup.h"
#include "bdk/engine/extensions/extension_ui_util.h"
#include "bdk/engine/extensions/extensions_container.h"
#include "bdk/engine/host_context.h"
#include "bdk/engine/host_view.h"
#include "chrome/browser/extensions/menu_manager.h"
#include "chrome/browser/profiles/profile.h"
#include "extensions/browser/permissions/active_tab_permission_granter.h"
#include "extensions/browser/unpacked_installer.h"
#include "extensions/common/extension.h"

namespace bdk {

namespace {

// The last fixture load: whether it finished, the extension's id or the error.
struct LoadState {
  bool done = false;
  std::string id;
  std::string error;
};

LoadState& Load() {
  static base::NoDestructor<LoadState> state;
  return *state;
}

EngineContents* PageOf(const base::DictValue& args) {
  const std::string* handle = args.FindString("contents");
  uint64_t value = 0;
  if (!handle || !base::StringToUint64(*handle, &value)) {
    return nullptr;
  }
  std::shared_ptr<bdkcore::Contents> c =
      bdkcore::contents_of(static_cast<bdk_contents_t>(value));
  return c ? EngineContents::From(*c) : nullptr;
}

BdkExtensionsContainer* ContainerOf(EngineContents& page) {
  HostContext* context = HostContexts::Get().ContextOf(page.web_contents());
  return context && context->browser_window()
             ? context->browser_window()->extensions_container()
             : nullptr;
}

}  // namespace

bool RunExtensionUiTestCommand(const std::string& name,
                               const base::DictValue& args,
                               base::DictValue& result) {
  EngineContents* page = PageOf(args);
  const std::string* path = args.FindString("path");
  const std::string* id = args.FindString("id");
  if (name == "extui.load") {
    // {"contents", "path"}: loads the unpacked extension in the page's
    // profile, as the unpacked installer does; poll extui.loaded.
    if (!page || !path) {
      return false;
    }
    Load() = LoadState();
    scoped_refptr<extensions::UnpackedInstaller> installer =
        extensions::UnpackedInstaller::Create(ProfileOf(*page->Core()));
    // A refusal is reported through the result, as in the install flow, not
    // in Chromium's modal alert, which blocks the process.
    installer->set_be_noisy_on_failure(false);
    installer->set_completion_callback(
        base::BindOnce([](const extensions::Extension* extension,
                          const base::FilePath&, const std::u16string& error) {
          LoadState& state = Load();
          state.done = true;
          if (extension) {
            state.id = extension->id();
          } else {
            state.error = base::UTF16ToUTF8(error);
          }
        }));
    installer->Load(base::FilePath::FromUTF8Unsafe(*path));
    return true;
  }
  if (name == "extui.loaded") {
    result.Set("done", Load().done);
    result.Set("id", Load().id);
    result.Set("error", Load().error);
    return true;
  }
  if (name == "extui.has_active_tab") {
    // {"contents", "id"}: whether activeTab is granted for the page.
    if (!page || !id) {
      return false;
    }
    const extensions::Extension* extension =
        EnabledExtension(ProfileOf(*page->Core()), *id);
    extensions::ActiveTabPermissionGranter* granter =
        extensions::ActiveTabPermissionGranter::FromWebContents(
            page->web_contents());
    result.Set("granted",
               extension && granter && granter->IsGranted(extension));
    return true;
  }
  if (name == "extui.context_items") {
    // {"contents", "id"}: how many contextMenus items the extension has.
    if (!page || !id) {
      return false;
    }
    extensions::MenuManager* manager =
        extensions::MenuManager::Get(ProfileOf(*page->Core()));
    const extensions::MenuItem::OwnedList* items =
        manager ? manager->MenuItems(extensions::MenuItem::ExtensionKey(*id))
                : nullptr;
    result.Set("count", items ? static_cast<int>(items->size()) : 0);
    return true;
  }
  if (name == "extui.popup") {
    // The popups so far: opened, shown, closed, why the last closed, the last
    // size, and whether the page's window has one open.
    const PopupRecord record = PopupRecordForTesting();
    result.Set("opened", record.opened);
    result.Set("shown", record.shown);
    result.Set("closed", record.closed);
    result.Set("lastCloseReason", record.last_close_reason);
    result.Set("lastSize", record.last_size);
    BdkExtensionsContainer* container = page ? ContainerOf(*page) : nullptr;
    result.Set("open", container && container->popup() &&
                           !container->popup()->is_closed());
    return true;
  }
  if (name == "extui.blur") {
    // {"contents"}: the popup of the page's window loses focus.
    BdkExtensionsContainer* container = page ? ContainerOf(*page) : nullptr;
    if (!container || !container->popup()) {
      return false;
    }
    container->popup()->OnBlur();
    return true;
  }
  if (name == "extui.titlebar") {
    // {"contents"}: the title-bar button of the page's window.
    if (!page) {
      return false;
    }
    TitlebarMenuButton* button =
        TitlebarButtonForTesting(page->host_view().Window());
    result.Set("present", button != nullptr);
    result.Set("visible", button && button->IsVisible());
    WindowRect frame = button ? button->FrameInWindow() : WindowRect{};
    result.Set("x", frame[0]);
    result.Set("y", frame[1]);
    result.Set("width", frame[2]);
    result.Set("height", frame[3]);
    return true;
  }
  return false;
}

}  // namespace bdk
