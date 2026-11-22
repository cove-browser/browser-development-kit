// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_AUTOMATION_AUTOMATION_HOST_SERVICE_H_
#define BDK_ENGINE_AUTOMATION_AUTOMATION_HOST_SERVICE_H_

#include <memory>

#include "bdk/engine/automation/automation_host.mojom.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"

namespace bdk {

// The utility process side of the automation host: V8 with one isolate for the
// process and one context per task. Called by the utility process's service
// factory (chrome/utility/services.cc, through a BDK patch); the process lives
// until the browser closes `receiver`.
std::unique_ptr<mojom::AutomationHost> RunAutomationHost(
    mojo::PendingReceiver<mojom::AutomationHost> receiver);

}  // namespace bdk

#endif  // BDK_ENGINE_AUTOMATION_AUTOMATION_HOST_SERVICE_H_
