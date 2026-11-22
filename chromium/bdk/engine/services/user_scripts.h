// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Persistent user scripts (services.md section 5.16, T-AUT-03): scripts and
// CSS that run in every page of a profile whose URL matches, without the
// caller driving each page. The core keeps the registry (per profile, and
// removes a session's scripts when it is revoked) and tells the engine through
// Backend::UserScriptsChanged; this applies them through each page's internal
// DevTools protocol client (page_protocol.h).

#ifndef BDK_ENGINE_SERVICES_USER_SCRIPTS_H_
#define BDK_ENGINE_SERVICES_USER_SCRIPTS_H_

#include <vector>

#include "bdk/core/runtime/automation_internal.h"

class Profile;

namespace content {
class WebContents;
}

namespace bdk {

// The profile's scripts are now `scripts` (every one, in registration order):
// registers what is new, replaces what changed and removes what is gone, in
// every page of the profile. An empty list detaches the machinery from the
// pages. Scripts run in documents loaded from now on, not in the ones already
// loaded (as Chrome's own user scripts API).
void ApplyUserScripts(Profile* profile,
                      const std::vector<bdkcore::UserScript>& scripts);

// A page was created or got a new WebContents (a restore or a discard): gives
// it the scripts of its profile, if the profile has any. Does nothing, and
// costs the page nothing, otherwise.
void ApplyUserScriptsToPage(content::WebContents* web_contents);

}  // namespace bdk

#endif  // BDK_ENGINE_SERVICES_USER_SCRIPTS_H_
