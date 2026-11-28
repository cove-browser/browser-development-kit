// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_CHROMIUM_MAIN_H_
#define BDK_ENGINE_CHROMIUM_MAIN_H_

#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback_forward.h"

namespace bdk {

struct ChromiumStartOptions {
  ChromiumStartOptions();
  ChromiumStartOptions(const ChromiumStartOptions&);
  ~ChromiumStartOptions();

  // The executable the command line names (argv[0]).
  base::FilePath program;
  // Chromium's user data directory. Every profile lives under it.
  base::FilePath user_data_dir;
  // Further switches, each without the leading dashes ("name" or
  // "name=value").
  std::vector<std::string> switches;
};

// Starts the //chrome browser process inside an application that owns the
// event loop. On return Chromium is initialised: profiles can be loaded and
// WebContents created. Its main loop runs nested inside the application's own
// loop, from a task posted here, until StopChromium(). Called once per
// process, on the main thread. On failure returns false and fills `error`.
bool StartChromium(const ChromiumStartOptions& options, std::string* error);

// True between a successful StartChromium() and the end of StopChromium().
bool IsChromiumRunning();

// Whether the application declares why it uses Bluetooth
// (NSBluetoothAlwaysUsageDescription on macOS). Without that the operating
// system ends a process that opens the Bluetooth adapter, so Web Bluetooth is
// off and the serial chooser lists no Bluetooth ports.
bool ApplicationDeclaresBluetoothUse();

// The keychain access group of the application's Touch ID passkeys,
// "<team>.<bundle identifier>.webauthn" from the running application's
// signature, or empty when it is not signed by a team. Chromium offers Touch
// ID only when the application is entitled to the group (T-UIR-11).
std::string ApplicationTouchIdKeychainGroup();

// Runs `task` on the main thread from the application's run loop, in order.
// Usable when Chromium is not running (before it starts, after it stops),
// from any thread.
void PostToMainThread(base::OnceClosure task);

// Ends the browser main loop and shuts the browser and the content runner
// down. Chromium closes what is still open. `done` runs on the main thread
// once Chromium has stopped. Chromium cannot be started again in this
// process.
void StopChromium(base::OnceClosure done);

}  // namespace bdk

#endif  // BDK_ENGINE_CHROMIUM_MAIN_H_
