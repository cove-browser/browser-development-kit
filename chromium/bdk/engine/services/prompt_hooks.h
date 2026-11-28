// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_SERVICES_PROMPT_HOOKS_H_
#define BDK_ENGINE_SERVICES_PROMPT_HOOKS_H_

namespace bdk {

// Installs the hooks that turn Chromium's prompts into BDK UI requests (the
// password bubble, the print preview, ...): the factories BDK's patches add to
// Chromium. Called once, when the engine starts and before the first page
// exists.
// `phones`: [services.webauthn] phones of the runtime that starts Chromium.
void InstallPromptHooks(bool phones);

}  // namespace bdk

#endif  // BDK_ENGINE_SERVICES_PROMPT_HOOKS_H_
