/* BDK C ABI: window registration. See c-abi.md section 5.5 and swift-api.md section 8.1.
   PROVISIONAL signatures: c-abi.md names the families, not their shapes. */
#ifndef BDK_WINDOW_H_
#define BDK_WINDOW_H_

#include "bdk_base.h"
#include "bdk_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers a native window (BDK_NATIVE_NSWINDOW, HWND, ...) with its kind (BDK_WINDOW_KIND_*,
   what extensions see in windows.get). Released by bdk_window_unregister, or at shutdown. A window
   can be registered once; a page view entering a window that is not registered registers it as
   BDK_WINDOW_KIND_NORMAL in the bindings. */
BDK_EXPORT bdk_status_t bdk_window_register(bdk_runtime_t, bdk_native_handle_t window,
                                            uint32_t window_kind, bdk_window_t* out, bdk_error_t*);
BDK_EXPORT bdk_status_t bdk_window_unregister(bdk_window_t, bdk_error_t*);

/* Asked, on the UI thread, where a default presenter for a role (BDK_ANCHOR_*) attaches: writes x,
   y, width, height in window coordinates to out_rect and returns 1, or returns 0 for "no anchor"
   (the presenter then uses the top centre of the page view, or for BDK_ANCHOR_EXTENSIONS the top
   trailing corner of the window's content). Never called from inside a BDK call. */
typedef uint8_t (*bdk_anchor_cb)(void* context, bdk_window_t window, uint32_t role,
                                 double out_rect[4]);
/* Replaces the window's anchor provider (the previous context is released); a null callback
   removes it. BDK owns the context until it is replaced or the window is unregistered. */
BDK_EXPORT bdk_status_t bdk_window_set_anchor_provider(bdk_window_t, bdk_anchor_cb, void* ctx,
                                                       bdk_release_cb, bdk_error_t*);

/* A request that targets the window rather than a page, from an extension (windows.update) or a
   page: request_json is {"kind": "setFrame" | "activate" | "minimize" | "zoom" |
   "enterFullscreen" | "exitFullscreen" | "close", "frame"?: {"x","y","width","height"} (setFrame,
   screen coordinates), "initiator"?: {"extensionId"}}. BDK_UI_USE_DEFAULT (and no handler) applies
   it to the native window; BDK_UI_REJECT and BDK_UI_HANDLED leave it to the application, and the
   extension's call resolves with the window as it then is. Called later, never from inside a BDK
   call. */
typedef bdk_ui_response_t (*bdk_window_request_cb)(void* context, bdk_window_t window,
                                                   bdk_string_t request_json);
BDK_EXPORT bdk_status_t bdk_window_set_request_handler(bdk_window_t, bdk_window_request_cb,
                                                       void* ctx, bdk_release_cb, bdk_error_t*);

/* The registered window the page is attached to, or was last attached to; 0 if none. */
BDK_EXPORT bdk_status_t bdk_window_for_contents(bdk_contents_t, bdk_window_t* out, bdk_error_t*);
/* The pages attached or last attached to the window, as a JSON array of decimal handle strings,
   in the order they arrived. Release the buffer with bdk_release. */
BDK_EXPORT bdk_status_t bdk_window_contents(bdk_window_t, bdk_buffer_t* out_json, bdk_error_t*);
/* The window's active page (the one extensions see as its active tab), or 0. */
BDK_EXPORT bdk_status_t bdk_window_active_contents(bdk_window_t, bdk_contents_t* out, bdk_error_t*);
/* The identifier extensions use for the window (windows.get), or -1 before the window shows a page
   of a profile with extensions. A window shows one extension window per profile; this is the one
   of the profile of its active page. */
BDK_EXPORT bdk_status_t bdk_window_extension_id(bdk_window_t, int32_t* out, bdk_error_t*);

#ifdef __cplusplus
}
#endif

#endif /* BDK_WINDOW_H_ */
