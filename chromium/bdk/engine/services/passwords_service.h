// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_SERVICES_PASSWORDS_SERVICE_H_
#define BDK_ENGINE_SERVICES_PASSWORDS_SERVICE_H_

#include <memory>

#include "bdk/core/runtime/broker_internal.h"

namespace bdk {

// The passwords.* and autofill.* built-in commands (services.md 5.9 and 5.10,
// T-SVC-04), over Chromium's password store and personal data manager.
//
// Returns false when `call` is not one of them, and does nothing. Otherwise
// the command is answered through call->control, now or later, and this
// returns true. The engine's Backend::ExecuteCommand calls it first; the
// broker has already checked the caller, the capability, the gesture,
// authentication and the arguments.
bool ExecutePasswordsOrAutofillCommand(
    bdkcore::Runtime& rt,
    const std::shared_ptr<bdkcore::BuiltinCall>& call);

}  // namespace bdk

#endif  // BDK_ENGINE_SERVICES_PASSWORDS_SERVICE_H_
