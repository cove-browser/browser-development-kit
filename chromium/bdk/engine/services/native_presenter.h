// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_SERVICES_NATIVE_PRESENTER_H_
#define BDK_ENGINE_SERVICES_NATIVE_PRESENTER_H_

#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "bdk/core/include/bdk/bdk_base.h"

namespace bdk {

// The small native presenters the default UI of a surface uses where
// Chromium's own bubbles cannot be hosted (ui-requests.md section 6: "Fallback:
// small AppKit presenters"). Platform code is in native_presenter_mac.mm; each
// call consults the test answers first (presenter_answers.h), so a test never
// shows a window.

struct AlertButton {
  std::string key;    // what a test answers with
  std::string title;  // shown
};

struct Alert {
  std::string title;
  std::string message;
  // The first is the default button. The last one is Escape's.
  std::vector<AlertButton> buttons;
};

// Shows `alert` as a sheet on `window` (a tagged native window handle) and
// calls `done` with the chosen button's index, or -1 when it is dismissed or
// there is no window to show it in. `surface` names the [ui.surfaces] key, for
// the test answers. `done` runs later, on the UI thread, exactly once.
void PresentAlert(const std::string& surface,
                  Alert alert,
                  bdk_native_handle_t window,
                  base::OnceCallback<void(int)> done);

// Asks where to save a file: a save panel as a sheet on `window`. `done` gets
// the chosen path, or nothing when the user cancels (or there is no window).
// A test's answer is the path, or "cancel".
void PresentSavePanel(
    const std::string& surface,
    const std::string& suggested_name,
    bdk_native_handle_t window,
    base::OnceCallback<void(std::optional<std::string>)> done);

}  // namespace bdk

#endif  // BDK_ENGINE_SERVICES_NATIVE_PRESENTER_H_
