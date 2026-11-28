// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_HOST_VIEW_H_
#define BDK_ENGINE_HOST_VIEW_H_

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "bdk/core/include/bdk/bdk_base.h"

namespace content {
class WebContents;
}  // namespace content

namespace input {
struct NativeWebKeyboardEvent;
}  // namespace input

namespace bdk {

// One item of a context menu the engine shows itself.
struct MenuItem {
  std::string id;  // empty: a separator, or a submenu when `children` is not
  std::string title;
  bool enabled = true;
  bool checked = false;
  // Not empty: a submenu titled `title` (one extension's context-menu items).
  std::vector<MenuItem> children;
};

// The platform half of a page: the native view the application places, which
// stays the same when the engine replaces the page's WebContents (restore,
// discard). Each platform implements Create() in its own file
// (host_view_mac.mm); nothing else in the engine names a toolkit type.
class HostView {
 public:
  static std::unique_ptr<HostView> Create();

  virtual ~HostView() = default;

  // Shows `contents`' view inside the host view, filling it.
  virtual void SetWebContents(content::WebContents* contents) = 0;
  virtual bdk_native_handle_t NativeHandle() = 0;
  // Whether the host view is in a window.
  virtual bool IsInWindow() = 0;
  // Whether the host view is shown: in a window, and neither it nor a view it
  // is in is hidden. Whether the window is on screen is left to Chromium's
  // own occlusion tracking: a window ordered in after the view was added
  // sends no notice a host view could follow.
  virtual bool IsShown() = 0;
  // The native window the host view is in, as a tagged handle, or a handle of
  // kind 0 when it is in none.
  virtual bdk_native_handle_t Window() = 0;
  // Called after the host view moved into a window or out of one (the
  // application attached or detached it), from inside the application's own
  // call: do not do work that Chromium or the application must not see
  // half-finished.
  virtual void SetWindowChangedCallback(base::RepeatingClosure callback) = 0;

  // The key event as a tagged native handle, borrowed for the callback.
  static bdk_native_handle_t KeyEvent(
      const input::NativeWebKeyboardEvent& event);
  // What the platform does with a key no one handled: on macOS the main menu
  // gets it as a key equivalent. Returns whether something handled it.
  static bool RedispatchUnhandledKey(
      const input::NativeWebKeyboardEvent& event);

  // The default context-menu presenter: shows `items` at `x`, `y` (view
  // coordinates, origin top-left) and calls `chosen` with the chosen item's
  // id, or not at all if the menu is dismissed.
  virtual void ShowMenu(
      const std::vector<MenuItem>& items,
      double x,
      double y,
      base::OnceCallback<void(const std::string&)> chosen) = 0;

  // The default fullscreen presenter: makes the window the view is in
  // fullscreen, or leaves fullscreen if the engine made it so.
  virtual void SetWindowFullscreen(bool fullscreen) = 0;

  // The view's frame in its window, in window coordinates with the origin at
  // the top left. All zero when the view is not in a window.
  virtual std::array<double, 4> FrameInWindow() = 0;
  // A line of text in a bar above the page, or none for an empty `text`:
  // BDK's app window shows an app page's origin there while the page is out
  // of the app's scope (web-apps.md 4.2).
  virtual void SetOriginNotice(const std::string& text) = 0;
};

}  // namespace bdk

#endif  // BDK_ENGINE_HOST_VIEW_H_
