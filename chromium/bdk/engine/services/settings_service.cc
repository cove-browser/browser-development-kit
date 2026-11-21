// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// settings.*: the settings catalogue (services.md section 6) over Chromium's
// preferences and content-setting defaults. Each key maps to one preference,
// or for content.<kind>.default to the default of one permission kind. A key
// the table does not list is not reachable (services.md section 6, rules).

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/json/json_writer.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "bdk/engine/services/events.h"
#include "bdk/engine/services/permissions.h"
#include "bdk/engine/services/services.h"
#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "components/content_settings/core/browser/content_settings_info.h"
#include "components/content_settings/core/browser/content_settings_registry.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/prefs/pref_service.h"

namespace bdk {

namespace {

enum class Codec {
  kBool,
  kInt,  // a number, stored as an integer
  kString,
  kPath,  // a string, stored as a file path
  kStringList,
  kCommaList,   // an array of strings, stored as one comma separated string
  kIntEnum,     // a name from `names`, stored as the integer next to it
  kStringEnum,  // one of `names`, stored as itself
};

struct EnumName {
  const char* name;
  int value;
};

struct Setting {
  const char* key;
  const char* type;  // as settings.describe reports it
  Codec codec;
  const char* pref;  // Chromium's preference path
  std::vector<EnumName> names;
};

// Pref paths are those of chrome/common/pref_names.h and the components'
// pref_names.h at the pinned Chromium; a path that does not exist at a new pin
// makes its key answer an error instead of failing silently (FindPreference).
const std::vector<Setting>& Catalogue() {
  static const base::NoDestructor<std::vector<Setting>> catalogue(
      std::vector<Setting>{
          {"appearance.fontSize",
           "number",
           Codec::kInt,
           "webkit.webprefs.default_font_size",
           {}},
          {"appearance.minimumFontSize",
           "number",
           Codec::kInt,
           "webkit.webprefs.minimum_font_size",
           {}},
          {"languages.accept",
           "array",
           Codec::kCommaList,
           "intl.accept_languages",
           {}},
          {"languages.spellcheck.enabled",
           "boolean",
           Codec::kBool,
           "browser.enable_spellchecking",
           {}},
          {"languages.spellcheck.languages",
           "array",
           Codec::kStringList,
           "spellcheck.dictionaries",
           {}},
          {"search.suggestions",
           "boolean",
           Codec::kBool,
           "search.suggest_enabled",
           {}},
          {"downloads.directory",
           "path",
           Codec::kPath,
           "download.default_directory",
           {}},
          {"downloads.askWhereToSave",
           "boolean",
           Codec::kBool,
           "download.prompt_for_download",
           {}},
          {"privacy.cookies.thirdParty",
           "enum",
           Codec::kIntEnum,
           "profile.cookie_controls_mode",
           {{"allow", 0}, {"block", 1}, {"blockInPrivate", 2}}},
          {"privacy.doNotTrack",
           "boolean",
           Codec::kBool,
           "enable_do_not_track",
           {}},
          {"privacy.webrtcIPHandling",
           "enum",
           Codec::kStringEnum,
           "webrtc.ip_handling_policy",
           {{"default", 0},
            {"default_public_and_private_interfaces", 0},
            {"default_public_interface_only", 0},
            {"disable_non_proxied_udp", 0}}},
          {"privacy.preloading",
           "enum",
           Codec::kIntEnum,
           "net.network_prediction_options",
           {{"standard", 0}, {"off", 2}}},
          {"security.httpsOnly",
           "boolean",
           Codec::kBool,
           "https_only_mode_enabled",
           {}},
          {"passwords.offerToSave",
           "boolean",
           Codec::kBool,
           "credentials_enable_service",
           {}},
          {"passwords.autoSignIn",
           "boolean",
           Codec::kBool,
           "credentials_enable_autosignin",
           {}},
          {"autofill.addresses.enabled",
           "boolean",
           Codec::kBool,
           "autofill.profile_enabled",
           {}},
          {"autofill.cards.enabled",
           "boolean",
           Codec::kBool,
           "autofill.credit_card_enabled",
           {}},
      });
  return *catalogue;
}

constexpr std::string_view kContentPrefix = "content.";
constexpr std::string_view kContentSuffix = ".default";

const Setting* FindSetting(const std::string& key) {
  for (const Setting& s : Catalogue()) {
    if (key == s.key) {
      return &s;
    }
  }
  return nullptr;
}

// The permission kind of a content.<kind>.default key, or null.
const PermissionKind* ContentKind(const std::string& key) {
  if (!key.starts_with(kContentPrefix) || !key.ends_with(kContentSuffix) ||
      key.size() <= kContentPrefix.size() + kContentSuffix.size()) {
    return nullptr;
  }
  return FindPermissionKind(
      key.substr(kContentPrefix.size(),
                 key.size() - kContentPrefix.size() - kContentSuffix.size()));
}

std::string Json(const base::Value& value) {
  return base::WriteJson(value).value_or("null");
}

std::string JsonList(const std::vector<std::string>& items) {
  std::string out = "[";
  for (size_t i = 0; i < items.size(); ++i) {
    out += (i ? "," : "") + JsonString(items[i]);
  }
  return out + "]";
}

std::string AllowedNames(const Setting& s) {
  std::vector<std::string> names;
  for (const EnumName& n : s.names) {
    names.push_back(n.name);
  }
  return JsonList(names);
}

// The preference's value as the catalogue key's JSON value.
std::string Read(const Setting& s, const base::Value& value) {
  switch (s.codec) {
    case Codec::kBool:
    case Codec::kInt:
    case Codec::kString:
    case Codec::kStringList:
      return Json(value);
    case Codec::kPath:
      return value.is_string() ? JsonString(value.GetString()) : "null";
    case Codec::kCommaList: {
      std::vector<std::string> items;
      if (value.is_string()) {
        for (const std::string& item :
             base::SplitString(value.GetString(), ",", base::TRIM_WHITESPACE,
                               base::SPLIT_WANT_NONEMPTY)) {
          items.push_back(item);
        }
      }
      return JsonList(items);
    }
    case Codec::kIntEnum:
      for (const EnumName& n : s.names) {
        if (value.is_int() && value.GetInt() == n.value) {
          return JsonString(n.name);
        }
      }
      return "null";
    case Codec::kStringEnum:
      return value.is_string() ? JsonString(value.GetString()) : "null";
  }
  return "null";
}

// Writes the JSON value to the preference; an error message, or empty.
std::string Write(const Setting& s,
                  const base::Value& value,
                  PrefService& prefs) {
  switch (s.codec) {
    case Codec::kBool:
      if (!value.is_bool()) {
        return "the value must be true or false";
      }
      prefs.SetBoolean(s.pref, value.GetBool());
      return "";
    case Codec::kInt:
      if (!value.is_int() &&
          !(value.is_double() &&
            value.GetDouble() == static_cast<int>(value.GetDouble()))) {
        return "the value must be a whole number";
      }
      prefs.SetInteger(s.pref, value.GetIfInt().value_or(
                                   static_cast<int>(value.GetDouble())));
      return "";
    case Codec::kString:
    case Codec::kPath:
      if (!value.is_string()) {
        return "the value must be a string";
      }
      if (s.codec == Codec::kPath) {
        prefs.SetFilePath(s.pref,
                          base::FilePath::FromUTF8Unsafe(value.GetString()));
      } else {
        prefs.SetString(s.pref, value.GetString());
      }
      return "";
    case Codec::kStringList:
    case Codec::kCommaList: {
      if (!value.is_list()) {
        return "the value must be an array of strings";
      }
      base::ListValue list;
      std::vector<std::string> items;
      for (const base::Value& item : value.GetList()) {
        if (!item.is_string()) {
          return "the value must be an array of strings";
        }
        list.Append(item.GetString());
        items.push_back(item.GetString());
      }
      if (s.codec == Codec::kStringList) {
        prefs.SetList(s.pref, std::move(list));
      } else {
        prefs.SetString(s.pref, base::JoinString(items, ","));
      }
      return "";
    }
    case Codec::kIntEnum:
    case Codec::kStringEnum:
      if (value.is_string()) {
        for (const EnumName& n : s.names) {
          if (value.GetString() == n.name) {
            if (s.codec == Codec::kIntEnum) {
              prefs.SetInteger(s.pref, n.value);
            } else {
              prefs.SetString(s.pref, n.name);
            }
            return "";
          }
        }
      }
      return "the value must be one of " + AllowedNames(s);
  }
  return "unsupported";
}

// The key's preference, or null with the call rejected.
const PrefService::Preference* PrefOf(const std::shared_ptr<ServiceCall>& c,
                                      const Setting& s) {
  if (!c->profile()) {
    c->InvalidArguments("'" + std::string(s.key) + "' needs a profile");
    return nullptr;
  }
  const PrefService::Preference* pref =
      c->profile()->GetPrefs()->FindPreference(s.pref);
  if (!pref) {
    c->Reject(BDK_DOMAIN_PLATFORM, "unavailable",
              "'" + std::string(s.key) + "' is not available in this Chromium");
  }
  return pref;
}

bool UnknownKey(const std::shared_ptr<ServiceCall>& c, const std::string& key) {
  c->InvalidArguments("unknown setting '" + key + "'");
  return true;
}

HostContentSettingsMap* ContentMap(const ServiceCall& c) {
  return HostContentSettingsMapFactory::GetForProfile(c.original_profile());
}

// The value of one key, as JSON; empty, with the call rejected, when it fails.
// A key that is not in the catalogue has no value: null.
std::string ValueOf(const std::shared_ptr<ServiceCall>& c,
                    const std::string& key) {
  if (const PermissionKind* kind = ContentKind(key)) {
    if (!c->profile()) {
      c->InvalidArguments("'" + key + "' needs a profile");
      return "";
    }
    return JsonString(PermissionValueName(
        ContentMap(*c)->GetDefaultContentSetting(kind->type)));
  }
  const Setting* s = FindSetting(key);
  if (!s) {
    return "null";  // a key nothing has set reads as null; writing it is
                    // refused
  }
  const PrefService::Preference* pref = PrefOf(c, *s);
  return pref ? Read(*s, *pref->GetValue()) : "";
}

void Get(const std::shared_ptr<ServiceCall>& c) {
  std::string value = ValueOf(c, c->StringOr("key", ""));
  if (!value.empty()) {
    c->Resolve("{\"value\":" + value + "}");
  }
}

void GetMany(const std::shared_ptr<ServiceCall>& c) {
  std::string out = "{\"values\":{";
  bool first = true;
  if (const base::ListValue* keys = c->List("keys")) {
    for (const base::Value& key : *keys) {
      std::string value = ValueOf(c, key.GetString());
      if (value.empty()) {
        return;
      }
      out += (first ? "" : ",") + JsonString(key.GetString()) + ":" + value;
      first = false;
    }
  }
  c->Resolve(out + "}}");
}

void Describe(const std::shared_ptr<ServiceCall>& c) {
  const std::string key = c->StringOr("key", "");
  if (const PermissionKind* kind = ContentKind(key)) {
    if (!c->profile()) {
      c->InvalidArguments("'" + key + "' needs a profile");
      return;
    }
    HostContentSettingsMap* map = ContentMap(*c);
    const ContentSetting initial =
        content_settings::ContentSettingsRegistry::GetInstance()
            ->Get(kind->type)
            ->GetInitialDefaultSetting();
    const ContentSetting current = map->GetDefaultContentSetting(kind->type);
    std::string allowed = "[";
    for (ContentSetting s :
         {CONTENT_SETTING_ALLOW, CONTENT_SETTING_BLOCK, CONTENT_SETTING_ASK}) {
      if (PermissionValueAllowed(*kind, s)) {
        allowed += std::string(allowed.size() > 1 ? "," : "") +
                   JsonString(PermissionValueName(s));
      }
    }
    c->Resolve(
        "{\"key\":" + JsonString(key) + ",\"type\":\"enum\",\"default\":" +
        JsonString(PermissionValueName(initial)) +
        ",\"value\":" + JsonString(PermissionValueName(current)) +
        ",\"allowed\":" + allowed + "]" + ",\"scope\":\"profile\",\"source\":" +
        (current == initial ? "\"product\"" : "\"user\"") +
        ",\"locked\":false}");
    return;
  }
  const Setting* s = FindSetting(key);
  if (!s) {
    UnknownKey(c, key);
    return;
  }
  const PrefService::Preference* pref = PrefOf(c, *s);
  if (!pref) {
    return;
  }
  std::string out = "{\"key\":" + JsonString(key) +
                    ",\"type\":" + JsonString(s->type) + ",\"default\":";
  const base::Value* default_value =
      c->profile()->GetPrefs()->GetDefaultPrefValue(s->pref);
  out += default_value ? Read(*s, *default_value) : "null";
  out += ",\"value\":" + Read(*s, *pref->GetValue());
  if (!s->names.empty()) {
    out += ",\"allowed\":" + AllowedNames(*s);
  }
  out += std::string(",\"scope\":\"profile\",\"source\":") +
         (pref->IsDefaultValue() ? "\"product\"" : "\"user\"") +
         ",\"locked\":" + (pref->IsManaged() ? "true" : "false") + "}";
  c->Resolve(out);
}

void SetImpl(const std::shared_ptr<ServiceCall>& c) {
  const std::string key = c->StringOr("key", "");
  const base::Value* value = c->args().Find("value");
  if (!value) {
    c->InvalidArguments("a value is needed");
    return;
  }
  if (const PermissionKind* kind = ContentKind(key)) {
    if (!c->profile()) {
      c->InvalidArguments("'" + key + "' needs a profile");
      return;
    }
    std::optional<ContentSetting> setting =
        value->is_string() ? ParsePermissionValue(value->GetString())
                           : std::nullopt;
    if (!setting || !PermissionValueAllowed(*kind, *setting)) {
      c->InvalidArguments("'" + Json(*value) + "' is not allowed for " + key);
      return;
    }
    ContentMap(*c)->SetDefaultContentSetting(kind->type, *setting);
    c->ResolveNull();
    return;
  }
  const Setting* s = FindSetting(key);
  if (!s) {
    UnknownKey(c, key);
    return;
  }
  if (!PrefOf(c, *s)) {
    return;
  }
  const std::string problem = Write(*s, *value, *c->profile()->GetPrefs());
  if (!problem.empty()) {
    c->InvalidArguments("'" + key + "': " + problem);
    return;
  }
  c->ResolveNull();
}

void ResetImpl(const std::shared_ptr<ServiceCall>& c) {
  const std::string key = c->StringOr("key", "");
  if (const PermissionKind* kind = ContentKind(key)) {
    if (!c->profile()) {
      c->InvalidArguments("'" + key + "' needs a profile");
      return;
    }
    ContentMap(*c)->SetDefaultContentSetting(kind->type,
                                             CONTENT_SETTING_DEFAULT);
    c->ResolveNull();
    return;
  }
  const Setting* s = FindSetting(key);
  if (!s) {
    UnknownKey(c, key);
    return;
  }
  if (!PrefOf(c, *s)) {
    return;
  }
  c->profile()->GetPrefs()->ClearPref(s->pref);
  c->ResolveNull();
}

// Runs a change and announces it when the value changed.
void Changing(const std::shared_ptr<ServiceCall>& c,
              void (*change)(const std::shared_ptr<ServiceCall>&)) {
  const std::string key = c->StringOr("key", "");
  const std::string before = ValueOf(c, key);
  if (before.empty()) {
    return;
  }
  change(c);
  const std::string after = ValueOf(c, key);
  if (after != before) {
    EmitEvent(c->builtin().profile, "settings.changed",
              "{\"key\":" + JsonString(key) + ",\"oldValue\":" + before +
                  ",\"newValue\":" + after + ",\"source\":\"application\"}");
  }
}

void Set(const std::shared_ptr<ServiceCall>& c) {
  Changing(c, &SetImpl);
}

void Reset(const std::shared_ptr<ServiceCall>& c) {
  Changing(c, &ResetImpl);
}

}  // namespace

void AddSettingsHandlers(HandlerTable& table) {
  table["settings.get"] = &Get;
  table["settings.getMany"] = &GetMany;
  table["settings.describe"] = &Describe;
  table["settings.set"] = &Set;
  table["settings.reset"] = &Reset;
}

}  // namespace bdk
