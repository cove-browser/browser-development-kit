// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The permission kinds the runtime knows, each one Chromium content setting.
// Shared by permissions.* and by the settings catalogue's
// content.<kind>.default keys.

#ifndef BDK_ENGINE_SERVICES_PERMISSIONS_H_
#define BDK_ENGINE_SERVICES_PERMISSIONS_H_

#include <optional>
#include <string>

#include "components/content_settings/core/common/content_settings.h"
#include "components/content_settings/core/common/content_settings_types.h"

namespace bdk {

struct PermissionKind {
  const char* name;
  ContentSettingsType type;
  unsigned allowed;  // a mask of the values the kind accepts
};

// Null when there is no kind of that name.
const PermissionKind* FindPermissionKind(const std::string& name);

const char* PermissionValueName(ContentSetting setting);
std::optional<ContentSetting> ParsePermissionValue(const std::string& value);
bool PermissionValueAllowed(const PermissionKind& kind, ContentSetting setting);

}  // namespace bdk

#endif  // BDK_ENGINE_SERVICES_PERMISSIONS_H_
