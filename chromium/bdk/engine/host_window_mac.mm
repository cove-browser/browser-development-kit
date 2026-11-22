// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/host_window.h"

#import <AppKit/AppKit.h>

#include <utility>

#include "ui/gfx/mac/coordinate_conversion.h"

namespace bdk {

namespace {

class HostWindowMac : public HostWindow {
 public:
  HostWindowMac(NSWindow* window, ActiveChangedCallback callback)
      : window_(window), callback_(std::move(callback)) {
    NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
    become_key_ = [center addObserverForName:NSWindowDidBecomeKeyNotification
                                      object:window_
                                       queue:nil
                                  usingBlock:^(NSNotification*) {
                                    callback_.Run(true);
                                  }];
    resign_key_ = [center addObserverForName:NSWindowDidResignKeyNotification
                                      object:window_
                                       queue:nil
                                  usingBlock:^(NSNotification*) {
                                    callback_.Run(false);
                                  }];
  }

  ~HostWindowMac() override {
    NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
    [center removeObserver:become_key_];
    [center removeObserver:resign_key_];
  }

  gfx::NativeWindow NativeWindow() override {
    return gfx::NativeWindow(window_);
  }

  gfx::Rect Bounds() override {
    return gfx::ScreenRectFromNSRect(window_.frame);
  }
  bool IsActive() override { return window_.isKeyWindow; }
  bool IsVisible() override { return window_.isVisible; }
  bool IsMinimized() override { return window_.isMiniaturized; }
  bool IsMaximized() override { return window_.isZoomed; }
  bool IsFullscreen() override {
    return (window_.styleMask & NSWindowStyleMaskFullScreen) != 0;
  }
  void Activate() override {
    if (window_.isMiniaturized) {
      [window_ deminiaturize:nil];
    }
    [window_ makeKeyAndOrderFront:nil];
  }
  void SetFrame(const gfx::Rect& bounds) override {
    [window_ setFrame:gfx::ScreenRectToNSRect(bounds) display:YES];
  }
  void Minimize() override { [window_ miniaturize:nil]; }
  void ToggleZoom() override { [window_ zoom:nil]; }
  void SetFullscreen(bool fullscreen) override {
    if (IsFullscreen() != fullscreen) {
      [window_ toggleFullScreen:nil];
    }
  }
  void Close() override {
    // The window's delegate may veto a close the application owns; a window
    // with no close button has no performClose.
    if (window_.styleMask & NSWindowStyleMaskClosable) {
      [window_ performClose:nil];
    } else {
      [window_ close];
    }
  }

 private:
  // The application owns the window; it unregisters it before it goes.
  NSWindow* __weak window_;
  ActiveChangedCallback callback_;
  id __strong become_key_;
  id __strong resign_key_;
};

}  // namespace

// static
std::unique_ptr<HostWindow> HostWindow::Wrap(bdk_native_handle_t window,
                                             ActiveChangedCallback callback) {
  if (window.kind != BDK_NATIVE_NSWINDOW || !window.pointer) {
    return nullptr;
  }
  return std::make_unique<HostWindowMac>((__bridge NSWindow*)window.pointer,
                                         std::move(callback));
}

}  // namespace bdk
