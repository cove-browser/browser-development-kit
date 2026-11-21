// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/extensions/extension_support.h"

#include "base/version_info/version_info.h"
#include "bdk/engine/json_util.h"
#include "chrome/browser/profiles/profile.h"
#include "extensions/common/extension.h"
#include "extensions/common/manifest.h"

namespace bdk {

bdkcore::ext::EngineFacts EngineFacts() {
  bdkcore::ext::EngineFacts facts;
  facts.chrome_major = version_info::GetMajorVersionNumberAsInt();
  facts.manifest_versions = {3};
  return facts;
}

std::shared_ptr<bdkcore::Runtime> CurrentRuntime() {
  return bdkcore::current_runtime();
}

std::string ManifestJsonOf(const extensions::Extension& extension) {
  const base::DictValue* manifest = extension.manifest()->value();
  return manifest ? ToJson(*manifest) : "{}";
}

bdkcore::ext::Report ReportOf(const bdkcore::Runtime& runtime,
                              const extensions::Extension& extension) {
  return bdkcore::ext::inspect_manifest(runtime, EngineFacts(),
                                        ManifestJsonOf(extension));
}

Profile* ExtensionProfile(Profile* profile) {
  return profile ? profile->GetOriginalProfile() : nullptr;
}

}  // namespace bdk
