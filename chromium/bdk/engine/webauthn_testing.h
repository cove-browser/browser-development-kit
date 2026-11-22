// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_WEBAUTHN_TESTING_H_
#define BDK_ENGINE_WEBAUTHN_TESTING_H_

#include <string>

#include "base/values.h"

namespace bdk {

// The `webauthn.*` verbs of bdk_testing_command (bdk_testing.h): a virtual
// USB security key in place of the real authenticators, so the engine
// scenarios drive passkey requests with no hardware and no Chromium switch.
// Returns false for a verb it does not know or arguments that do not fit.
bool RunWebAuthnTestingCommand(const std::string& name,
                               const base::DictValue& args,
                               base::DictValue& result);

}  // namespace bdk

#endif  // BDK_ENGINE_WEBAUTHN_TESTING_H_
