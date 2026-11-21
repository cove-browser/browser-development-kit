// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_EXTENSIONS_OPTIONS_PAGE_H_
#define BDK_ENGINE_EXTENSIONS_OPTIONS_PAGE_H_

#include <string>

#include "bdk/engine/services/services.h"

namespace bdk {

// Extension options pages (extensions.md section 4.5, T-EXT-04). An options
// page opens as a new-window request of kind newPage whose payload names the
// extension as its initiator, in the extension's profile. It is reached by
// the command `extensions.openOptions`, by chrome.runtime.openOptionsPage and
// by the options item of an action's menu; the last two end in Chromium's
// BrowserExtensionWindowController::OpenOptionsPage, where a BDK patch asks
// the hook installed here first (chromium/patches).
//
// `[extensions.ui] options_pages = "disabled"` refuses the command with
// extension.surfaceDisabled and fails chrome.runtime.openOptionsPage.

// Installs the hook Chromium asks before it opens an options page in a
// Browser. Called once when the engine starts.
void InstallExtensionOptionsPageHook();

// The command `extensions.openOptions`.
void AddOptionsPageHandlers(HandlerTable& table);

}  // namespace bdk

#endif  // BDK_ENGINE_EXTENSIONS_OPTIONS_PAGE_H_
