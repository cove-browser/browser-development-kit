// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/extensions/side_panel_view.h"

#import <AppKit/AppKit.h>

#include <algorithm>
#include <utility>

#include "base/memory/raw_ptr.h"
#include "base/strings/sys_string_conversions.h"
#include "content/public/browser/web_contents.h"
#include "ui/gfx/native_ui_types.h"

// The view an extension's side panel lives in, which the application receives
// as the request's secure view. It holds the panel's page as its only subview
// and fills itself with it, as the page's host view does for a page
// (host_view_mac.mm).
@interface BDKSidePanelContainerView : NSView
@end

@implementation BDKSidePanelContainerView

- (BOOL)isFlipped {
  return YES;
}

- (void)resizeSubviewsWithOldSize:(NSSize)oldSize {
  for (NSView* view in self.subviews) {
    view.frame = self.bounds;
  }
}

@end

// The divider on the panel's leading edge. Dragging it resizes the panel.
@interface BDKSidePanelDividerView : NSView
// Called with the width the drag asks for while it goes on, and when it ends.
@property(nonatomic, copy) void (^dragged)(CGFloat delta, BOOL done);
@end

@implementation BDKSidePanelDividerView {
  CGFloat _startX;
}

@synthesize dragged = _dragged;

- (void)resetCursorRects {
  [self addCursorRect:self.bounds cursor:NSCursor.resizeLeftRightCursor];
}

- (void)mouseDown:(NSEvent*)event {
  _startX = [self convertPoint:event.locationInWindow fromView:nil].x;
}

- (void)mouseDragged:(NSEvent*)event {
  if (self.dragged) {
    self.dragged(
        [self convertPoint:event.locationInWindow fromView:nil].x - _startX,
        NO);
  }
}

- (void)mouseUp:(NSEvent*)event {
  if (self.dragged) {
    self.dragged(
        [self convertPoint:event.locationInWindow fromView:nil].x - _startX,
        YES);
  }
}

- (void)drawRect:(NSRect)rect {
  [NSColor.separatorColor setFill];
  NSRectFill(NSMakeRect(0, 0, 1, NSHeight(self.bounds)));
}

@end

// What the default presenter puts around the panel: a header and a divider.
@interface BDKSidePanelFrameView : NSView
@property(nonatomic, copy) void (^closeClicked)(void);
@property(nonatomic, copy) void (^widthDragged)(CGFloat delta, BOOL done);
- (instancetype)initWithTitle:(NSString*)title
                         icon:(NSImage*)icon
                        panel:(NSView*)panel
                        width:(CGFloat)width;
@end

namespace {
constexpr CGFloat kHeaderHeight = 36;
constexpr CGFloat kDividerWidth = 6;
}  // namespace

@implementation BDKSidePanelFrameView {
  NSView* _panel;
  BDKSidePanelDividerView* _divider;
  NSView* _header;
}

@synthesize closeClicked = _closeClicked;
@synthesize widthDragged = _widthDragged;

- (instancetype)initWithTitle:(NSString*)title
                         icon:(NSImage*)icon
                        panel:(NSView*)panel
                        width:(CGFloat)width {
  self = [super initWithFrame:NSMakeRect(0, 0, width, 600)];
  if (!self) {
    return nil;
  }
  _panel = panel;
  self.wantsLayer = YES;
  self.layer.backgroundColor = NSColor.windowBackgroundColor.CGColor;
  self.autoresizingMask = NSViewMinXMargin | NSViewHeightSizable;
  self.autoresizesSubviews = YES;

  const NSRect bounds = self.bounds;
  _header = [[NSView alloc]
      initWithFrame:NSMakeRect(kDividerWidth, NSHeight(bounds) - kHeaderHeight,
                               NSWidth(bounds) - kDividerWidth, kHeaderHeight)];
  _header.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
  [self addSubview:_header];

  CGFloat x = 12;
  if (icon) {
    NSImageView* icon_view =
        [[NSImageView alloc] initWithFrame:NSMakeRect(x, 10, 16, 16)];
    icon_view.image = icon;
    [_header addSubview:icon_view];
    x += 24;
  }
  NSTextField* title_label = [NSTextField labelWithString:title ?: @""];
  title_label.font = [NSFont boldSystemFontOfSize:13];
  title_label.lineBreakMode = NSLineBreakByTruncatingTail;
  title_label.frame = NSMakeRect(x, 9, 240, 18);
  title_label.autoresizingMask = NSViewWidthSizable;
  [_header addSubview:title_label];

  NSButton* close = [NSButton buttonWithTitle:@"×"
                                       target:self
                                       action:@selector(closeTapped:)];
  close.bordered = NO;
  close.toolTip = @"Close";
  close.frame = NSMakeRect(NSWidth(_header.bounds) - 32, 7, 24, 22);
  close.autoresizingMask = NSViewMinXMargin;
  [_header addSubview:close];

  _panel.frame = NSMakeRect(kDividerWidth, 0, NSWidth(bounds) - kDividerWidth,
                            NSHeight(bounds) - kHeaderHeight);
  _panel.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
  [self addSubview:_panel];

  _divider = [[BDKSidePanelDividerView alloc]
      initWithFrame:NSMakeRect(0, 0, kDividerWidth, NSHeight(bounds))];
  _divider.autoresizingMask = NSViewHeightSizable;
  __weak BDKSidePanelFrameView* weak_self = self;
  _divider.dragged = ^(CGFloat delta, BOOL done) {
    if (weak_self.widthDragged) {
      weak_self.widthDragged(delta, done);
    }
  };
  [self addSubview:_divider];
  return self;
}

