// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_EXTENSIONS_EXTENSION_MENU_H_
#define BDK_ENGINE_EXTENSIONS_EXTENSION_MENU_H_

#include <memory>
#include <string>

#include "bdk/core/include/bdk/bdk_base.h"
#include "bdk/core/runtime/core_internal.h"

namespace bdk {

class EngineContents;

// The default extensions menu (extensions.md section 4.7): every action of the
// profile of the window's focused page, with icon, title and badge, and a last
// line "Manage Extensions". Choosing an action activates it for that page (its
// popup is hosted as any popup is); the last line opens chrome://extensions as
// a new-window request of kind newPage (the M4 branded route is not wired yet).
//
// The menu is shown later, from a task (a menu tracks events in a loop of its
// own, which must not run inside a call of the application). `anchor_view` is
// the native view to open under (the title-bar button), or empty to use the
// window's extensions anchor, or the top trailing corner of its content.
// Returns false, with `error` set, when the window shows no page.
//
// A test scripts the menu with present.script on the surface "extension_menu":
// {"choose": "<extension id>" | "manage"}; the items it was given are in
// present.record's payload.
bool PresentExtensionsMenu(bdkcore::Runtime& rt,
                           const std::shared_ptr<bdkcore::Window>& window,
                           bdk_native_handle_t anchor_view,
                           std::string* error);

// The page of `window` the user is looking at: the focused one, else the
// active tab of a context of the window. Null when it shows none.
EngineContents* FocusedPageOf(const bdkcore::Window& window);

// Asks the application for a new page showing chrome://extensions in `page`'s
// profile.
void OpenManageExtensions(EngineContents& page);

}  // namespace bdk

#endif  // BDK_ENGINE_EXTENSIONS_EXTENSION_MENU_H_
