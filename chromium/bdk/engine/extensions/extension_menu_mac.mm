// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/extensions/extension_ui_platform.h"

#import <AppKit/AppKit.h>

#include <utility>

#include "base/strings/sys_string_conversions.h"

// Calls back with the chosen line of the default extensions menu.
@interface BDKExtensionsMenuTarget : NSObject
@property(nonatomic, copy) void (^chosen)(NSString*);
- (void)choose:(NSMenuItem*)item;
@end

@implementation BDKExtensionsMenuTarget

@synthesize chosen = _chosen;

- (void)choose:(NSMenuItem*)item {
  if (self.chosen) {
    self.chosen(item.representedObject);
  }
}

@end

// The title-bar accessory: one button, at the trailing edge of the title bar.
@interface BDKExtensionsAccessory : NSTitlebarAccessoryViewController
@property(nonatomic, strong) NSButton* button;
@property(nonatomic, copy) void (^clicked)(NSView*);
- (void)click:(id)sender;
@end

@implementation BDKExtensionsAccessory

@synthesize button = _button;
@synthesize clicked = _clicked;

- (instancetype)init {
  self = [super init];
  if (!self) {
    return nil;
  }
  NSImage* image = [NSImage imageWithSystemSymbolName:@"puzzlepiece.extension"
                             accessibilityDescription:@"Extensions"];
  _button = [NSButton buttonWithImage:image
                               target:self
                               action:@selector(click:)];
  _button.bordered = NO;
  _button.toolTip = @"Extensions";
  _button.frame = NSMakeRect(0, 0, 36, 28);
  NSView* container = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 36, 28)];
  [container addSubview:_button];
  self.view = container;
  self.layoutAttribute = NSLayoutAttributeTrailing;
  return self;
}

- (void)click:(id)sender {
  if (self.clicked) {
    self.clicked(self.button);
  }
}

@end

namespace bdk {

namespace {

NSImage* ImageFromDataUrl(const std::string& url) {
  constexpr char kPrefix[] = "data:image/png;base64,";
  if (!url.starts_with(kPrefix)) {
    return nil;
  }
  NSData* data = [[NSData alloc]
      initWithBase64EncodedString:base::SysUTF8ToNSString(
                                      url.substr(sizeof(kPrefix) - 1))
                          options:0];
  NSImage* image = data ? [[NSImage alloc] initWithData:data] : nil;
  image.size = NSMakeSize(16, 16);
  return image;
}

class TitlebarMenuButtonMac : public TitlebarMenuButton {
 public:
  TitlebarMenuButtonMac(NSWindow* window, BDKExtensionsAccessory* accessory)
      : window_(window), accessory_(accessory) {}

  ~TitlebarMenuButtonMac() override {
    NSWindow* window = window_;
    if (window) {
      const NSUInteger index =
          [window.titlebarAccessoryViewControllers indexOfObject:accessory_];
      if (index != NSNotFound) {
        [window removeTitlebarAccessoryViewControllerAtIndex:index];
      }
    }
    accessory_.clicked = nil;
  }

  void SetVisible(bool visible) override { accessory_.hidden = !visible; }

  bool IsVisible() const override { return !accessory_.hidden; }

  WindowRect FrameInWindow() const override {
    NSWindow* window = window_;
    NSView* button = accessory_.button;
    if (!window || accessory_.hidden || !button.window) {
      return {0, 0, 0, 0};
    }
    // Relative to the window's frame, origin top-left: the title bar is above
    // the content area.
    const NSRect rect = [button convertRect:button.bounds toView:nil];
    const CGFloat top =
        window.frame.size.height - (rect.origin.y + rect.size.height);
    return {rect.origin.x, top, rect.size.width, rect.size.height};
  }

