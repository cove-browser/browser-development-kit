// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/services/prompt_hooks.h"

#include <memory>

#include "base/functional/bind.h"
#include "bdk/engine/extensions/install_prompts.h"
#include "bdk/engine/extensions/options_page.h"
#include "bdk/engine/services/autofill_prompts.h"
#include "bdk/engine/services/password_prompts.h"
#include "bdk/engine/services/print_prompts.h"
#include "bdk/engine/ui_browser_dialogs.h"
#include "bdk/engine/ui_webauthn.h"
#include "chrome/browser/printing/embedder_print_preview_hook.h"
#include "chrome/browser/ui/views/embedder_tab_dialogs_factory.h"
#include "components/permissions/embedder_prompt_visibility.h"

namespace bdk {

void InstallPromptHooks(bool phones) {
  embedder_tab_dialogs_factory::SetFactory(base::BindRepeating(
      [](tabs::TabInterface& tab,
         content::WebContents* contents) -> std::unique_ptr<TabDialogs> {
        return std::make_unique<BdkTabDialogs>(tab, contents);
      }));
  embedder_print_preview_hook::SetHook(base::BindRepeating(&OnPrintRequested));
  InstallAutofillPopupHook();
  InstallExtensionPromptHooks();
  InstallExtensionOptionsPageHook();
  InstallWebAuthnDialogHook(phones);
  InstallBrowserDialogHooks();
  InstallCredentialManagerPrompts();
  // An application may show several pages of one window side by side; each
  // one on screen gets its permission prompt, not only the window's active tab
  // (T-ADP-06, risk R2).
  permissions::embedder_prompt_visibility::SetFollowsVisibility(true);
}

}  // namespace bdk
