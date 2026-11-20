// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import <LocalAuthentication/LocalAuthentication.h>

#include <utility>

#include "base/functional/bind.h"
#include "base/strings/sys_string_conversions.h"
#include "base/task/bind_post_task.h"
#include "base/task/single_thread_task_runner.h"
#include "bdk/engine/services/authentication.h"

namespace bdk {

void AuthenticateWithSystem(const std::string& reason,
                            base::OnceCallback<void(bool)> done) {
  // The reply arrives on a thread of the framework's choosing.
  __block base::OnceCallback<void(bool)> reply = base::BindPostTask(
      base::SingleThreadTaskRunner::GetCurrentDefault(), std::move(done));
  LAContext* context = [[LAContext alloc] init];
  NSError* error = nil;
  // Touch ID or the account password: the same prompt as any Mac application
  // that reveals a stored secret.
  if (![context canEvaluatePolicy:LAPolicyDeviceOwnerAuthentication
                            error:&error]) {
    std::move(reply).Run(false);
    return;
  }
  [context evaluatePolicy:LAPolicyDeviceOwnerAuthentication
          localizedReason:base::SysUTF8ToNSString(reason)
                    reply:^(BOOL success, NSError*) {
                      if (reply) {
                        std::move(reply).Run(success);
                      }
                    }];
}

}  // namespace bdk
