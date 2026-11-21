// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The operating system's side of system.defaultBrowser.*. One implementation
// per platform (system_mac.mm); nothing else in the engine names a platform
// API for this.

#ifndef BDK_ENGINE_SERVICES_SYSTEM_PLATFORM_H_
#define BDK_ENGINE_SERVICES_SYSTEM_PLATFORM_H_

#include <string>

#include "base/functional/callback.h"

namespace bdk {

struct DefaultBrowserStatus {
  // The application is the default handler of http and https.
  bool is_default = false;
  // The application declares http and https, so the system may make it the
  // default.
  bool can_be_default = false;
};

// Reads the status without blocking the UI thread; `done` runs on it.
void GetDefaultBrowserStatus(
    base::OnceCallback<void(DefaultBrowserStatus)> done);

// Asks the system to make the application the default browser. The system
// shows its own confirmation. `done` runs on the UI thread with an empty
// string on success, or the reason it failed.
void RequestDefaultBrowser(base::OnceCallback<void(std::string)> done);

}  // namespace bdk

#endif  // BDK_ENGINE_SERVICES_SYSTEM_PLATFORM_H_
