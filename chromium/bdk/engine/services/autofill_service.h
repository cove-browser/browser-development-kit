// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_SERVICES_AUTOFILL_SERVICE_H_
#define BDK_ENGINE_SERVICES_AUTOFILL_SERVICE_H_

#include <memory>

#include "bdk/core/runtime/broker_internal.h"

namespace bdk {

// The autofill.* built-in commands (services.md 5.10): addresses and cards
// over Chromium's personal data manager. False when `call` is not one of them;
// see ExecutePasswordsOrAutofillCommand (passwords_service.h).
bool ExecuteAutofillCommand(bdkcore::Runtime& rt,
                            const std::shared_ptr<bdkcore::BuiltinCall>& call);

}  // namespace bdk

#endif  // BDK_ENGINE_SERVICES_AUTOFILL_SERVICE_H_