 private:
  NSWindow* __weak window_;
  BDKExtensionsAccessory* __strong accessory_;
};

}  // namespace

void ShowExtensionsMenu(
    bdk_native_handle_t window,
    std::optional<WindowRect> anchor,
    bdk_native_handle_t anchor_view,
    const std::vector<ExtensionMenuEntry>& entries,
    base::OnceCallback<void(const std::string& id)> chosen) {
  NSMenu* menu = [[NSMenu alloc] initWithTitle:@""];
  menu.autoenablesItems = NO;
  BDKExtensionsMenuTarget* target = [[BDKExtensionsMenuTarget alloc] init];
  __block base::OnceCallback<void(const std::string&)> callback =
      std::move(chosen);
  target.chosen = ^(NSString* item_id) {
    if (callback) {
      std::move(callback).Run(base::SysNSStringToUTF8(item_id));
    }
  };
  for (const ExtensionMenuEntry& entry : entries) {
    if (entry.id.empty()) {
      [menu addItem:[NSMenuItem separatorItem]];
      continue;
    }
    std::string title = entry.title;
    if (!entry.badge.empty()) {
      title += "  [" + entry.badge + "]";
    }
    NSMenuItem* item =
        [[NSMenuItem alloc] initWithTitle:base::SysUTF8ToNSString(title)
                                   action:@selector(choose:)
                            keyEquivalent:@""];
    item.target = target;
    item.enabled = entry.enabled;
    item.representedObject = base::SysUTF8ToNSString(entry.id);
    if (NSImage* icon = ImageFromDataUrl(entry.icon)) {
      item.image = icon;
    }
    [menu addItem:item];
  }

  NSView* below = anchor_view.kind == BDK_NATIVE_NSVIEW && anchor_view.pointer
                      ? (__bridge NSView*)anchor_view.pointer
                      : nil;
  NSWindow* parent = window.kind == BDK_NATIVE_NSWINDOW && window.pointer
                         ? (__bridge NSWindow*)window.pointer
                         : nil;
  if (below) {
    // Under the title-bar button, leading edges together.
    const CGFloat y = below.isFlipped ? below.bounds.size.height : 0;
    [menu popUpMenuPositioningItem:nil
                        atLocation:NSMakePoint(0, y)
                            inView:below];
  } else if (parent) {
    // Under the anchor with the trailing edges together, or in the top
    // trailing corner of the window's content.
    NSView* content = parent.contentView;
    const CGFloat width = menu.size.width;
    CGFloat x = content.bounds.size.width - width - 8;
    CGFloat y = 0;  // From the top.
    if (anchor) {
      x = (*anchor)[0] + (*anchor)[2] - width;
      y = (*anchor)[1] + (*anchor)[3];
    }
    const CGFloat view_y =
        content.isFlipped ? y : content.bounds.size.height - y;
    [menu popUpMenuPositioningItem:nil
                        atLocation:NSMakePoint(x, view_y)
                            inView:content];
  } else {
    [menu popUpMenuPositioningItem:nil
                        atLocation:NSEvent.mouseLocation
                            inView:nil];
  }
}

// static
std::unique_ptr<TitlebarMenuButton> TitlebarMenuButton::Create(
    bdk_native_handle_t window,
    base::RepeatingCallback<void(bdk_native_handle_t button)> clicked) {
  NSWindow* ns_window = window.kind == BDK_NATIVE_NSWINDOW && window.pointer
                            ? (__bridge NSWindow*)window.pointer
                            : nil;
  if (!ns_window || !(ns_window.styleMask & NSWindowStyleMaskTitled)) {
    return nullptr;
  }
  BDKExtensionsAccessory* accessory = [[BDKExtensionsAccessory alloc] init];
  accessory.clicked = ^(NSView* button) {
    clicked.Run(
        bdk_native_handle_t{BDK_NATIVE_NSVIEW, (__bridge void*)button, 0});
  };
  [ns_window addTitlebarAccessoryViewController:accessory];
  return std::make_unique<TitlebarMenuButtonMac>(ns_window, accessory);
}

}  // namespace bdk
