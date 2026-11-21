// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_EXTENSIONS_EXTENSION_SUPPORT_H_
#define BDK_ENGINE_EXTENSIONS_EXTENSION_SUPPORT_H_

#include <memory>
#include <string>

#include "bdk/core/runtime/extension_policy.h"

class Profile;

namespace extensions {
class Extension;
}  // namespace extensions

namespace bdk {

// What the extension files of the engine share: the facts the engine tells
// the core's inspection about itself, the configuration, and the manifest as
// the core's JSON text.

// What this engine loads. Chromium 158 loads manifest version 3 only for an
// extension that is not a component: ManifestV2Handler disables every other
// one unconditionally (extensions/browser/manifest_v2_handler.cc,
// ShouldDisableLegacyExtensions; the only exception is a test switch), with
// no dependence on branding, so an MV2 manifest could never run in this
// product.
bdkcore::ext::EngineFacts EngineFacts();

// The core's runtime, or null while none runs (an observer outlives the
// runtime that opened its profile for a moment).
std::shared_ptr<bdkcore::Runtime> CurrentRuntime();

// The manifest of `extension` as JSON text, for bdkcore::ext::inspect_manifest.
// It is the manifest after Chromium localised it.
std::string ManifestJsonOf(const extensions::Extension& extension);

// The compatibility report of an extension that Chromium has loaded.
bdkcore::ext::Report ReportOf(const bdkcore::Runtime& runtime,
                              const extensions::Extension& extension);

// The profile extensions live in: a private profile has none of its own.
Profile* ExtensionProfile(Profile* profile);

}  // namespace bdk

#endif  // BDK_ENGINE_EXTENSIONS_EXTENSION_SUPPORT_H_
