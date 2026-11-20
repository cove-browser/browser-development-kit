// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/json_util.h"

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"

namespace bdk {

std::string ToJson(const base::DictValue& value) {
  return base::WriteJson(value).value_or("{}");
}

std::string ToJson(const base::ListValue& value) {
  return base::WriteJson(value).value_or("[]");
}

std::optional<base::DictValue> ParseJsonDict(std::string_view text) {
  return base::JSONReader::ReadDict(text, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
}

}  // namespace bdk
