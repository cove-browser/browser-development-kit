// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <utility>

#include "base/no_destructor.h"
#include "bdk/engine/services/services.h"

namespace bdk {

namespace {

const HandlerTable& Handlers() {
  static const base::NoDestructor<HandlerTable> table([] {
    HandlerTable t;
    AddHistoryHandlers(t);
    AddBookmarksHandlers(t);
    AddDownloadsHandlers(t);
    AddSettingsHandlers(t);
    AddPermissionsHandlers(t);
    AddCookiesHandlers(t);
    AddPrivacyHandlers(t);
    AddProfilesHandlers(t);
    AddSystemHandlers(t);
    AddExtensionsHandlers(t);
    return t;
  }());
  return *table;
}

}  // namespace

TimeBounds ParseTimeRange(const base::DictValue* range) {
  TimeBounds bounds;
  if (!range) {
    return bounds;
  }
  if (std::optional<double> start = range->FindDouble("start")) {
    bounds.begin = base::Time::FromMillisecondsSinceUnixEpoch(
        static_cast<int64_t>(*start));
  }
  if (std::optional<double> end = range->FindDouble("end")) {
    bounds.end =
        base::Time::FromMillisecondsSinceUnixEpoch(static_cast<int64_t>(*end)) +
        base::Milliseconds(1);
  }
  return bounds;
}

void ExecuteBuiltinCommand(bdkcore::Runtime& runtime,
                           const std::shared_ptr<bdkcore::BuiltinCall>& call) {
  const HandlerTable& handlers = Handlers();
  auto it = handlers.find(call->name);
  if (it == handlers.end()) {
    call->control->Reject(
        BDK_DOMAIN_BROKER, BDK_CODE_NOT_IMPLEMENTED,
        "'" + call->name + "' is not implemented by this runtime yet");
    return;
  }
  std::shared_ptr<ServiceCall> service_call = MakeServiceCall(runtime, call);
  if (!service_call) {
    return;
  }
  it->second(service_call);
}

}  // namespace bdk
