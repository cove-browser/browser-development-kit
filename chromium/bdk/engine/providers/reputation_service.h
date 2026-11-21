// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The reputation provider behind Chromium's Safe Browsing interstitials
// (T-PRV-05, decision D2). For "default" (BDK's block lists) and "app" (the
// application's provider), BDK supplies Chromium's Safe Browsing service with
// a database manager of its own: a navigation is checked against the core's
// lists (reputation_lists.cc) or the application's check_url, and a verdict
// shows Chromium's own blocking page, which interstitial_prompts.cc routes to
// the reputation_warnings surface. With "google" Chromium's Safe Browsing
// runs as in Chrome, with the application's key.

#ifndef BDK_ENGINE_PROVIDERS_REPUTATION_SERVICE_H_
#define BDK_ENGINE_PROVIDERS_REPUTATION_SERVICE_H_

#include <string>
#include <string_view>

namespace bdk {

// Registers BDK's Safe Browsing service factory when the reputation provider
// is not "google". Before Chromium starts: Chromium creates the service while
// it starts its browser process. Once per process.
void InstallReputationService();

// Replaces the default provider's list with `json` (a bdk-reputation-list).
// The caller has checked where it came from: the list download checks its
// signature first (T-PRV-11); the test hook is the application's. False, with
// a message, for a list that is malformed or not newer.
bool LoadReputationList(std::string_view json, std::string* error);

}  // namespace bdk

#endif  // BDK_ENGINE_PROVIDERS_REPUTATION_SERVICE_H_
