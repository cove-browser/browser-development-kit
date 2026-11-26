// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_EXTENSIONS_MANAGEMENT_POLICY_H_
#define BDK_ENGINE_EXTENSIONS_MANAGEMENT_POLICY_H_

#include <memory>
#include <optional>
#include <string>

#include "bdk/core/runtime/extension_policy.h"
#include "extensions/common/extension_id.h"

class Profile;

namespace extensions {
class Extension;
}  // namespace extensions

namespace bdk {

// What BDK decides about an extension at Chromium's own choke points
// (T-EXT-05, T-EXT-06, T-EXT-09): installing it, updating it, loading it.
//
// - A ManagementPolicy provider, asked by every installer (CrxInstaller,
//   UnpackedInstaller, an update, a reload) before the package is accepted:
//   the web store's install is refused unless [extensions.install] stores
//   lists it, an update URL must match update_urls, an unpacked package must
//   be allowed, and the manifest is inspected (bdkcore::ext::inspect_manifest)
//   and refused as incompatible unless install_policy is allow_all. A
//   refusal keeps the installed version running: the installer fails and
//   nothing replaces it.
// - An observer that, whenever an extension loads, withholds the APIs of
//   surfaces the configuration disables (so they are absent from the
//   extension's chrome object) and counts each as an `unavailableApi` error,
//   and turns off the extension's access to private profiles when
//   [extensions] allow_in_private is "never".
// - The extension keys of S6 (T-EXT-14; extensions.md section 9): an
//   identifier in [extensions.install] blocklist is refused from every source
//   and, already installed, kept disabled (the reason "blocklisted"); a
//   bundled extension cannot be uninstalled, nor disabled when it is required;
//   and Chromium's developer mode follows [extensions] developer_mode.

// The error message of a refusal starts with one of these, so the install
// flow can tell its kind from Chromium's own refusals.
inline constexpr char kRefusalIncompatible[] = "[bdk:incompatible] ";
inline constexpr char kRefusalSource[] = "[bdk:source] ";
inline constexpr char kRefusalBlocklisted[] = "[bdk:blocklisted] ";

// What BDK decides about an extension Chromium has unpacked or loaded.
struct Evaluation {
  std::string report_json;                        // the CompatibilityReport
  std::optional<bdkcore::ext::Finding> blocking;  // the finding that refused
  bool refused = false;
  // When refused: sourceNotAllowed, incompatible or blocklisted (BDK_CODE_*),
  // and why.
  std::string code;
  std::string message;
};

// The decision, for the flows that want it before they ask anyone (nothing is
// recorded). The provider makes the same one.
Evaluation EvaluateExtension(Profile* profile,
                             const extensions::Extension& extension);

// The evaluation of `id` in `profile`, once; nothing if it was never asked.
std::optional<Evaluation> TakeEvaluation(Profile* profile,
                                         const extensions::ExtensionId& id);

// Registers the provider and the observer for `profile` (a persistent
// profile, not a private one) and applies them to what is already loaded.
// Destroying the object unregisters them.
class ExtensionPolicyHooks {
 public:
  virtual ~ExtensionPolicyHooks() = default;
};
std::unique_ptr<ExtensionPolicyHooks> CreateExtensionPolicyHooks(
    Profile* profile);

// Whether the configuration enables the named store.
bool StoreEnabled(const bdkcore::ExtensionConfig& config,
                  const std::string& store);

}  // namespace bdk

#endif  // BDK_ENGINE_EXTENSIONS_MANAGEMENT_POLICY_H_
