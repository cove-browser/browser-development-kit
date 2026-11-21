// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_SERVICES_SERVICE_COMMON_H_
#define BDK_ENGINE_SERVICES_SERVICE_COMMON_H_

#include <memory>
#include <optional>
#include <string>

#include "base/values.h"
#include "bdk/core/runtime/broker_internal.h"

class Profile;

namespace content {
class WebContents;
}  // namespace content

namespace bdk {

// What the services share: the Chromium profile a command was called on, the
// arguments as a dictionary, and the ways a call is rejected (json_util.h has
// the way a result is written).

// The Chromium profile of a command that names one (every command of the
// passwords and autofill namespaces does), or null when it is not open.
Profile* ProfileOfCall(const bdkcore::BuiltinCall& call);

// The command's arguments. The broker already checked them against the
// command's types, so this is an object.
base::DictValue ArgumentsOf(const bdkcore::BuiltinCall& call);

// Answers `call` with a failure from the broker's own domain.
void RejectCall(const bdkcore::BuiltinCall& call,
                const char* code,
                const std::string& message);

// A command whose backing service does not exist for the profile (a private
// profile has no password store, for example).
void RejectUnavailable(const bdkcore::BuiltinCall& call,
                       const std::string& message);

}  // namespace bdk

#endif  // BDK_ENGINE_SERVICES_SERVICE_COMMON_H_
