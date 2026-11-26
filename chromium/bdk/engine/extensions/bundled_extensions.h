// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_EXTENSIONS_BUNDLED_EXTENSIONS_H_
#define BDK_ENGINE_EXTENSIONS_BUNDLED_EXTENSIONS_H_

#include <string>

#include "base/functional/callback_forward.h"

class Profile;

namespace base {
class FilePath;
}

namespace bdk {

// Extensions shipped inside the application ([extensions.bundled];
// extensions.md section 9; T-EXT-14). Each is an unpacked directory at
// Contents/Resources/<path>, loaded with Chromium's unpacked loader in every
// persistent profile when the profile opens, with no prompt: the
// application's signature covers it. The management policy
// (management_policy.cc) keeps it installed, and enabled when it is required.

struct BundledExtension {
  std::string name;  // the [extensions.bundled] key
  bool required = false;
};

// Loads the configuration's bundled extensions into `profile`, then runs
// `done`, also when there are none or a directory is missing (each failure is
// logged). A blocklisted one is refused by the management policy.
void LoadBundledExtensions(Profile* profile, base::OnceClosure done);

// The bundled extension Chromium loaded from `path`, or null for any other.
const BundledExtension* FindBundledExtension(const base::FilePath& path);

}  // namespace bdk

#endif  // BDK_ENGINE_EXTENSIONS_BUNDLED_EXTENSIONS_H_
