// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/services/authentication.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/task/single_thread_task_runner.h"

namespace bdk {

namespace {

AuthenticationAnswer g_answer = AuthenticationAnswer::kAsk;
int g_requests = 0;

}  // namespace

void SetAuthenticationAnswerForTesting(AuthenticationAnswer answer) {
  g_answer = answer;
}

int AuthenticationRequestCountForTesting() {
  return g_requests;
}

void AuthenticateUser(const std::string& reason,
                      base::OnceCallback<void(bool)> done) {
  ++g_requests;
  if (g_answer != AuthenticationAnswer::kAsk) {
    // A test's answer arrives later, as the system's would.
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(std::move(done),
                                  g_answer == AuthenticationAnswer::kAllow));
    return;
  }
  AuthenticateWithSystem(reason, std::move(done));
}

}  // namespace bdk
