// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/host_view.h"

#import <AppKit/AppKit.h>

#include <utility>

#include "base/strings/sys_string_conversions.h"
#include "components/input/native_web_keyboard_event.h"
#include "content/public/browser/web_contents.h"
#include "ui/gfx/native_ui_types.h"

// The view the application places. It holds the page's WebContents view as
// its only subview, sized to fill it, so the page can be replaced without the
// application noticing. AppKit makes it the accessibility parent of the page,
// and Chromium follows it for visibility: moving it between windows or hiding
// it hides or shows the page.
@interface BDKContentsHostView : NSView
// Runs after the view moved into a window or out of one.
@property(nonatomic, copy) void (^windowChanged)(void);
@end

@implementation BDKContentsHostView

@synthesize windowChanged = _windowChanged;

- (void)viewDidMoveToWindow {
  [super viewDidMoveToWindow];
  if (self.windowChanged) {
    self.windowChanged();
  }
}

- (BOOL)isFlipped {
  return YES;
}

- (void)resizeSubviewsWithOldSize:(NSSize)oldSize {
  for (NSView* view in self.subviews) {
    view.frame = self.bounds;
  }
}

@end

// Calls back with the chosen item of the default context menu.
@interface BDKMenuTarget : NSObject
@property(nonatomic, copy) void (^chosen)(NSString*);
- (void)choose:(NSMenuItem*)item;
@end

@implementation BDKMenuTarget
@synthesize chosen = _chosen;
- (void)choose:(NSMenuItem*)item {
  if (self.chosen) {
    self.chosen(item.representedObject);
  }
}
@end

namespace bdk {

namespace {

class HostViewMac : public HostView {
 public:
  HostViewMac()
      : view_([[BDKContentsHostView alloc]
            initWithFrame:NSMakeRect(0, 0, 800, 600)]) {
    view_.autoresizesSubviews = YES;
  }

  ~HostViewMac() override {
    if (window_made_fullscreen_ && view_.window &&
        (view_.window.styleMask & NSWindowStyleMaskFullScreen)) {
      [view_.window toggleFullScreen:nil];
    }
    view_.windowChanged = nil;
    for (NSView* view in [view_.subviews copy]) {
      [view removeFromSuperview];
    }
  }

  void SetWebContents(content::WebContents* contents) override {
    NSView* page = contents->GetNativeView().GetNativeNSView();
    if (page.superview == view_) {
      return;
    }
    for (NSView* view in [view_.subviews copy]) {
      [view removeFromSuperview];
    }
    page.frame = view_.bounds;
    page.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [view_ addSubview:page];
  }

  bdk_native_handle_t NativeHandle() override {
    return bdk_native_handle_t{BDK_NATIVE_NSVIEW, (__bridge void*)view_, 0};
  }

  bool IsInWindow() override { return view_.window != nil; }

  bdk_native_handle_t Window() override {
    NSWindow* window = view_.window;
    return window ? bdk_native_handle_t{BDK_NATIVE_NSWINDOW,
                                        (__bridge void*)window, 0}
                  : bdk_native_handle_t{};
  }

  void SetWindowChangedCallback(base::RepeatingClosure callback) override {
    view_.windowChanged = ^{
      callback.Run();
    };
  }

  void ShowMenu(const std::vector<MenuItem>& items,
                double x,
                double y,
                base::OnceCallback<void(const std::string&)> chosen) override {
    if (!view_.window) {
      return;
    }
    NSMenu* menu = [[NSMenu alloc] initWithTitle:@""];
    menu.autoenablesItems = NO;
    BDKMenuTarget* target = [[BDKMenuTarget alloc] init];
    __block base::OnceCallback<void(const std::string&)> callback =
        std::move(chosen);
    target.chosen = ^(NSString* item_id) {
      if (callback) {
        std::move(callback).Run(base::SysNSStringToUTF8(item_id));
      }
    };
    for (const MenuItem& item : items) {
      if (item.id.empty()) {
        [menu addItem:[NSMenuItem separatorItem]];
        continue;
      }
      NSMenuItem* menu_item =
          [[NSMenuItem alloc] initWithTitle:base::SysUTF8ToNSString(item.title)
                                     action:@selector(choose:)
                              keyEquivalent:@""];
      menu_item.target = target;
      menu_item.enabled = item.enabled;
      menu_item.representedObject = base::SysUTF8ToNSString(item.id);
      [menu addItem:menu_item];
    }
    // The target is weak in NSMenuItem; the menu keeps it for its life.
    objc_target_ = target;
    [menu popUpMenuPositioningItem:nil
                        atLocation:NSMakePoint(x, y)
                            inView:view_];
  }

  void SetWindowFullscreen(bool fullscreen) override {
    NSWindow* window = view_.window;
    if (!window) {
      return;
    }
    const bool is_fullscreen =
        (window.styleMask & NSWindowStyleMaskFullScreen) != 0;
    if (fullscreen && !is_fullscreen) {
      window_made_fullscreen_ = true;
      [window toggleFullScreen:nil];
    } else if (!fullscreen && is_fullscreen && window_made_fullscreen_) {
      window_made_fullscreen_ = false;
      [window toggleFullScreen:nil];
    }
  }

  std::array<double, 4> FrameInWindow() override {
    NSWindow* window = view_.window;
    if (!window) {
      return {0, 0, 0, 0};
    }
    NSRect rect = [view_ convertRect:view_.bounds toView:nil];
    const CGFloat content_height = window.contentView.bounds.size.height;
    return {rect.origin.x, content_height - rect.origin.y - rect.size.height,
            rect.size.width, rect.size.height};
  }

 private:
  BDKContentsHostView* __strong view_;
  BDKMenuTarget* __strong objc_target_;
  bool window_made_fullscreen_ = false;
};

}  // namespace

// static
std::unique_ptr<HostView> HostView::Create() {
  return std::make_unique<HostViewMac>();
}

// static
bdk_native_handle_t HostView::KeyEvent(
    const input::NativeWebKeyboardEvent& event) {
  NSEvent* ns_event = event.os_event.Get();
  return bdk_native_handle_t{ns_event ? BDK_NATIVE_NSEVENT : 0u,
                             (__bridge void*)ns_event, 0};
}

// static
bool HostView::RedispatchUnhandledKey(
    const input::NativeWebKeyboardEvent& event) {
  NSEvent* ns_event = event.os_event.Get();
  if (!ns_event || ns_event.type != NSEventTypeKeyDown) {
    return false;
  }
  return [NSApp.mainMenu performKeyEquivalent:ns_event];
}

}  // namespace bdk
