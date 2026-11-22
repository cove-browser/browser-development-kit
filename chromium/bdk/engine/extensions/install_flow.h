// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_EXTENSIONS_INSTALL_FLOW_H_
#define BDK_ENGINE_EXTENSIONS_INSTALL_FLOW_H_

#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "bdk/core/runtime/extension_policy.h"
#include "extensions/common/extension_id.h"

class Profile;

namespace bdk {

class EngineContents;

// Installing extensions from each source (T-EXT-05, T-EXT-06, T-EXT-09) and
// updating them. The source policy is checked first (bdkcore::ext::
// source_refusal); the package then goes through the prompt (install_prompts.h)
// and Chromium's installers, whose management policy (management_policy.h)
// inspects the manifest and refuses what cannot work.
//
// An unpacked directory is loaded by Chromium's UnpackedInstaller after the
// prompt; a CRX, an update-URL package and a store package by CrxInstaller,
// which unpacks in a sandbox before it asks. BDK allows off-store CRX
// installs exactly when [extensions.install] crx is true and keeps the prompt
// for every install.

struct InstallSource {
  std::string kind;  // ExtensionSourceKind
  std::string path;  // unpacked, crx
  std::string id;    // updateUrl, store
  std::string update_url;
  std::string store;
};

struct FlowResult {
  bool ok = false;
  std::string extension_id;  // ok
  // extensions.inspect: the CompatibilityReport.
  std::string report_json;
  // Not ok: the error. The domain is `extension` unless it says otherwise.
  std::string domain = "extension";
  std::string code;
  std::string message;
};
using FlowDone = base::OnceCallback<void(FlowResult)>;

// Installs from `source`, or with `dry_run` only inspects it (nothing is
// installed or asked). `page` is the page the request belongs to when the
// caller has one. `done` is run once, later.
void StartInstall(Profile* profile,
                  base::WeakPtr<EngineContents> page,
                  const InstallSource& source,
                  bool dry_run,
                  FlowDone done);

struct UpdateReport {
  std::vector<extensions::ExtensionId> updated;
  std::vector<bdkcore::ext::Finding> refused;
};
using UpdatesDone = base::OnceCallback<void(UpdateReport)>;
using UpdateProgress =
    base::RepeatingCallback<void(double, const std::string&)>;

// Checks `id` (every installed extension with an update source when empty)
// for updates and applies them: an extension with an update URL is updated by
// BDK from its update manifest (the same protocol Chromium's updater speaks,
// so the result is known and the update URL policy is enforced on every
// update); an extension from the web store is handed to Chromium's updater,
// when the store is enabled. An update that the policy refuses (an
// incompatible manifest, a URL the configuration does not allow) is reported
// in `refused` and the installed version keeps running; one that adds
// permissions installs and disables the extension until it is approved.
void CheckForUpdates(Profile* profile,
                     std::optional<extensions::ExtensionId> id,
                     UpdateProgress progress,
                     UpdatesDone done);

}  // namespace bdk

#endif  // BDK_ENGINE_EXTENSIONS_INSTALL_FLOW_H_
