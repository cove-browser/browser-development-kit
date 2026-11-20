// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// permissions.*: site settings over Chromium's HostContentSettingsMap. Each
// kind is one ContentSettingsType. A private profile has its own map, so what
// it sets for an origin stays in it; defaults are Chromium's and are set on the
// parent's map, which the private profile's map follows.

#include "bdk/engine/services/permissions.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "bdk/engine/services/events.h"
#include "bdk/engine/services/services.h"
#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/content_settings/core/common/content_settings.h"
#include "components/content_settings/core/common/content_settings_pattern.h"
#include "components/content_settings/core/common/content_settings_types.h"
#include "url/gurl.h"

namespace bdk {

namespace {

constexpr unsigned kAllow = 1, kBlock = 2, kAsk = 4;
constexpr unsigned kAll = kAllow | kBlock | kAsk;
constexpr unsigned kAllowBlock = kAllow | kBlock;
constexpr unsigned kAskBlock = kAsk | kBlock;

const PermissionKind kKinds[] = {
    {"camera", ContentSettingsType::MEDIASTREAM_CAMERA, kAll},
    {"microphone", ContentSettingsType::MEDIASTREAM_MIC, kAll},
    {"geolocation", ContentSettingsType::GEOLOCATION, kAll},
    {"notifications", ContentSettingsType::NOTIFICATIONS, kAll},
    {"popups", ContentSettingsType::POPUPS, kAllowBlock},
    {"javascript", ContentSettingsType::JAVASCRIPT, kAllowBlock},
    {"images", ContentSettingsType::IMAGES, kAllowBlock},
    {"cookies", ContentSettingsType::COOKIES, kAllowBlock},
    {"sound", ContentSettingsType::SOUND, kAllowBlock},
    {"automaticDownloads", ContentSettingsType::AUTOMATIC_DOWNLOADS, kAll},
    {"clipboard", ContentSettingsType::CLIPBOARD_READ_WRITE, kAll},
    {"midi", ContentSettingsType::MIDI_SYSEX, kAll},
    {"usb", ContentSettingsType::USB_GUARD, kAskBlock},
    {"bluetooth", ContentSettingsType::BLUETOOTH_GUARD, kAskBlock},
    {"hid", ContentSettingsType::HID_GUARD, kAskBlock},
    {"serial", ContentSettingsType::SERIAL_GUARD, kAskBlock},
    {"windowManagement", ContentSettingsType::WINDOW_MANAGEMENT, kAll},
    {"fileSystemAccess", ContentSettingsType::FILE_SYSTEM_WRITE_GUARD,
     kAskBlock},
};

const char* ValueName(ContentSetting setting) {
  switch (setting) {
    case CONTENT_SETTING_ALLOW:
      return "allow";
    case CONTENT_SETTING_BLOCK:
      return "block";
    default:
      return "ask";
  }
}

std::optional<ContentSetting> ParseValue(const std::string& value) {
  if (value == "allow") {
    return CONTENT_SETTING_ALLOW;
  }
  if (value == "block") {
    return CONTENT_SETTING_BLOCK;
  }
  if (value == "ask") {
    return CONTENT_SETTING_ASK;
  }
  return std::nullopt;
}

unsigned Bit(ContentSetting setting) {
  return setting == CONTENT_SETTING_ALLOW   ? kAllow
         : setting == CONTENT_SETTING_BLOCK ? kBlock
                                            : kAsk;
}

std::string AllowedJson(unsigned allowed) {
  std::string out = "[";
  for (ContentSetting s :
       {CONTENT_SETTING_ALLOW, CONTENT_SETTING_BLOCK, CONTENT_SETTING_ASK}) {
    if (allowed & Bit(s)) {
      out +=
          std::string(out.size() > 1 ? "," : "") + "\"" + ValueName(s) + "\"";
    }
  }
  return out + "]";
}

HostContentSettingsMap* MapOf(const ServiceCall& c) {
  return HostContentSettingsMapFactory::GetForProfile(c.profile());
}

const PermissionKind* KindArg(const std::shared_ptr<ServiceCall>& c,
                              const char* key = "kind") {
  const PermissionKind* kind = FindPermissionKind(c->StringOr(key, ""));
  if (!kind) {
    c->InvalidArguments("unknown permission kind '" + c->StringOr(key, "") +
                        "'");
  }
  return kind;
}

std::optional<GURL> OriginArg(const std::shared_ptr<ServiceCall>& c) {
  GURL url(c->StringOr("origin", ""));
  if (!url.is_valid() || url.host().empty()) {
    c->InvalidArguments("the origin is not a valid URL");
    return std::nullopt;
  }
  return url;
}

// Where the value for the URL came from.
const char* SourceOf(const content_settings::SettingInfo& info) {
  switch (info.source) {
    case content_settings::SettingSource::kExtension:
      return "extension";
    case content_settings::SettingSource::kNone:
      return "default";
    default:
      break;
  }
  return info.primary_pattern.MatchesAllHosts() &&
                 info.secondary_pattern.MatchesAllHosts()
             ? "default"
             : "user";
}

std::string SettingJson(const HostContentSettingsMap& map,
                        const PermissionKind& kind,
                        const GURL& url) {
  content_settings::SettingInfo info;
  const ContentSetting value =
      map.GetContentSetting(url, url, kind.type, &info);
  return std::string("{\"kind\":") + JsonString(kind.name) +
         ",\"value\":" + JsonString(ValueName(value)) +
         ",\"source\":" + JsonString(SourceOf(info)) + "}";
}

void Announce(const ServiceCall& c,
              const std::string& origin,
              const std::string& kind,
              const std::string& value) {
  EmitEvent(c.builtin().profile, "permissions.changed",
            "{\"origin\":" + JsonStringOrNull(origin) +
                ",\"kind\":" + JsonString(kind) +
                ",\"value\":" + (value.empty() ? "null" : JsonString(value)) +
                ",\"source\":\"application\"}");
}

void Kinds(const std::shared_ptr<ServiceCall>& c) {
  const HostContentSettingsMap* map = MapOf(*c);
  std::string out = "{\"kinds\":[";
  bool first = true;
  for (const PermissionKind& kind : kKinds) {
    out +=
        std::string(first ? "" : ",") + "{\"kind\":" + JsonString(kind.name) +
        ",\"allowed\":" + AllowedJson(kind.allowed) + ",\"default\":" +
        JsonString(ValueName(map->GetDefaultContentSetting(kind.type))) + "}";
    first = false;
  }
  c->Resolve(out + "]}");
}

void Get(const std::shared_ptr<ServiceCall>& c) {
  const PermissionKind* kind = KindArg(c);
  std::optional<GURL> origin = kind ? OriginArg(c) : std::nullopt;
  if (origin) {
    c->Resolve(SettingJson(*MapOf(*c), *kind, *origin));
  }
}

void ForOrigin(const std::shared_ptr<ServiceCall>& c) {
  std::optional<GURL> origin = OriginArg(c);
  if (!origin) {
    return;
  }
  const HostContentSettingsMap& map = *MapOf(*c);
  std::string out = "{\"items\":[";
  bool first = true;
  for (const PermissionKind& kind : kKinds) {
    content_settings::SettingInfo info;
    map.GetContentSetting(*origin, *origin, kind.type, &info);
    if (std::string(SourceOf(info)) == "default") {
      continue;
    }
    out += (first ? "" : ",") + SettingJson(map, kind, *origin);
    first = false;
  }
  c->Resolve(out + "]}");
}

void Exceptions(const std::shared_ptr<ServiceCall>& c) {
  const PermissionKind* kind = KindArg(c);
  if (!kind) {
    return;
  }
  std::string out = "{\"items\":[";
  bool first = true;
  for (const ContentSettingPatternSource& rule :
       MapOf(*c)->GetSettingsForOneType(kind->type)) {
    if (rule.primary_pattern.MatchesAllHosts() &&
        rule.secondary_pattern.MatchesAllHosts()) {
      continue;  // the default
    }
    const bool extension =
        rule.source ==
        content_settings::mojom::ProviderType::kCustomExtensionProvider;
    // The origin is the rule's pattern, which is what Chromium keeps: a URL
    // pattern such as https://[*.]example.com:443.
    out += std::string(first ? "" : ",") +
           "{\"origin\":" + JsonString(rule.primary_pattern.ToString()) +
           ",\"value\":" + JsonString(ValueName(rule.GetContentSetting())) +
           ",\"source\":" + JsonString(extension ? "extension" : "user") + "}";
    first = false;
  }
  c->Resolve(out + "]}");
}

void Set(const std::shared_ptr<ServiceCall>& c) {
  const PermissionKind* kind = KindArg(c);
  std::optional<GURL> origin = kind ? OriginArg(c) : std::nullopt;
  if (!origin) {
    return;
  }
  std::optional<ContentSetting> value = ParseValue(c->StringOr("value", ""));
  if (!value || !(kind->allowed & Bit(*value))) {
    c->InvalidArguments("'" + c->StringOr("value", "") +
                        "' is not allowed for " + kind->name);
    return;
  }
  MapOf(*c)->SetContentSettingDefaultScope(*origin, *origin, kind->type,
                                           *value);
  Announce(*c, c->StringOr("origin", ""), kind->name, ValueName(*value));
  c->ResolveNull();
}

void Reset(const std::shared_ptr<ServiceCall>& c) {
  std::optional<GURL> origin = OriginArg(c);
  if (!origin) {
    return;
  }
  HostContentSettingsMap* map = MapOf(*c);
  if (c->String("kind")) {
    const PermissionKind* kind = KindArg(c);
    if (!kind) {
      return;
    }
    map->SetContentSettingDefaultScope(*origin, *origin, kind->type,
                                       CONTENT_SETTING_DEFAULT);
    Announce(*c, c->StringOr("origin", ""), kind->name, "");
  } else {
    for (const PermissionKind& kind : kKinds) {
      map->SetContentSettingDefaultScope(*origin, *origin, kind.type,
                                         CONTENT_SETTING_DEFAULT);
      Announce(*c, c->StringOr("origin", ""), kind.name, "");
    }
  }
  c->ResolveNull();
}

void GetDefault(const std::shared_ptr<ServiceCall>& c) {
  if (const PermissionKind* kind = KindArg(c)) {
    c->Resolve(
        std::string("{\"value\":") +
        JsonString(ValueName(MapOf(*c)->GetDefaultContentSetting(kind->type))) +
        "}");
  }
}

void SetDefault(const std::shared_ptr<ServiceCall>& c) {
  const PermissionKind* kind = KindArg(c);
  if (!kind) {
    return;
  }
  std::optional<ContentSetting> value = ParseValue(c->StringOr("value", ""));
  if (!value || !(kind->allowed & Bit(*value))) {
    c->InvalidArguments("'" + c->StringOr("value", "") +
                        "' is not allowed for " + kind->name);
    return;
  }
  // Defaults are not set on an off-the-record map; the private profile's map
  // follows its parent's.
  HostContentSettingsMapFactory::GetForProfile(c->original_profile())
      ->SetDefaultContentSetting(kind->type, *value);
  Announce(*c, "", kind->name, ValueName(*value));
  c->ResolveNull();
}

}  // namespace

const PermissionKind* FindPermissionKind(const std::string& name) {
  for (const PermissionKind& kind : kKinds) {
    if (name == kind.name) {
      return &kind;
    }
  }
  return nullptr;
}

const char* PermissionValueName(ContentSetting setting) {
  return ValueName(setting);
}

std::optional<ContentSetting> ParsePermissionValue(const std::string& value) {
  return ParseValue(value);
}

bool PermissionValueAllowed(const PermissionKind& kind,
                            ContentSetting setting) {
  return (kind.allowed & Bit(setting)) != 0;
}

void AddPermissionsHandlers(HandlerTable& table) {
  table["permissions.kinds"] = &Kinds;
  table["permissions.get"] = &Get;
  table["permissions.forOrigin"] = &ForOrigin;
  table["permissions.exceptions"] = &Exceptions;
  table["permissions.set"] = &Set;
  table["permissions.reset"] = &Reset;
  table["permissions.getDefault"] = &GetDefault;
  table["permissions.setDefault"] = &SetDefault;
}

}  // namespace bdk
