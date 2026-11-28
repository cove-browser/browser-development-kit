// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_DEVICE_CHOOSER_TESTING_H_
#define BDK_ENGINE_DEVICE_CHOOSER_TESTING_H_

#include <string>

#include "base/values.h"

namespace bdk {

class EngineContents;

// The `chooser.*` verbs of bdk_testing_command (bdk_testing.h): a chooser
// controller of devices the scenario names, shown through the same session as
// WebUSB's, WebHID's, Web Serial's and Web Bluetooth's (ui_device_chooser.h),
// so a scenario can choose a device with no hardware.
//   chooser.open {"contents", "kind", "devices": [names]}
//   chooser.add {"name"}, chooser.remove {"name"}
//   chooser.result: {"ended", "chosen": name (absent when cancelled)}
// Returns false for a verb it does not know or arguments that do not fit.
bool RunChooserTestingCommand(const std::string& name,
                              EngineContents* page,
                              const base::DictValue& args,
                              base::DictValue& result);

}  // namespace bdk

#endif  // BDK_ENGINE_DEVICE_CHOOSER_TESTING_H_
