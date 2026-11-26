// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// What the engine sets before and after Chromium starts so that providers are
// the ones the application chose and nothing talks to Google unless it
// configured a Google provider (architecture.md 11.1 and 12, T-PRV-07,
// T-PRV-09). The choice itself is the core's (providers_internal.h); this keeps
// a copy that Chromium's threads may read, because Chromium asks some questions
// (the geolocation key, a location provider) off the UI thread.

#ifndef BDK_ENGINE_PROVIDERS_PROVIDER_SETTINGS_H_
#define BDK_ENGINE_PROVIDERS_PROVIDER_SETTINGS_H_

#include <string>
#include <vector>

class Profile;

namespace bdkcore {
struct Runtime;
}

namespace bdk {

struct ProviderSettings {
  std::string translation = "default";
  std::string reputation = "default";
  std::string geolocation = "system";
  std::string reputation_endpoint;
};

// The choices of the runtime that started last. Any thread; written only by
// SetProviderSettings, before Chromium starts or while it runs no page.
const ProviderSettings& CurrentProviderSettings();
void SetProviderSettings(const bdkcore::Runtime& runtime);

// Switches for Chromium's command line (no leading dashes): the Google-free
// defaults of the audit list (chromium/bdk/engine/providers/README.md) and the
// geolocation mode. A --disable-features or --enable-features entry is merged
// with the others by StartChromium.
std::vector<std::string> ProviderSwitches(const ProviderSettings& settings);

// Records the application's Google key for Chromium. Must run before Chromium
// starts: Chromium allows the key to be set once, before anything has read it.
// False, with a message, when the runtime was built without
// support_external_google_api_key or Chromium already runs with another key.
bool ApplyGoogleKey(const std::string& key, std::string* error);

// Gives Chromium the recorded key, if any. Called at the start of the
// browser's main parts: the key cache reads a feature, so it may not be made
// before Chromium's feature list exists, and nothing has read a key yet.
void InstallGoogleKey();

// The preferences of the audit list, for a profile when the engine opens it
// and for local state once Chromium has started. A preference Chromium has not
// registered is left alone.
void ApplyPrivacyPrefs(Profile* profile);
void ApplyLocalStatePrivacyPrefs();

}  // namespace bdk

#endif  // BDK_ENGINE_PROVIDERS_PROVIDER_SETTINGS_H_
