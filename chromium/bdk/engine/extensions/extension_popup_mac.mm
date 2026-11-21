// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/extensions/extension_ui_platform.h"

#import <AppKit/AppKit.h>

#include <utility>

#include "content/public/browser/web_contents.h"
#include "ui/gfx/native_ui_types.h"

// The native view of an extension popup: it holds the popup page's view as its
// only subview, filling it. The application embeds this view (the request's
// secure view); the default presenter puts it in a panel.
@interface BDKExtensionPopupView : NSView
@end

@implementation BDKExtensionPopupView

- (BOOL)isFlipped {
  return YES;
}

- (void)resizeSubviewsWithOldSize:(NSSize)oldSize {
  for (NSView* view in self.subviews) {
    view.frame = self.bounds;
  }
}

@end

// The default presenter's panel. A popup's page takes keyboard input, so the
// panel must be able to become key; it never becomes the main window, which
// stays the application's.
@interface BDKExtensionPopupPanel : NSPanel
@end

@implementation BDKExtensionPopupPanel

- (BOOL)canBecomeKeyWindow {
  return YES;
}

- (BOOL)canBecomeMainWindow {
  return NO;
}

@end

namespace bdk {

namespace {

// Keeps the screen position of a panel inside the visible part of the screen
// it is on.
NSPoint ClampToScreen(NSPoint origin, NSSize size, NSScreen* screen) {
  if (!screen) {
    return origin;
  }
  const NSRect visible = screen.visibleFrame;
  origin.x = MAX(NSMinX(visible), MIN(origin.x, NSMaxX(visible) - size.width));
  origin.y = MAX(NSMinY(visible), MIN(origin.y, NSMaxY(visible) - size.height));
  return origin;
}

class PopupNativeMac : public PopupNative {
 public:
  explicit PopupNativeMac(content::WebContents* contents)
      : view_([[BDKExtensionPopupView alloc]
            initWithFrame:NSMakeRect(0, 0, 25, 25)]) {
    view_.autoresizesSubviews = YES;
    NSView* page = contents->GetNativeView().GetNativeNSView();
    page.frame = view_.bounds;
    page.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [view_ addSubview:page];
  }

  ~PopupNativeMac() override {
    ClosePanel();
    for (NSView* view in [view_.subviews copy]) {
      [view removeFromSuperview];
    }
  }

  bdk_native_handle_t SecureView() override {
    return bdk_native_handle_t{BDK_NATIVE_NSVIEW, (__bridge void*)view_, 0};
  }

  void SetSize(int width, int height) override {
    const NSSize size = NSMakeSize(width, height);
    if (!panel_) {
      [view_ setFrameSize:size];
      return;
    }
    // The panel grows and shrinks downwards and towards its leading edge, the
    // way a popup under its button does, keeping its top and trailing edges.
    const NSRect frame = panel_.frame;
    const NSRect content = [panel_ contentRectForFrameRect:frame];
    const NSRect resized = [panel_
        frameRectForContentRect:NSMakeRect(content.origin.x, content.origin.y,
                                           size.width, size.height)];
    const CGFloat right = NSMaxX(frame);
    const CGFloat top = NSMaxY(frame);
    NSRect updated = resized;
    updated.origin.x = right - resized.size.width;
    updated.origin.y = top - resized.size.height;
    [panel_ setFrame:updated display:YES];
  }

  void ShowInPanel(bdk_native_handle_t window,
                   std::optional<WindowRect> anchor,
                   base::OnceClosure resigned_key) override {
    ClosePanel();
    NSWindow* parent = window.kind == BDK_NATIVE_NSWINDOW && window.pointer
                           ? (__bridge NSWindow*)window.pointer
                           : nil;
    const NSSize size = view_.frame.size;
    panel_ = [[BDKExtensionPopupPanel alloc]
        initWithContentRect:NSMakeRect(0, 0, size.width, size.height)
                  styleMask:NSWindowStyleMaskBorderless |
                            NSWindowStyleMaskNonactivatingPanel
                    backing:NSBackingStoreBuffered
                      defer:NO];
    panel_.releasedWhenClosed = NO;
    panel_.hasShadow = YES;
    panel_.opaque = NO;
    panel_.backgroundColor = NSColor.clearColor;
    panel_.hidesOnDeactivate = NO;
    panel_.contentView = view_;
    view_.wantsLayer = YES;
    view_.layer.cornerRadius = 8;
    view_.layer.masksToBounds = YES;

    // The panel's top trailing edge goes under the anchor, or in the top
    // trailing corner of the window's content, or at the top right of the
    // screen when the page is in no window.
    NSPoint origin;
    NSScreen* screen = parent.screen ?: NSScreen.mainScreen;
    constexpr CGFloat kInset = 8;
    if (parent) {
      const NSRect content = [parent contentRectForFrameRect:parent.frame];
      CGFloat right = NSMaxX(content) - kInset;
      CGFloat top = NSMaxY(content) - kInset;
      if (anchor) {
        right = NSMinX(content) + (*anchor)[0] + (*anchor)[2];
        top = NSMaxY(content) - ((*anchor)[1] + (*anchor)[3]);
      }
      origin = NSMakePoint(right - size.width, top - size.height);
    } else {
      const NSRect visible = screen.visibleFrame;
      origin = NSMakePoint(NSMaxX(visible) - size.width - kInset,
                           NSMaxY(visible) - size.height - kInset);
    }
    [panel_ setFrameOrigin:ClampToScreen(origin, panel_.frame.size, screen)];
    parent_ = parent;
    if (parent) {
      [parent addChildWindow:panel_ ordered:NSWindowAbove];
    }

    // The panel stops being key: the popup is over. Only once it has been
    // key; a panel that never gets the key (the application is not active)
    // does not close itself.
    auto callback =
        std::make_shared<base::OnceClosure>(std::move(resigned_key));
    __block BOOL armed = NO;
    NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
    became_key_ = [center addObserverForName:NSWindowDidBecomeKeyNotification
                                      object:panel_
                                       queue:nil
                                  usingBlock:^(NSNotification*) {
                                    armed = YES;
                                  }];
    resigned_key_ = [center addObserverForName:NSWindowDidResignKeyNotification
                                        object:panel_
                                         queue:nil
                                    usingBlock:^(NSNotification*) {
                                      if (armed && *callback) {
                                        std::move(*callback).Run();
                                      }
                                    }];
    [panel_ makeKeyAndOrderFront:nil];
  }

  void ClosePanel() override {
    if (!panel_) {
      return;
    }
    NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
    [center removeObserver:became_key_];
    [center removeObserver:resigned_key_];
    became_key_ = nil;
    resigned_key_ = nil;
    if (parent_) {
      [parent_ removeChildWindow:panel_];
      parent_ = nil;
    }
    // The view goes back to being its own: the panel must not hold it.
    panel_.contentView = [[NSView alloc] init];
    [panel_ orderOut:nil];
    [panel_ close];
    panel_ = nil;
  }

  bool IsInPanel() const override { return panel_ != nil; }

 private:
  BDKExtensionPopupView* __strong view_;
  BDKExtensionPopupPanel* __strong panel_;
  NSWindow* __weak parent_;
  id __strong became_key_;
  id __strong resigned_key_;
};

}  // namespace

// static
std::unique_ptr<PopupNative> PopupNative::Create(
    content::WebContents* host_contents) {
  return std::make_unique<PopupNativeMac>(host_contents);
}

}  // namespace bdk
