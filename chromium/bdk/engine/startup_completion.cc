// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/startup_completion.h"

#include <memory>

#include "base/no_destructor.h"
#include "chrome/browser/after_startup_task_utils.h"

namespace bdk {

namespace {

std::unique_ptr<AfterStartupTaskUtils::StartupInProgressRef>& Hold() {
  static base::NoDestructor<
      std::unique_ptr<AfterStartupTaskUtils::StartupInProgressRef>>
      hold;
  return *hold;
}

}  // namespace

void HoldStartupUntilRuntimeStarts() {
  Hold() = AfterStartupTaskUtils::RegisterStartupInProgressRef(
      StartupIsCompleteReason::kStartupRegistrationDone);
}

void RuntimeStarted() {
  Hold().reset();
}

}  // namespace bdk
