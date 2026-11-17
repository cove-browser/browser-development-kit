/* What the conformance suite needs from the runtime under test beyond the ABI itself: a way to run
   the UI thread's pending work, a second thread, and test-only ways to act as a page or as
   Chromium. Each runtime under test provides one implementation (harness_stub.cc for the stub). The
   suite itself includes only bdk/bdk.h and this header, so the same tests link against a real
   runtime. Plain C. */
#ifndef BDK_TEST_HARNESS_H_
#define BDK_TEST_HARNESS_H_

#include "bdk/bdk.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Runs every pending callback and simulated step on the calling (UI) thread until idle. Returns the
 * count. */
size_t bdk_harness_pump(void);

/* Runs fn(ctx) on a different thread and returns after it finishes. */
void bdk_harness_run_on_other_thread(void (*fn)(void*), void* ctx);

/* Scripts the outcome of every later navigation to url. net_error 0 succeeds; title may be NULL. */
bdk_status_t bdk_harness_script_navigation(bdk_runtime_t, const char* url, int32_t net_error,
                                           const char* title);

/* Acts as Chromium raising a UI request. The registered handler is called during a later pump. */
bdk_status_t bdk_harness_raise_ui_request(bdk_runtime_t, uint32_t surface, bdk_contents_t,
                                          const char* payload_json);
/* Acts as Chromium withdrawing every request that is not completed. */
bdk_status_t bdk_harness_withdraw_ui_requests(bdk_runtime_t);

#define BDK_HARNESS_UI_NONE 0u
#define BDK_HARNESS_UI_COMPLETED 1u
#define BDK_HARNESS_UI_DEFAULTED 2u
#define BDK_HARNESS_UI_REJECTED 3u
#define BDK_HARNESS_UI_CANCELLED 4u
#define BDK_HARNESS_UI_WITHDRAWN 5u

typedef struct bdk_harness_ui_outcome {
    uint32_t kind; /* what happened to the most recently raised request */
    char completion[32];
    char arguments_json[128];
} bdk_harness_ui_outcome_t;

bdk_status_t bdk_harness_last_ui_outcome(bdk_runtime_t, bdk_harness_ui_outcome_t* out);

/* Acts as the native view delivering a key event to the contents' keyboard handler. */
bdk_status_t bdk_harness_simulate_key(bdk_contents_t, uint8_t* out_reserved);

#ifdef __cplusplus
}
#endif

#endif /* BDK_TEST_HARNESS_H_ */
