// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Geolocation from the application's provider (T-PRV-04, provider = "app").
// Chromium asks the embedder for a location provider
// (ContentBrowserClient::OverrideSystemLocationProvider) on the geolocation
// thread; this one starts the application's watch on the UI thread, where the
// core runs, once a page's permission is granted, and hands each position
// back. "system" and "google" use Chromium's own providers, in the mode
// ProviderSwitches sets.

#ifndef BDK_ENGINE_PROVIDERS_GEOLOCATION_PROVIDER_H_
#define BDK_ENGINE_PROVIDERS_GEOLOCATION_PROVIDER_H_

#include <memory>

namespace device {
class LocationProvider;
}

namespace bdk {

// The application's provider when geolocation is configured "app", or null
// for Chromium's own. Any thread.
std::unique_ptr<device::LocationProvider> CreateApplicationLocationProvider();

// Chromium checks the operating system's location permission before any
// provider starts, so BDK installs the permission Chromium reads: granted while
// the runtime that started last uses the application's provider, the
// operating system's otherwise (macOS; elsewhere Chromium's own is kept).
// Called as Chromium's browser process starts, before Chromium would install
// its own; GeolocationProviderChanged tells it the runtime's choice changed.
void InstallGeolocationPermission();
void GeolocationProviderChanged();

}  // namespace bdk

#endif  // BDK_ENGINE_PROVIDERS_GEOLOCATION_PROVIDER_H_
