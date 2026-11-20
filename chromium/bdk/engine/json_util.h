// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_JSON_UTIL_H_
#define BDK_ENGINE_JSON_UTIL_H_

#include <optional>
#include <string>
#include <string_view>

#include "base/values.h"

namespace bdk {

// The engine's one way to move between JSON text and dictionaries: the
// payloads of UI requests, the arguments of commands and the answers of the
// test hooks all use it.

// `value` as JSON text; "{}" or "[]" when it cannot be written.
std::string ToJson(const base::DictValue& value);
std::string ToJson(const base::ListValue& value);

// The object `text` holds, or nothing when it is not a JSON object. Accepts
// what the core writes (Chromium's JSON extensions: comments, trailing
// commas).
std::optional<base::DictValue> ParseJsonDict(std::string_view text);

}  // namespace bdk

#endif  // BDK_ENGINE_JSON_UTIL_H_
