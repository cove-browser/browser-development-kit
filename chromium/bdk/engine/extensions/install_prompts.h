// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_EXTENSIONS_INSTALL_PROMPTS_H_
#define BDK_ENGINE_EXTENSIONS_INSTALL_PROMPTS_H_

#include <memory>
#include <optional>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "bdk/engine/extensions/extension_info.h"
#include "chrome/browser/extensions/extension_install_prompt.h"
#include "extensions/browser/extension_install_prompt_client.h"
#include "extensions/browser/install_prompt_data.h"

class Profile;

namespace extensions {
class Extension;
class PermissionSet;
}  // namespace extensions

namespace bdk {

class EngineContents;

// Install, permission and uninstall prompts as UI requests (extensions.md
// 4.6, conventions.md "Extensions (M5)" item 4). Chromium's own prompts (the
// install flow, chrome.permissions.request, the store's inline install, a
// re-enable) reach BDK through the hooks the patch adds; the installs BDK
// starts itself use the same request through its own prompt client.
//
// Every request is answered by `grant` (which needs the gesture, as on every
// surface) or `deny`. Anything else, a dropped request, and the default
// presenter with no window to ask in, end it as a refusal: nothing installs
// silently, and there is no way to pre-approve.

// What a flow learns from the prompt it started.
struct PromptTrace {
  // Set by the flow: raise no request, only compute the report and answer
  // "aborted" (extensions.inspect).
  bool dry_run = false;
  // Set by the prompt.
  std::string report_json;  // the CompatibilityReport of the extension
  bool asked = false;       // a request or alert was shown
  bool declined = false;    // the person (or nothing able to ask) said no
};

struct InstallPromptContext {
  // The page the request belongs to, when the caller has one; a page of the
  // profile otherwise.
  base::WeakPtr<EngineContents> page;
  // The ExtensionSourceKind, or empty to read it from the extension.
  std::string source;
  // Where the package came from, for the install BDK started (noted when the
  // package is unpacked and its id is known, recorded when it is installed).
  std::optional<SourceRecord> record;
  std::shared_ptr<PromptTrace> trace;
};

// Installs the hooks that turn Chromium's extension prompts into requests
// (called once, from InstallPromptHooks).
void InstallExtensionPromptHooks();

// Shows the prompt for `extension` through the request. `type` is the
// prompt's type (install, re-enable, permissions), `custom_permissions` the
// permissions a permissions prompt asks for. `done` is run once, later.
void PromptForExtension(
    Profile* profile,
    InstallPromptContext context,
    const extensions::Extension* extension,
    extensions::InstallPromptData::PromptType type,
    std::unique_ptr<const extensions::PermissionSet> custom_permissions,
    ExtensionInstallPrompt::DoneCallback done);

// The prompt client of an installation BDK starts (CrxInstaller's). It
// shows no post-install UI, and notes the extension's source for the
// observer that announces the installation.
std::unique_ptr<extensions::ExtensionInstallPromptClient> CreateInstallClient(
    Profile* profile,
    InstallPromptContext context);

enum class UninstallOutcome { kUninstalled, kCancelled, kFailed };
using UninstallDone =
    base::OnceCallback<void(UninstallOutcome, const std::string& error)>;

// Asks, through an extension_uninstalls request, and uninstalls when it is
// granted. `done` is run once, later.
void ConfirmAndUninstall(Profile* profile,
                         base::WeakPtr<EngineContents> page,
                         scoped_refptr<const extensions::Extension> extension,
                         UninstallDone done);

}  // namespace bdk

#endif  // BDK_ENGINE_EXTENSIONS_INSTALL_PROMPTS_H_
