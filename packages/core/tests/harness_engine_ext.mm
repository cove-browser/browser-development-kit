/* Application windows for the extension panel scenarios (tests_engine_ext_panels.c): windows
   registered with a chosen kind, pages attached to them, their frames, and a container an
   application places a side panel's secure view in. Part of the engine conformance app. */
#import <AppKit/AppKit.h>

#include <cstring>

#include "bdk/bdk.h"

namespace {

NSMutableArray<NSWindow*>* Windows() {
    static NSMutableArray<NSWindow*>* windows;
    if (!windows)
        windows = [NSMutableArray array];
    return windows;
}

} // namespace

extern "C" {

/* Registers a new window of `kind` (BDK_WINDOW_KIND_*); returns its index, or -1. */
int bdk_ext_window_open(bdk_runtime_t rt, uint32_t kind, bdk_window_t* handle) {
    NSWindow* window =
        [[NSWindow alloc] initWithContentRect:NSMakeRect(100, 100, 900, 600)
                                    styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskResizable
                                      backing:NSBackingStoreBuffered
                                        defer:NO];
    window.releasedWhenClosed = NO;
    [window orderFront:nil];
    bdk_native_handle_t native;
    memset(&native, 0, sizeof native);
    native.kind = BDK_NATIVE_NSWINDOW;
    native.pointer = (__bridge void*)window;
    bdk_error_t error = 0;
    if (bdk_window_register(rt, native, kind, handle, &error) != BDK_OK)
        return -1;
    [Windows() addObject:window];
    return (int)Windows().count - 1;
}

void bdk_ext_window_attach(int window, bdk_contents_t contents) {
    bdk_native_handle_t view;
    bdk_error_t error = 0;
    if (bdk_contents_native_view(contents, &view, &error) != BDK_OK || !view.pointer)
        return;
    NSWindow* w = Windows()[window];
    NSView* page = (__bridge NSView*)view.pointer;
    page.frame = w.contentView.bounds;
    page.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [w.contentView addSubview:page];
}

/* Unregisters (the caller does) and closes the window. */
void bdk_ext_window_close(int window) {
    NSWindow* w = Windows()[window];
    [w orderOut:nil];
    [w close];
}

/* The window's frame in screen coordinates with the origin at the top left: x, y, width, height.
 */
void bdk_ext_window_frame(int window, double out[4]) {
    NSWindow* w = Windows()[window];
    NSRect screen = NSScreen.screens.firstObject.frame;
    out[0] = w.frame.origin.x;
    out[1] = NSHeight(screen) - w.frame.origin.y - NSHeight(w.frame);
    out[2] = NSWidth(w.frame);
    out[3] = NSHeight(w.frame);
}

void bdk_ext_window_content_size(int window, double out[2]) {
    NSView* content = Windows()[window].contentView;
    out[0] = NSWidth(content.bounds);
    out[1] = NSHeight(content.bounds);
}

int bdk_ext_window_is_minimized(int window) {
    return Windows()[window].isMiniaturized ? 1 : 0;
}

/* An application's container: puts the panel's secure view in the window at x, y (top left),
   width and height. Returns 1 when it is in the window. */
int bdk_ext_place_view(int window, bdk_native_handle_t view, double x, double y, double width,
                       double height) {
    if (view.kind != BDK_NATIVE_NSVIEW || !view.pointer)
        return 0;
    NSView* content = Windows()[window].contentView;
    NSView* panel = (__bridge NSView*)view.pointer;
    panel.frame = NSMakeRect(x, NSHeight(content.bounds) - y - height, width, height);
    [content addSubview:panel];
    return panel.window != nil ? 1 : 0;
}

int bdk_ext_view_in_window(bdk_native_handle_t view) {
    if (view.kind != BDK_NATIVE_NSVIEW || !view.pointer)
        return 0;
    return ((__bridge NSView*)view.pointer).window != nil ? 1 : 0;
}

void bdk_ext_remove_view(bdk_native_handle_t view) {
    if (view.kind == BDK_NATIVE_NSVIEW && view.pointer)
        [(__bridge NSView*)view.pointer removeFromSuperview];
}

} /* extern "C" */
