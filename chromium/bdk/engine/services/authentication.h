// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_SERVICES_AUTHENTICATION_H_
#define BDK_ENGINE_SERVICES_AUTHENTICATION_H_

#include <string>

#include "base/functional/callback.h"

namespace bdk {

// What a test makes the operating system answer (testing_control.cc).
enum class AuthenticationAnswer { kAsk, kAllow, kDeny };

// Asks the user to authenticate with the operating system (Touch ID or the
// account password on macOS), for a command marked A in services.md. `reason`
// is shown in the system's prompt. `done` runs later on the UI thread with
// whether the user did.
void AuthenticateUser(const std::string& reason,
                      base::OnceCallback<void(bool)> done);

void SetAuthenticationAnswerForTesting(AuthenticationAnswer answer);
// How many times a command asked for authentication since the process began.
int AuthenticationRequestCountForTesting();

// The platform half (authentication_mac.mm): shows the system prompt.
void AuthenticateWithSystem(const std::string& reason,
                            base::OnceCallback<void(bool)> done);

}  // namespace bdk

#endif  // BDK_ENGINE_SERVICES_AUTHENTICATION_H_
