// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// bdk_testing_control (bdk_testing.h): the test hooks of the services and the
// default presenters, behind one exported function so the framework's export
// list needs one name for all of them.

#include <algorithm>
#include <optional>
#include <string>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/values.h"
#include "bdk/core/runtime/core_internal.h"
#include "bdk/engine/bdk_testing.h"
#include "bdk/engine/services/authentication.h"
#include "bdk/engine/services/download_prompts.h"
#include "bdk/engine/services/presenter_answers.h"
#include "bdk/engine/testing.h"

namespace bdk {

namespace {

bdk_status_t Fill(const std::string& text, char* out, size_t out_size) {
  if (out && out_size > 0) {
    const size_t n = std::min(text.size(), out_size - 1);
    base::span<char> dst = UNSAFE_BUFFERS(base::span<char>(out, out_size));
    dst.first(n).copy_from(base::span(text).first(n));
    dst[n] = '\0';
  }
  return BDK_OK;
}

}  // namespace

}  // namespace bdk

extern "C" {

BDK_EXPORT bdk_status_t bdk_testing_control(bdk_runtime_t runtime,
                                            bdk_string_t verb,
                                            bdk_string_t arguments_json,
                                            char* out,
                                            size_t out_size) {
  if (bdk_status_t s = bdkcore::check_thread()) {
    return s;
  }
  if (!bdkcore::runtime_of(runtime)) {
    return BDK_ERR_STALE_HANDLE;
  }
  if (!bdkcore::string_ok(verb) || !bdkcore::string_ok(arguments_json)) {
    return BDK_ERR_INVALID_ARGUMENT;
  }
  const std::string name = bdkcore::str(verb);
  std::optional<base::DictValue> args = base::JSONReader::ReadDict(
      bdkcore::str(arguments_json), base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  if (!args) {
    args = base::DictValue();
  }
  const std::string* answer = args->FindString("answer");

  if (name == "authentication") {
    // What the operating system's prompt answers: "allow", "deny" or "ask"
    // (the real prompt).
    bdk::AuthenticationAnswer value = bdk::AuthenticationAnswer::kAsk;
    if (answer && *answer == "allow") {
      value = bdk::AuthenticationAnswer::kAllow;
    } else if (answer && *answer == "deny") {
      value = bdk::AuthenticationAnswer::kDeny;
    }
    bdk::SetAuthenticationAnswerForTesting(value);
    return BDK_OK;
  }
  if (name == "authentication.count") {
    return bdk::Fill(
        std::to_string(bdk::AuthenticationRequestCountForTesting()), out,
        out_size);
  }
  if (name == "presenter") {
    // What a default presenter answers instead of showing its native UI:
    // {"surface": "downloads", "answer": "accept"}; an empty answer removes it.
    const std::string* surface = args->FindString("surface");
    if (!surface) {
      return BDK_ERR_INVALID_ARGUMENT;
    }
    bdk::SetPresenterAnswerForTesting(*surface, answer ? *answer : "");
    return BDK_OK;
  }
  if (name == "presenter.count") {
    const std::string* surface = args->FindString("surface");
    if (!surface) {
      return BDK_ERR_INVALID_ARGUMENT;
    }
    return bdk::Fill(std::to_string(bdk::PresenterUseCountForTesting(*surface)),
                     out, out_size);
  }
  if (name == "downloads.ask") {
    // Makes every loaded profile ask where to save, as the setting does.
    const std::optional<bool> on = args->FindBool("on");
    bdk::AskWhereToSaveForTesting(on.value_or(true));
    return BDK_OK;
  }
  if (name == "downloads.danger") {
    bdk::ForceDangerousDownloadsForTesting(args->FindBool("on").value_or(true));
    return BDK_OK;
  }
  if (name == "script_page") {
    // A page with content: {"url", "body", "contentType", "headers"}.
    const std::string* url = args->FindString("url");
    const std::string* body = args->FindString("body");
    if (!url || !body) {
      return BDK_ERR_INVALID_ARGUMENT;
    }
    const std::string* content_type = args->FindString("contentType");
    const std::string* headers = args->FindString("headers");
    bdk::ScriptPageForTesting(*url, *body, content_type ? *content_type : "",
                              headers ? *headers : "");
    return BDK_OK;
  }
  return BDK_ERR_INVALID_ARGUMENT;
}

}  // extern "C"
