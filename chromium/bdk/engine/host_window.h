// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_HOST_WINDOW_H_
#define BDK_ENGINE_HOST_WINDOW_H_

#include <memory>

#include "base/functional/callback.h"
#include "bdk/core/include/bdk/bdk_base.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/native_ui_types.h"

namespace bdk {

// The platform half of a registered window: the application's native window,
// which BDK reads and never owns. It gives Chromium's `BrowserWindow` what
// Chromium asks of a window (bounds, activation, visibility) from the window
// the application made. Each platform implements Wrap() in its own file
// (host_window_mac.mm); nothing else in the engine names a toolkit type for a
// window.
class HostWindow {
 public:
  // Called when the window becomes or stops being the key window.
  using ActiveChangedCallback = base::RepeatingCallback<void(bool active)>;

  // Wraps the native window `window` (BDK_NATIVE_NSWINDOW on macOS), or
  // returns null for a handle of another kind.
  static std::unique_ptr<HostWindow> Wrap(bdk_native_handle_t window,
                                          ActiveChangedCallback callback);

  virtual ~HostWindow() = default;

  virtual gfx::NativeWindow NativeWindow() = 0;
  // In screen coordinates, origin top left.
  virtual gfx::Rect Bounds() = 0;
  virtual bool IsActive() = 0;
  virtual bool IsVisible() = 0;
  virtual bool IsMinimized() = 0;
  virtual bool IsMaximized() = 0;
  virtual bool IsFullscreen() = 0;
  // Makes the window the key window and brings it to the front.
  virtual void Activate() = 0;
};

}  // namespace bdk

#endif  // BDK_ENGINE_HOST_WINDOW_H_
