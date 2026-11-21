// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/services/service_common.h"

#include <utility>

#include "bdk/core/include/bdk/bdk_ids.h"
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/json_util.h"

namespace bdk {

Profile* ProfileOfCall(const bdkcore::BuiltinCall& call) {
  if (!call.profile || !call.profile->data) {
    return nullptr;
  }
  return static_cast<EngineProfile*>(call.profile->data.get())->profile;
}

base::DictValue ArgumentsOf(const bdkcore::BuiltinCall& call) {
  return ParseJsonDict(call.arguments).value_or(base::DictValue());
}

void RejectCall(const bdkcore::BuiltinCall& call,
                const char* code,
                const std::string& message) {
  call.control->Reject(BDK_DOMAIN_BROKER, code, message);
}

void RejectUnavailable(const bdkcore::BuiltinCall& call,
                       const std::string& message) {
  RejectCall(call, "unavailable", message);
}

}  // namespace bdk
