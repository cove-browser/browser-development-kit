// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/providers/provider_settings.h"

#include <optional>

#include "base/containers/span.h"
#include "base/no_destructor.h"
#include "base/values.h"
#include "bdk/core/runtime/providers_internal.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "components/prefs/pref_service.h"
#include "google_apis/buildflags.h"
#include "google_apis/google_api_keys.h"

namespace bdk {

namespace {

ProviderSettings& Settings() {
  static base::NoDestructor<ProviderSettings> settings;
  return *settings;
}

// The key Chromium was given, once per process.
std::optional<std::string>& KeyGiven() {
  static base::NoDestructor<std::optional<std::string>> key;
  return *key;
}

// Preferences of the audit list (providers/README.md), by the name Chromium
// registers them under. Strings rather than each component's constant keep
// the engine from depending on every component that declares one; a name
// that is not registered (the component is not built) is skipped.
struct BoolPref {
  const char* name;
  bool value;
};

constexpr BoolPref kProfilePrefs[] = {
    // Enhanced protection sends URLs to Google in real time; extended
    // reporting uploads pages. Standard protection checks BDK's lists.
    {"safebrowsing.enhanced", false},
    {"safebrowsing.scout_reporting_enabled", false},
    {"safebrowsing.extended_reporting_opt_in_allowed", false},
    // URL-keyed data collection gates optimization-guide hint fetches.
    {"url_keyed_anonymized_data_collection.enabled", false},
    {"signin.allowed_on_next_startup", false},
    {"spellcheck.use_spelling_service", false},
    {"profile.password_manager_leak_detection", false},
    // Navigation error suggestions and captive-portal probes go to Google.
    {"alternate_error_pages.enabled", false},
    {"privacy_sandbox.m1.topics_enabled", false},
    {"privacy_sandbox.m1.fledge_enabled", false},
    {"privacy_sandbox.m1.ad_measurement_enabled", false},
    {"media_router.enable_media_router", false},
};

constexpr BoolPref kLocalStatePrefs[] = {
    {"user_experience_metrics.reporting_enabled", false},
    {"network_time.network_time_queries_enabled", false},
    {"component_updates.component_updates_enabled", false},
};

void SetBools(PrefService* prefs, base::span<const BoolPref> list) {
  if (!prefs) {
    return;
  }
  for (const BoolPref& pref : list) {
    if (prefs->FindPreference(pref.name)) {
      prefs->SetBoolean(pref.name, pref.value);
    }
  }
}

}  // namespace

const ProviderSettings& CurrentProviderSettings() {
  return Settings();
}

void SetProviderSettings(const bdkcore::Runtime& runtime) {
  ProviderSettings& s = Settings();
  s.translation =
      bdkcore::provider_choice(runtime, bdkcore::Service::kTranslation)
          .provider;
  s.reputation =
      bdkcore::provider_choice(runtime, bdkcore::Service::kReputation).provider;
  s.reputation_endpoint =
      bdkcore::provider_choice(runtime, bdkcore::Service::kReputation).endpoint;
  s.geolocation =
      bdkcore::provider_choice(runtime, bdkcore::Service::kGeolocation)
          .provider;
}

std::vector<std::string> ProviderSwitches(const ProviderSettings& settings) {
  std::vector<std::string> switches = {
      // Security data ships with the runtime (T-PRV-06, decision D5): the
      // component updater would ask update.googleapis.com, and its signed
      // responses cannot come from a mirror.
      "disable-component-update",
      "disable-sync",
      "disable-domain-reliability",
      "allow-browser-signin=false",
      // Network time asks clients2.google.com at start-up; translate would
      // send page text to Google for the always-translate list as soon as a
      // key exists; optimization-guide hints and models come from Google.
      // Found by the network audit (tests_engine_privacy.c): the omnibox's AI
      // Mode eligibility check asks www.google.com/async/folae, the search
      // preconnector opens www.google.com at start-up, and the translate
      // ranker downloads its model from www.gstatic.com.
      "disable-features=NetworkTimeServiceQuerying,TranslateTrigger,"
      "OptimizationHints,OptimizationTargetPrediction,AimEnabled,"
      "AimServerEligibilityEnabled,PreconnectToSearch,TranslateRankerQuery,"
      "TranslateRankerEnforcement",
  };
  // On macOS Chromium defaults to the hybrid mode, which can fall back to
  // Google's network provider (device_features.cc). "system" is the operating
  // system's provider only; "google" keeps Chrome's behaviour; "app" is the
  // application's (BdkContentBrowserClient::OverrideSystemLocationProvider).
  if (settings.geolocation == "system") {
    switches.push_back(
        "enable-features=LocationProviderManager:LocationProviderManagerMode/"
        "PlatformOnly");
  }
  return switches;
}

bool ApplyGoogleKey(const std::string& key, std::string* error) {
  std::optional<std::string>& given = KeyGiven();
  if (given) {
    if (*given == key) {
      return true;
    }
    *error = "Chromium already runs in this process with another Google key";
    return false;
  }
  if (key.empty()) {
    given = key;
    return true;
  }
#if BUILDFLAG(SUPPORT_EXTERNAL_GOOGLE_API_KEY)
  // Chromium takes it in InstallGoogleKey(), once its features exist.
  given = key;
  return true;
#else
  *error =
      "this runtime was built without support_external_google_api_key, so it "
      "cannot take the application's Google key";
  return false;
#endif
}

void InstallGoogleKey() {
#if BUILDFLAG(SUPPORT_EXTERNAL_GOOGLE_API_KEY)
  const std::optional<std::string>& given = KeyGiven();
  if (given && !given->empty()) {
    google_apis::InitializeAndOverrideAPIKey(*given);
  }
#endif
}

void ApplyPrivacyPrefs(Profile* profile) {
  SetBools(profile ? profile->GetPrefs() : nullptr, kProfilePrefs);
}

void ApplyLocalStatePrivacyPrefs() {
  PrefService* local =
      g_browser_process ? g_browser_process->local_state() : nullptr;
  SetBools(local, kLocalStatePrefs);
  if (local && local->FindPreference("dns_over_https.mode")) {
    local->SetString("dns_over_https.mode", "off");
  }
}

}  // namespace bdk
