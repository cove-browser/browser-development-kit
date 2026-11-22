// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The operating system's location source, which Chromium's permission check
// and its system provider use (geolocation_provider.cc wraps it). One
// implementation per platform that has one (geolocation_mac.mm); nothing else
// in the engine names a platform API for this.

#ifndef BDK_ENGINE_PROVIDERS_GEOLOCATION_PLATFORM_H_
#define BDK_ENGINE_PROVIDERS_GEOLOCATION_PLATFORM_H_

#include <memory>

namespace device {
class SystemGeolocationSource;
}  // namespace device

namespace bdk {

// The source Chromium itself would install. Called once, on the UI thread,
// after Chromium's threads exist.
std::unique_ptr<device::SystemGeolocationSource>
NewOperatingSystemGeolocationSource();

}  // namespace bdk

#endif  // BDK_ENGINE_PROVIDERS_GEOLOCATION_PLATFORM_H_
