// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/providers/geolocation_platform.h"

#include "services/device/public/cpp/geolocation/system_geolocation_source_apple.h"

namespace bdk {

std::unique_ptr<device::SystemGeolocationSource>
NewOperatingSystemGeolocationSource() {
  return std::make_unique<device::SystemGeolocationSourceApple>();
}

}  // namespace bdk
