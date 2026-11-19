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

/* Delivers a key event to the keyboard handler of the contents. *out_reserved is the reserve
 * callback's answer. */
BDK_EXPORT bdk_status_t bdk_stub_simulate_key(bdk_contents_t, bdk_native_handle_t key_event,
                                              uint8_t* out_reserved);

#ifdef __cplusplus
}
#endif

#endif /* BDK_STUB_H_ */
