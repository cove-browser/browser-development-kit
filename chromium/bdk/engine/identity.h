// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_IDENTITY_H_
#define BDK_ENGINE_IDENTITY_H_

#include <optional>
#include <string>
#include <vector>

namespace bdk {

// What the application's bdk.toml says about who it is (T-PKG-02,
// configuration.md section 3.2). One prebuilt runtime serves many
// applications, so none of this is compiled in.
struct Identity {
  std::string name;               // [browser] name
  std::string storage_directory;  // [storage] directory
  std::string keychain_item;      // [storage] keychain_item
  bool devtools = true;           // [ui.surfaces] devtools is not "disabled"
};

// Reads the identity from bdk.resolved.json. The storage keys default as
// configuration.md says. Nullopt when the browser has no name.
std::optional<Identity> ParseIdentity(const std::string& resolved_json);

// Before Chromium starts: the Keychain item that protects cookies and
// passwords.
void ApplyIdentityBeforeStart(const Identity& identity);
// After Chromium starts: the product name in Chromium's own strings.
void ApplyIdentityAfterStart(const Identity& identity);

// Switches added to Chromium's command line beyond the ones start-up always
// sets. Test and diagnostic runs add more through BDK_ENGINE_SWITCHES (space
// separated, without dashes), BDK_ENGINE_MOCK_KEYCHAIN=1 and
// BDK_ENGINE_HOST_RESOLVER_RULES (Chromium's --host-resolver-rules).
std::vector<std::string> ExtraSwitches();

// Whether the application allows DevTools (the context menu's Inspect item).
bool DevToolsEnabled();
// Set from the configuration at every start (devtools_frontend.cc).
void SetDevToolsEnabled(bool enabled);

}  // namespace bdk

#endif  // BDK_ENGINE_IDENTITY_H_
