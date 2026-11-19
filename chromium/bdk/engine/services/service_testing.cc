// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The services' test hooks (bdk_testing.h): they read what Chromium holds and
// start the captures no command reaches yet. Like the other hooks they act as
// the application on its own pages and profiles.

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "bdk/core/runtime/core_internal.h"
#include "bdk/engine/bdk_testing.h"
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/services/capture_service.h"
#include "bdk/engine/services/permissions.h"
#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/web_contents.h"
#include "url/gurl.h"

namespace bdk {

namespace {

// 0 nothing started or still running, 1 written, 2 failed.
uint32_t g_capture_state = 0;

bdk_status_t CopyOut(const std::string& text, char* out, size_t n) {
  if (!out || n == 0) {
    return BDK_ERR_INVALID_ARGUMENT;
  }
  const size_t length = std::min(text.size(), n - 1);
  std::copy_n(text.data(), length, out);
  out[length] = '\0';
  return BDK_OK;
}

bdk_status_t PrefJson(Profile* profile,
                      const std::string& name,
                      char* out,
                      size_t n) {
  const PrefService::Preference* pref =
      profile->GetPrefs()->FindPreference(name);
  if (!pref) {
    return BDK_ERR_FAILED;
  }
  return CopyOut(base::WriteJson(*pref->GetValue()).value_or("null"), out, n);
}

bdk_status_t ContentSetting(Profile* profile,
                            const std::string& url,
                            const std::string& kind,
                            char* out,
                            size_t n) {
  const PermissionKind* permission = FindPermissionKind(kind);
  if (!permission) {
    return BDK_ERR_INVALID_ARGUMENT;
  }
  const GURL origin(url);
  return CopyOut(PermissionValueName(
                     HostContentSettingsMapFactory::GetForProfile(profile)
                         ->GetContentSetting(origin, origin, permission->type)),
                 out, n);
}

bdk_status_t Capture(bdkcore::Contents& page,
                     bool pdf,
                     const std::string& path) {
  g_capture_state = 0;
  const base::FilePath file = base::FilePath::FromUTF8Unsafe(path);
  content::WebContents* contents = EngineContents::From(page)->web_contents();
  CaptureCallback done = base::BindOnce(
      [](base::FilePath file, std::optional<std::vector<uint8_t>> bytes,
         std::string error) {
        if (!bytes) {
          g_capture_state = 2;
          return;
        }
        WriteCaptureFile(file, std::move(*bytes),
                         base::BindOnce([](bool written) {
                           g_capture_state = written ? 1 : 2;
                         }));
      },
      file);
  if (pdf) {
    CapturePdf(contents, std::move(done));
  } else {
    CaptureScreenshot(contents, std::move(done));
  }
  return BDK_OK;
}

}  // namespace

}  // namespace bdk

extern "C" {

BDK_EXPORT bdk_status_t bdk_testing_services(uint32_t op,
                                             bdk_contents_t handle,
                                             bdk_string_t a,
                                             bdk_string_t b,
                                             char* out,
                                             size_t n) {
  if (bdk_status_t s = bdkcore::check_thread()) {
    return s;
  }
  if (op == BDK_TESTING_SERVICES_CAPTURE_STATE) {
    return bdk::CopyOut(std::to_string(bdk::g_capture_state), out, n);
  }
  std::shared_ptr<bdkcore::Contents> page = bdkcore::contents_of(handle);
  if (!page || page->dead || !page->data) {
    return BDK_ERR_STALE_HANDLE;
  }
  if (!bdkcore::string_ok(a) || !bdkcore::string_ok(b)) {
    return BDK_ERR_INVALID_ARGUMENT;
  }
  switch (op) {
    case BDK_TESTING_SERVICES_PREF_JSON:
      return bdk::PrefJson(bdk::ProfileOf(*page), bdkcore::str(a), out, n);
    case BDK_TESTING_SERVICES_CONTENT_SETTING:
      return bdk::ContentSetting(bdk::ProfileOf(*page), bdkcore::str(a),
                                 bdkcore::str(b), out, n);
    case BDK_TESTING_SERVICES_CAPTURE_PNG:
    case BDK_TESTING_SERVICES_CAPTURE_PDF:
      return bdk::Capture(*page, op == BDK_TESTING_SERVICES_CAPTURE_PDF,
                          bdkcore::str(a));
    default:
      return BDK_ERR_INVALID_ARGUMENT;
  }
}

}  // extern "C"
