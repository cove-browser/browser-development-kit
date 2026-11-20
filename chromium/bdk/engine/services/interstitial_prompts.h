// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_SERVICES_INTERSTITIAL_PROMPTS_H_
#define BDK_ENGINE_SERVICES_INTERSTITIAL_PROMPTS_H_

namespace content {
class NavigationHandle;
}  // namespace content

namespace bdk {

class EngineContents;

// Security interstitials (ui-requests.md 5.14, T-UIR-05). The interstitial
// page itself is Chromium's and is what a user sees by default; the page
// stays blocked whatever the application does. When a certificate-error or
// reputation interstitial commits, a request of the `certificate_errors` or
// `reputation_warnings` surface goes to the application's handler, which may
// draw its own warning and can complete it only by going back or, where
// Chromium offers it, proceeding with a user gesture. A request that is
// rejected, dropped or released goes back.
//
// Called by the page after every main-frame navigation that finished.
void OnPrimaryNavigationFinished(EngineContents& page,
                                 content::NavigationHandle* handle);

}  // namespace bdk

#endif  // BDK_ENGINE_SERVICES_INTERSTITIAL_PROMPTS_H_
