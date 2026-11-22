/* Test hooks of the in-memory stub runtime. Not part of the ABI: a real runtime has its own way to
   run its event loop and to act as a page or a user, and the conformance harness (tests/harness.h)
   hides the difference. */
#ifndef BDK_STUB_H_
#define BDK_STUB_H_

#include "bdk/bdk.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The stub has no event loop of its own. The host (or test) calls this on the UI thread to run
   every queued callback and simulated step, in order, until the queue is empty. Returns the number
   of tasks run. */
BDK_EXPORT size_t bdk_stub_pump(void);

typedef struct bdk_stub_navigation_script {
    size_t struct_size;
    int32_t
        net_error; /* 0: the navigation commits and finishes; otherwise it fails with this error */
    bdk_string_t title; /* empty: the title is the URL */
} bdk_stub_navigation_script_t;

/* Scripts the outcome of every later navigation to url. */
BDK_EXPORT bdk_status_t bdk_stub_script_navigation(bdk_runtime_t, bdk_string_t url,
                                                   const bdk_stub_navigation_script_t*);

/* Raises a UI request as Chromium would. The handler registered for the surface is called from the
 * next pump. */
BDK_EXPORT bdk_status_t bdk_stub_raise_ui_request(bdk_runtime_t, uint32_t surface, bdk_contents_t,
                                                  bdk_string_t payload_json);
/* The raiser withdraws the request (the page navigated away, say). */
BDK_EXPORT bdk_status_t bdk_stub_withdraw_ui_requests(bdk_runtime_t);

#define BDK_STUB_UI_NONE 0u
#define BDK_STUB_UI_COMPLETED 1u
#define BDK_STUB_UI_DEFAULTED 2u
#define BDK_STUB_UI_REJECTED 3u
#define BDK_STUB_UI_CANCELLED 4u
#define BDK_STUB_UI_WITHDRAWN 5u
#define BDK_STUB_UI_DISABLED 6u /* the surface is disabled in the configuration */

typedef struct bdk_stub_ui_outcome {
    size_t struct_size;
    uint32_t kind; /* BDK_STUB_UI_*; the outcome of the most recently raised request */
    uint32_t surface;
    char completion[32];
    char arguments_json[128];
} bdk_stub_ui_outcome_t;

BDK_EXPORT bdk_status_t bdk_stub_last_ui_outcome(bdk_runtime_t, bdk_stub_ui_outcome_t* out);

/* Open requests (bdk_ui.h): sends the runtime's update to the most recently raised request, and
   reads the last update the application sent it (bdk_ui_update), "" for none. */
BDK_EXPORT bdk_status_t bdk_stub_send_ui_update(bdk_runtime_t, bdk_string_t update_json);
BDK_EXPORT bdk_status_t bdk_stub_last_ui_update(bdk_runtime_t, char* out, size_t cap);

/* Windows (bdk_window.h). Attaches the page to a registered window, as a view entering it would
   (window 0 detaches it): the first page becomes the active one and the window gets an extension
   window identifier, from 1. Focusing an attached page makes it the window's active page. */
BDK_EXPORT bdk_status_t bdk_stub_attach_contents(bdk_contents_t, bdk_window_t);
/* Acts as an extension asking for a change to the window: the window's request handler is called
   later, and bdk_stub_last_window_answer reports its answer (BDK_UI_*, 0 before it is known). */
BDK_EXPORT bdk_status_t bdk_stub_window_request(bdk_window_t, bdk_string_t request_json);
BDK_EXPORT bdk_status_t bdk_stub_last_window_answer(uint32_t* out);
/* Acts as a default presenter asking the window's anchor provider for a role. */
BDK_EXPORT bdk_status_t bdk_stub_window_anchor(bdk_window_t, uint32_t role, double out_rect[4],
                                               uint8_t* out_found);

/* Extensions. Registers the manifest of a package the stub cannot read from disk (kind "crx" and
   the file path, "updateUrl" and the extension id, "store" and "<store>/<id>"; "unpacked" and a
   directory overrides its manifest.json). Registering a newer version for an installed id is what
   extensions.checkForUpdates (or reload, for unpacked) finds. */
BDK_EXPORT bdk_status_t bdk_stub_extension_package(bdk_runtime_t, bdk_string_t kind,
                                                   bdk_string_t key, bdk_string_t manifest_json);
/* How many times bdk_extension_menu_present reached the stub since start. */
BDK_EXPORT bdk_status_t bdk_stub_menus_presented(uint32_t* out);

/* Delivers a key event to the keyboard handler of the contents. *out_reserved is the reserve
 * callback's answer. */
BDK_EXPORT bdk_status_t bdk_stub_simulate_key(bdk_contents_t, bdk_native_handle_t key_event,
                                              uint8_t* out_reserved);

#ifdef __cplusplus
}
#endif

#endif /* BDK_STUB_H_ */
