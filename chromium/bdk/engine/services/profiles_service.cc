// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// runtime.info and profiles.*. The core owns the profile list; these handlers
// answer from it and change it the way bdk_profile_create, bdk_profile_rename
// and bdk_profile_delete do (core_host.cc), so that a profile made by a
// command is one the application can open.

#include <algorithm>
#include <string>
#include <utility>

#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "bdk/engine/json_util.h"
#include "bdk/engine/services/events.h"
#include "bdk/engine/services/services.h"
#include "components/version_info/version_info.h"

namespace bdk {

namespace {

struct RuntimeInfo {
  std::string browser_name;
  std::string internal_scheme = "bdk";
};

RuntimeInfo& Info() {
  static base::NoDestructor<RuntimeInfo> info;
  return *info;
}

std::string ProfileJson(const bdkcore::Profile& profile) {
  return "{\"id\":" + JsonString(profile.id) +
         ",\"name\":" + JsonString(profile.name) +
         ",\"lastUsed\":null,\"isPrivate\":" +
         (profile.ephemeral ? "true" : "false") + "}";
}

std::shared_ptr<bdkcore::Profile> FindProfile(bdkcore::Runtime& runtime,
                                              const std::string& id) {
  for (const auto& profile : runtime.profiles) {
    if (profile->id == id) {
      return profile;
    }
  }
  return nullptr;
}

bool NameTaken(const bdkcore::Runtime& runtime,
               const std::string& name,
               const bdkcore::Profile* except) {
  for (const auto& profile : runtime.profiles) {
    if (!profile->ephemeral && profile.get() != except &&
        profile->name == name) {
      return true;
    }
  }
  return false;
}

void RuntimeInfoCommand(const std::shared_ptr<ServiceCall>& c) {
  c->Resolve("{\"bdkVersion\":\"0.0.0\",\"chromiumVersion\":" +
             JsonString(version_info::GetVersionNumber()) +
             ",\"browserName\":" + JsonString(Info().browser_name) +
             ",\"internalScheme\":" + JsonString(Info().internal_scheme) +
             ",\"platform\":\"macos\"}");
}

void List(const std::shared_ptr<ServiceCall>& c) {
  std::string out = "{\"profiles\":[";
  bool first = true;
  for (const auto& profile : c->runtime().profiles) {
    if (profile->ephemeral) {
      continue;
    }
    out += (first ? "" : ",") + ProfileJson(*profile);
    first = false;
  }
  c->Resolve(out + "]}");
}

void Current(const std::shared_ptr<ServiceCall>& c) {
  c->Resolve(ProfileJson(*c->builtin().profile));
}

void Create(const std::shared_ptr<ServiceCall>& c) {
  bdkcore::Runtime& runtime = c->runtime();
  const std::string name = c->StringOr("name", "");
  if (name.empty()) {
    c->InvalidArguments("a profile needs a display name");
    return;
  }
  if (NameTaken(runtime, name, nullptr)) {
    c->Reject(BDK_DOMAIN_PROFILE, BDK_CODE_NAME_TAKEN,
              "a profile named '" + name + "' exists");
    return;
  }
  auto profile = std::make_shared<bdkcore::Profile>();
  profile->id = "profile-" + base::NumberToString(runtime.next_id++);
  profile->name = name;
  runtime.profiles.push_back(profile);
  bdkcore::backend().ProfilesChanged(runtime);
  EmitEvent(
      nullptr, "profiles.created",
      "{\"profile\":" + ProfileJson(*profile) + ",\"source\":\"application\"}");
  c->Resolve(ProfileJson(*profile));
}

void Rename(const std::shared_ptr<ServiceCall>& c) {
  bdkcore::Runtime& runtime = c->runtime();
  const std::string id = c->StringOr("id", "");
  std::shared_ptr<bdkcore::Profile> profile = FindProfile(runtime, id);
  const std::string name = c->StringOr("name", "");
  if (!profile || profile->ephemeral) {
    c->Reject(BDK_DOMAIN_PROFILE, BDK_CODE_NOT_FOUND,
              "no profile '" + id + "'");
  } else if (name.empty()) {
    c->InvalidArguments("a profile needs a display name");
  } else if (NameTaken(runtime, name, profile.get())) {
    c->Reject(BDK_DOMAIN_PROFILE, BDK_CODE_NAME_TAKEN,
              "a profile named '" + name + "' exists");
  } else {
    profile->name = name;
    bdkcore::backend().ProfilesChanged(runtime);
    EmitEvent(nullptr, "profiles.renamed",
              "{\"profile\":" + ProfileJson(*profile) +
                  ",\"source\":\"application\"}");
    c->ResolveNull();
  }
}

void Delete(const std::shared_ptr<ServiceCall>& c) {
  bdkcore::Runtime& runtime = c->runtime();
  const std::string id = c->StringOr("id", "");
  std::shared_ptr<bdkcore::Profile> profile = FindProfile(runtime, id);
  if (!profile || profile->ephemeral) {
    c->Reject(BDK_DOMAIN_PROFILE, BDK_CODE_NOT_FOUND,
              "no profile '" + id + "'");
    return;
  }
  // services.md 5.2: refused while pages exist in it.
  for (const auto& page : runtime.contents) {
    if (page->profile == profile && !page->dead) {
      c->Reject(BDK_DOMAIN_PROFILE, "inUse", "the profile has pages");
      return;
    }
  }
  if (profile->open) {
    c->Reject(BDK_DOMAIN_PROFILE, "inUse", "the profile is open");
    return;
  }
  std::erase(runtime.profiles, profile);
  bdkcore::backend().ProfilesChanged(runtime);
  bdkcore::backend().DeleteProfile(
      runtime, *profile,
      [c](const std::string& code, const std::string& message) {
        if (code.empty()) {
          EmitEvent(nullptr, "profiles.deleted",
                    "{\"id\":" + JsonString(c->StringOr("id", "")) +
                        ",\"source\":\"application\"}");
          c->ResolveNull();
        } else {
          c->Reject(BDK_DOMAIN_PROFILE, code.c_str(), message);
        }
      });
}

}  // namespace

void SetRuntimeInfo(const std::string& resolved_json) {
  std::optional<base::DictValue> resolved = ParseJsonDict(resolved_json);
  if (!resolved) {
    return;
  }
  if (const std::string* name =
          resolved->FindStringByDottedPath("browser.name")) {
    Info().browser_name = *name;
  }
  if (const std::string* scheme =
          resolved->FindStringByDottedPath("browser.internal_scheme")) {
    if (!scheme->empty()) {
      Info().internal_scheme = *scheme;
    }
  }
}

void AddProfilesHandlers(HandlerTable& table) {
  table["runtime.info"] = &RuntimeInfoCommand;
  table["profiles.list"] = &List;
  table["profiles.current"] = &Current;
  table["profiles.create"] = &Create;
  table["profiles.rename"] = &Rename;
  table["profiles.delete"] = &Delete;
}

}  // namespace bdk
