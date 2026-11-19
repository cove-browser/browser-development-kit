/* Test hooks of the Chromium engine. Not part of the ABI: the engine's
   counterpart of packages/core/stub/bdk_stub.h, used by the conformance
   harness (packages/core/tests/harness_engine.mm) and the engine scenario
   tests. The framework exports them; an application has no reason to call
   them.

   None of them gives a page or an automation session anything: each acts as
   the application itself (raising a UI request to the application's own
   handler, calling its own keyboard handler) or on the application's own
   pages, which the application already controls fully through the ABI. */
#ifndef BDK_ENGINE_BDK_TESTING_H_
#define BDK_ENGINE_BDK_TESTING_H_

#include "bdk/bdk.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Serves later navigations to `url` from the engine instead of the network:
   net_error 0 answers with an HTML page titled `title` (the URL if empty);
   otherwise the navigation fails with that network error. */
BDK_EXPORT bdk_status_t bdk_testing_script_navigation(bdk_runtime_t,
                                                      bdk_string_t url,
                                                      int32_t net_error,
                                                      bdk_string_t title);

/* With `on` set, every http and https navigation is served as if scripted
   with net_error 0 and no title, so a test never reaches the network. */
BDK_EXPORT bdk_status_t bdk_testing_intercept_all(uint8_t on);

/* Raises a UI request as Chromium would, for the application's own handler.
 */
BDK_EXPORT bdk_status_t bdk_testing_raise_ui_request(bdk_runtime_t,
                                                     uint32_t surface,
                                                     bdk_contents_t,
                                                     bdk_string_t payload_json);
BDK_EXPORT bdk_status_t bdk_testing_withdraw_ui_requests(bdk_runtime_t);

typedef struct bdk_testing_ui_outcome {
  size_t struct_size;
  uint32_t kind; /* 0 none, 1 completed, 2 defaulted, 3 rejected,
                    4 cancelled, 5 withdrawn */
  uint32_t surface;
  char completion[32];
  char arguments_json[128];
} bdk_testing_ui_outcome_t;

/* The outcome of the request most recently raised by
   bdk_testing_raise_ui_request. */
BDK_EXPORT bdk_status_t
bdk_testing_last_ui_outcome(bdk_runtime_t, bdk_testing_ui_outcome_t* out);

/* Calls the page's keyboard handler as its view would for `key_event`. */
BDK_EXPORT bdk_status_t bdk_testing_simulate_key(bdk_contents_t,
                                                 bdk_native_handle_t key_event,
                                                 uint8_t* out_reserved);

/* Runs `script` in the page's main frame, with a user activation if
   `user_gesture` is set (for window.open and requestFullscreen). */
BDK_EXPORT bdk_status_t bdk_testing_execute_script(bdk_contents_t,
                                                   bdk_string_t script,
                                                   uint8_t user_gesture);

/* Sends a mouse click at x, y in the page's view (top-left origin). button: 0
   left, 2 right. A right click opens the context menu. */
BDK_EXPORT bdk_status_t bdk_testing_click(bdk_contents_t,
                                          double x,
                                          double y,
                                          uint32_t button);

/* Runs the application's run loop for `seconds`, letting Chromium run its
   tasks even when called from inside one (a harness running as a task). */
BDK_EXPORT void bdk_testing_run_for(double seconds);

/* The next runtime starts with no persistent profiles, as with a new data
   directory (Chromium's data directory is fixed for the process). Their data
   stays on disk under identifiers that are not reused. */
BDK_EXPORT void bdk_testing_forget_profiles(void);

/* How many Chrome browser windows (Browser objects) exist that are not host
   contexts. BDK's host contexts are Browsers with no native window of their
   own, so they are not counted: anything above 0 means Chromium opened a window
   on its own. */
BDK_EXPORT size_t bdk_testing_chrome_window_count(void);

/* How many host contexts (the Browsers BDK keeps its pages' tabs in) exist. */
BDK_EXPORT size_t bdk_testing_host_context_count(void);

typedef struct bdk_testing_tab_info {
  size_t struct_size;
  uint8_t is_tab;            /* the page is a tab of a host context */
  uint8_t parking;           /* ... of a parking context (no window) */
  uint8_t active;            /* ... and the active tab of it */
  uint8_t has_native_window; /* the context's window has a native window */
  int32_t tab_id;     /* the tab's session identifier: the extension tab id */
  int32_t context_id; /* the context's window identifier (Browser session id) */
  uint32_t tab_count; /* tabs in the context */
  uint32_t reserved;
} bdk_testing_tab_info_t;

/* Where the page's tab is. */
BDK_EXPORT bdk_status_t bdk_testing_tab_info(bdk_contents_t,
                                             bdk_testing_tab_info_t* out);

/* The services' test hooks, behind one function so that the framework's export
   list needs one name for them. `op` is one of the BDK_TESTING_SERVICES_*
   values; `page` is a page of the profile the hook looks at.

   PREF_JSON: writes into `out` the JSON value of the Chromium preference named
   `a` in the profile; fails when there is no such preference.
   CONTENT_SETTING: writes into `out` what Chromium's content settings hold for
   the profile, URL `a` and permission kind `b`: "allow", "block" or "ask".
   CAPTURE_PNG, CAPTURE_PDF: capture the page to the file named `a`, a PNG
   screenshot of its visible area or a PDF of the document. They run in the
   background; poll CAPTURE_STATE.
   CAPTURE_STATE: writes "0" (running, or none started), "1" (the file is
   written) or "2" (it failed) into `out`. */
#define BDK_TESTING_SERVICES_PREF_JSON 1u
#define BDK_TESTING_SERVICES_CONTENT_SETTING 2u
#define BDK_TESTING_SERVICES_CAPTURE_PNG 3u
#define BDK_TESTING_SERVICES_CAPTURE_PDF 4u
#define BDK_TESTING_SERVICES_CAPTURE_STATE 5u
BDK_EXPORT bdk_status_t bdk_testing_services(uint32_t op,
                                             bdk_contents_t page,
                                             bdk_string_t a,
                                             bdk_string_t b,
                                             char* out,
                                             size_t n);

/* Work the engine has queued or started for the application: BDK tasks not yet
   run plus pages still loading. A harness pumps until it reads 0. */
BDK_EXPORT size_t bdk_testing_pending_work(void);

#ifdef __cplusplus
}
#endif

#endif /* BDK_ENGINE_BDK_TESTING_H_ */
