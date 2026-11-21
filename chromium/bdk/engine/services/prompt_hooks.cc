// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/services/prompt_hooks.h"

#include <memory>

#include "base/functional/bind.h"
#include "bdk/engine/services/autofill_prompts.h"
#include "bdk/engine/services/password_prompts.h"
#include "bdk/engine/services/print_prompts.h"
#include "chrome/browser/printing/embedder_print_preview_hook.h"
#include "chrome/browser/ui/views/embedder_tab_dialogs_factory.h"

namespace bdk {

void InstallPromptHooks() {
  embedder_tab_dialogs_factory::SetFactory(base::BindRepeating(
      [](tabs::TabInterface& tab,
         content::WebContents* contents) -> std::unique_ptr<TabDialogs> {
        return std::make_unique<BdkTabDialogs>(tab, contents);
      }));
  embedder_print_preview_hook::SetHook(base::BindRepeating(&OnPrintRequested));
  InstallAutofillPopupHook();
}

}  // namespace bdk
