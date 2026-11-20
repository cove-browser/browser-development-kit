// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The test hook for the browser-owned surfaces (bdk_testing.h,
// bdk_testing_ui): scripts the default presenters, drives find in page, and
// reads the site settings a permission decision left.

#include <optional>
#include <string>
#include <utility>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/string_number_conversions.h"
#include "base/values.h"
#include "bdk/core/runtime/core_internal.h"
#include "bdk/engine/bdk_testing.h"
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/ui_find.h"
#include "bdk/engine/ui_presenters.h"
#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/content_settings/core/common/content_settings.h"
#include "components/content_settings/core/common/content_settings_types.h"
#include "url/gurl.h"

namespace bdk {

namespace {

EngineContents* PageOf(const base::DictValue& args) {
  const std::string* handle = args.FindString("contents");
  uint64_t value = 0;
  if (!handle || !base::StringToUint64(*handle, &value)) {
    return nullptr;
  }
  std::shared_ptr<bdkcore::Contents> c =
      bdkcore::contents_of(static_cast<bdk_contents_t>(value));
  return c ? EngineContents::From(*c) : nullptr;
}

std::optional<ContentSettingsType> SettingOf(const std::string& kind) {
  if (kind == "geolocation") {
    return ContentSettingsType::GEOLOCATION;
  }
  if (kind == "notifications") {
    return ContentSettingsType::NOTIFICATIONS;
  }
  if (kind == "camera") {
    return ContentSettingsType::MEDIASTREAM_CAMERA;
  }
  if (kind == "microphone") {
    return ContentSettingsType::MEDIASTREAM_MIC;
  }
  return std::nullopt;
}

const char* SettingName(ContentSetting setting) {
  switch (setting) {
    case CONTENT_SETTING_ALLOW:
      return "allow";
    case CONTENT_SETTING_BLOCK:
      return "block";
    default:
      return "ask";
  }
}

}  // namespace

}  // namespace bdk

BDK_EXPORT bdk_status_t bdk_testing_ui(bdk_runtime_t runtime,
                                       bdk_string_t command,
                                       bdk_string_t args_json,
                                       char* out,
                                       size_t out_size) {
  if (bdk_status_t s = bdkcore::check_thread()) {
    return s;
  }
  if (!bdkcore::runtime_of(runtime)) {
    return BDK_ERR_STALE_HANDLE;
  }
  if (!bdkcore::string_ok(command) || !bdkcore::string_ok(args_json)) {
    return BDK_ERR_INVALID_ARGUMENT;
  }
  const std::string name = bdkcore::str(command);
  std::optional<base::DictValue> parsed = base::JSONReader::ReadDict(
      args_json.length ? bdkcore::str(args_json) : "{}",
      base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  if (!parsed) {
    return BDK_ERR_INVALID_ARGUMENT;
  }
  const base::DictValue& args = *parsed;
  base::DictValue result;

  if (name == "present.script") {
    const std::string* surface = args.FindString("surface");
    const base::DictValue* answer = args.FindDict("answer");
    if (!surface) {
      return BDK_ERR_INVALID_ARGUMENT;
    }
    bdk::ScriptPresenter(*surface,
                         answer ? answer->Clone() : base::DictValue());
  } else if (name == "present.clear") {
    bdk::ClearPresenterScripts();
  } else if (name == "present.record") {
    const std::string* surface = args.FindString("surface");
    if (!surface) {
      return BDK_ERR_INVALID_ARGUMENT;
    }
    bdk::PresenterRecord record = bdk::PresenterRecordOf(*surface);
    result.Set("shown", record.shown);
    result.Set("dismissed", record.dismissed);
    result.Set("payload", record.last_payload);
  } else if (name.starts_with("find.")) {
    bdk::EngineContents* page = bdk::PageOf(args);
    if (!page) {
      return BDK_ERR_STALE_HANDLE;
    }
    bdk::PageFind& find = page->find();
    if (name == "find.present") {
      find.Present();
    } else if (name == "find.start") {
      const std::string* text = args.FindString("text");
      find.Start(text ? *text : std::string(),
                 args.FindBool("matchCase").value_or(false));
    } else if (name == "find.next") {
      find.Next(args.FindBool("forward").value_or(true));
    } else if (name == "find.stop") {
      find.Stop(args.FindBool("clearSelection").value_or(true));
    } else if (name != "find.result") {
      return BDK_ERR_INVALID_ARGUMENT;
    }
    result.Set("ordinal", find.result().ordinal);
    result.Set("count", find.result().count);
    result.Set("presented", find.IsPresented());
  } else if (name == "permission.get") {
    bdk::EngineContents* page = bdk::PageOf(args);
    const std::string* origin = args.FindString("origin");
    const std::string* kind = args.FindString("kind");
    if (!page || !origin || !kind) {
      return BDK_ERR_INVALID_ARGUMENT;
    }
    std::optional<ContentSettingsType> type = bdk::SettingOf(*kind);
    if (!type) {
      return BDK_ERR_INVALID_ARGUMENT;
    }
    std::shared_ptr<bdkcore::Contents> core = page->Core();
    HostContentSettingsMap* map =
        HostContentSettingsMapFactory::GetForProfile(bdk::ProfileOf(*core));
    const GURL url(*origin);
    result.Set("value",
               bdk::SettingName(map->GetContentSetting(url, url, *type)));
  } else {
    return BDK_ERR_INVALID_ARGUMENT;
  }

  if (out && out_size > 0) {
    const std::string text = base::WriteJson(result).value_or("{}");
    if (text.size() + 1 > out_size) {
      return BDK_ERR_INVALID_ARGUMENT;
    }
    // The caller promises `out_size` bytes at `out`.
    base::span<char> buffer = UNSAFE_BUFFERS(base::span<char>(out, out_size));
    buffer.first(text.size()).copy_from(base::span(text));
    buffer[text.size()] = '\0';
  }
  return BDK_OK;
}
