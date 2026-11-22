// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_EXTENSIONS_SIDE_PANEL_VIEW_H_
#define BDK_ENGINE_EXTENSIONS_SIDE_PANEL_VIEW_H_

#include <array>
#include <memory>
#include <string>

#include "bdk/core/include/bdk/bdk_base.h"

namespace content {
class WebContents;
}  // namespace content

namespace bdk {

// The platform half of an extension side panel (extensions.md 4.3,
// T-EXT-03): the native view that holds the panel's page, which the
// application receives as the request's secure view, and what the default
// presenter draws around it. Each platform implements the factories in its
// own file (side_panel_view_mac.mm); nothing else names a toolkit type.

// The panel's view: it holds the panel's page as its only subview and fills
// itself with it, so the application can place it anywhere at any size.
class SidePanelView {
 public:
  static std::unique_ptr<SidePanelView> Create();

  virtual ~SidePanelView() = default;

  // Shows `contents`' view inside the panel's view.
  virtual void SetContents(content::WebContents* contents) = 0;
  // The view as a tagged native handle (the request's secure view).
  virtual bdk_native_handle_t NativeHandle() = 0;
  // Whether the view is in a window.
  virtual bool IsInWindow() = 0;
};

// The default presenter's frame around a panel (no handler for the surface, or
// one that answered "use default"): attached to the trailing edge of a window
// over its content, with a header that shows the extension's title and icon
// and a close button, and a divider the person drags to resize it. Destroying
// it takes it away.
class SidePanelFrame {
 public:
  class Client {
   public:
    virtual ~Client() = default;
    // The close button was pressed.
    virtual void CloseClicked() = 0;
    // The person finished resizing the panel to `width` points.
    virtual void WidthChanged(int width) = 0;
  };

  struct Params {
    // The window to attach to (a tagged native window) and the panel's view
    // (SidePanelView::NativeHandle()).
    bdk_native_handle_t window;
    bdk_native_handle_t view;
    std::string title;
    // A data: URL for the header's icon; empty for none.
    std::string icon_data_url;
    int width = 360;
  };

  // Null when `window` is not a window of this platform.
  static std::unique_ptr<SidePanelFrame> Create(const Params& params,
                                                Client* client);

  virtual ~SidePanelFrame() = default;

  // Shows or hides the frame (a page-scoped panel whose page is not the
  // window's focused page is hidden).
  virtual void SetVisible(bool visible) = 0;
  // Resizes to `width` points within the limits (the minimum Chromium's side
  // panel has, and most of the window), without telling the client.
  virtual void SetWidth(int width) = 0;
  // What dragging the divider by `delta` points (negative: wider) does,
  // including telling the client when it ends.
  virtual void DragDivider(int delta) = 0;
  // The frame in the window's content view, in points, origin top left: x, y,
  // width, height. All zero when it is not in a window.
  virtual std::array<double, 4> FrameInWindow() = 0;
  virtual bool IsVisible() = 0;
};

// The width the panel opens at, and the least it can be dragged to. Chromium's
// side panel is 360 points wide by default, and extensions lay out for that.
inline constexpr int kSidePanelDefaultWidth = 360;
inline constexpr int kSidePanelMinimumWidth = 360;

}  // namespace bdk

#endif  // BDK_ENGINE_EXTENSIONS_SIDE_PANEL_VIEW_H_