- (void)closeTapped:(id)sender {
  if (self.closeClicked) {
    self.closeClicked();
  }
}

@end

namespace bdk {

namespace {

class SidePanelViewMac : public SidePanelView {
 public:
  SidePanelViewMac()
      : view_([[BDKSidePanelContainerView alloc]
            initWithFrame:NSMakeRect(0, 0, kSidePanelDefaultWidth, 600)]) {
    view_.autoresizesSubviews = YES;
  }

  ~SidePanelViewMac() override {
    for (NSView* view in [view_.subviews copy]) {
      [view removeFromSuperview];
    }
    [view_ removeFromSuperview];
  }

  void SetContents(content::WebContents* contents) override {
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

 private:
  NSView* __strong view_;
};

NSImage* ImageFromDataURL(const std::string& url) {
  const size_t comma = url.find(',');
  if (url.rfind("data:", 0) != 0 || comma == std::string::npos) {
    return nil;
  }
  NSData* data = [[NSData alloc]
      initWithBase64EncodedString:base::SysUTF8ToNSString(url.substr(comma + 1))
                          options:0];
  return data ? [[NSImage alloc] initWithData:data] : nil;
}

class SidePanelFrameMac : public SidePanelFrame {
 public:
  SidePanelFrameMac(NSWindow* window, const Params& params, Client* client)
      : window_(window), client_(client), width_(params.width) {
    NSView* panel = (__bridge NSView*)params.view.pointer;
    frame_view_ = [[BDKSidePanelFrameView alloc]
        initWithTitle:base::SysUTF8ToNSString(params.title)
                 icon:ImageFromDataURL(params.icon_data_url)
                panel:panel
                width:width_];
    __weak BDKSidePanelFrameView* weak_view = frame_view_;
    frame_view_.closeClicked = ^{
      if (weak_view && client_) {
        client_->CloseClicked();
      }
    };
    frame_view_.widthDragged = ^(CGFloat delta, BOOL done) {
      // The width the drag started at is what it is measured from.
      if (!dragging_) {
        dragging_ = true;
        drag_start_width_ = width_;
      }
      ApplyWidth(drag_start_width_ - static_cast<int>(delta));
      if (done) {
        dragging_ = false;
        if (client_) {
          client_->WidthChanged(width_);
        }
      }
    };
    SetWidth(width_);
    [window_.contentView addSubview:frame_view_
                         positioned:NSWindowAbove
                         relativeTo:nil];
  }

  ~SidePanelFrameMac() override {
    frame_view_.closeClicked = nil;
    frame_view_.widthDragged = nil;
    // The panel's view goes back to being parentless: the host destroys it.
    for (NSView* view in [frame_view_.subviews copy]) {
      if ([view isKindOfClass:BDKSidePanelContainerView.class]) {
        [view removeFromSuperview];
      }
    }
    [frame_view_ removeFromSuperview];
  }

  void SetVisible(bool visible) override { frame_view_.hidden = !visible; }

  void SetWidth(int width) override { ApplyWidth(width); }

  void DragDivider(int delta) override {
    ApplyWidth(width_ - delta);
    if (client_) {
      client_->WidthChanged(width_);
    }
  }

  std::array<double, 4> FrameInWindow() override {
    if (!frame_view_.window) {
      return {0, 0, 0, 0};
    }
    NSView* content = frame_view_.window.contentView;
    const NSRect rect = [content convertRect:frame_view_.frame
                                    fromView:frame_view_.superview];
    return {rect.origin.x,
            NSHeight(content.bounds) - rect.origin.y - rect.size.height,
            rect.size.width, rect.size.height};
  }

  bool IsVisible() override { return !frame_view_.hiddenOrHasHiddenAncestor; }

 private:
  // Sizes the frame to `width` points within the limits, on the trailing edge.
  void ApplyWidth(int width) {
    NSView* content = window_.contentView;
    const CGFloat limit = std::max<CGFloat>(kSidePanelMinimumWidth,
                                            NSWidth(content.bounds) * 0.7);
    width_ = static_cast<int>(
        std::clamp<CGFloat>(width, kSidePanelMinimumWidth, limit));
    frame_view_.frame = NSMakeRect(NSWidth(content.bounds) - width_, 0, width_,
                                   NSHeight(content.bounds));
  }

  // The application owns the window; it unregisters it before it goes.
  NSWindow* __weak window_;
  BDKSidePanelFrameView* __strong frame_view_;
  raw_ptr<Client> client_;
  int width_;
  bool dragging_ = false;
  int drag_start_width_ = 0;
};

}  // namespace

// static
std::unique_ptr<SidePanelView> SidePanelView::Create() {
  return std::make_unique<SidePanelViewMac>();
}

// static
std::unique_ptr<SidePanelFrame> SidePanelFrame::Create(const Params& params,
                                                       Client* client) {
  if (params.window.kind != BDK_NATIVE_NSWINDOW || !params.window.pointer ||
      params.view.kind != BDK_NATIVE_NSVIEW || !params.view.pointer) {
    return nullptr;
  }
  NSWindow* window = (__bridge NSWindow*)params.window.pointer;
  if (!window.contentView) {
    return nullptr;
  }
  return std::make_unique<SidePanelFrameMac>(window, params, client);
}

}  // namespace bdk
