// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/identity.h"

#include "base/environment.h"
#include "base/strings/string_split.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "bdk/engine/json_util.h"
#include "build/build_config.h"
#include "chrome/grit/branded_strings.h"
#include "ui/base/resource/resource_bundle.h"

#if BUILDFLAG(IS_MAC)
#include "components/os_crypt/common/keychain_password_mac.h"
#endif

namespace bdk {

std::optional<Identity> ParseIdentity(const std::string& resolved_json) {
  std::optional<base::DictValue> resolved = ParseJsonDict(resolved_json);
  if (!resolved) {
    return std::nullopt;
  }
  const std::string* name = resolved->FindStringByDottedPath("browser.name");
  if (!name || name->empty()) {
    return std::nullopt;
  }
  Identity identity;
  identity.name = *name;
  const std::string* directory =
      resolved->FindStringByDottedPath("storage.directory");
  identity.storage_directory =
      directory && !directory->empty() ? *directory : *name;
  const std::string* keychain =
      resolved->FindStringByDottedPath("storage.keychain_item");
  identity.keychain_item =
      keychain && !keychain->empty() ? *keychain : *name + " Safe Storage";
  const std::string* devtools =
      resolved->FindStringByDottedPath("surfaces.devtools");
  identity.devtools = !devtools || *devtools != "disabled";
  return identity;
}

namespace {
bool g_devtools_enabled = true;
}  // namespace

bool DevToolsEnabled() {
  return g_devtools_enabled;
}

void ApplyIdentityBeforeStart(const Identity& identity) {
#if BUILDFLAG(IS_MAC)
  KeychainPassword::GetServiceName() = identity.keychain_item;
  KeychainPassword::GetAccountName() = identity.name;
#endif
}

void ApplyIdentityAfterStart(const Identity& identity) {
  g_devtools_enabled = identity.devtools;
  ui::ResourceBundle& bundle = ui::ResourceBundle::GetSharedInstance();
  const std::u16string name = base::UTF8ToUTF16(identity.name);
  bundle.OverrideLocaleStringResource(IDS_PRODUCT_NAME, name);
  bundle.OverrideLocaleStringResource(IDS_SHORT_PRODUCT_NAME, name);
}

std::vector<std::string> ExtraSwitches() {
  std::unique_ptr<base::Environment> env = base::Environment::Create();
  std::vector<std::string> switches;
  if (env->GetVar("BDK_ENGINE_MOCK_KEYCHAIN").value_or("") == "1") {
    switches.push_back("use-mock-keychain");
  }
  for (std::string& s :
       base::SplitString(env->GetVar("BDK_ENGINE_SWITCHES").value_or(""), " ",
                         base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    switches.push_back(std::move(s));
  }
  return switches;
}

}  // namespace bdk
