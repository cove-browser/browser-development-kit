// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// When Chromium's start-up counts as complete (T-ADP-10). Chromium holds its
// after-start-up and best-effort work until then (AfterStartupTaskUtils), and
// Chrome ends start-up when a visible tab of its first window has loaded. A BDK
// application opens no window of Chromium's, so without this start-up ended
// only at Chromium's three-minute failsafe. With the start switches of
// chromium_main (the start-up delay does not wait for a visible tab), it ends
// at the first idle of Chromium's main loop once the runtime's start has
// completed.

#ifndef BDK_ENGINE_STARTUP_COMPLETION_H_
#define BDK_ENGINE_STARTUP_COMPLETION_H_

namespace bdk {

// Holds start-up open. Called as Chromium's browser threads are created.
void HoldStartupUntilRuntimeStarts();

// The runtime's start completed: start-up may end. Later calls do nothing.
void RuntimeStarted();

}  // namespace bdk

#endif  // BDK_ENGINE_STARTUP_COMPLETION_H_
