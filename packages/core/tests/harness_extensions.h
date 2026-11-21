/* What the extension tests need from the runtime under test beyond tests/harness.h: packages it
   cannot read from disk, pages attached to registered windows, and acting as Chromium on an open
   request or a window. The stub offers them as test hooks (stub/bdk_stub.h); a runtime without
   them answers 0 from bdk_harness_extension_hooks and the tests that need them are skipped (the
   engine's own extension scenarios are in tests_engine_ext*.c). Plain C. */
#ifndef BDK_TEST_HARNESS_EXTENSIONS_H_
#define BDK_TEST_HARNESS_EXTENSIONS_H_

#include "bdk/bdk.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 1 if the runtime has these hooks and the stub's in-memory extensions, 0 if not. */
int bdk_harness_extension_hooks(void);

/* The manifest of a package (kind "unpacked", "crx", "updateUrl" or "store"; key the path, the id
   or "<store>/<id>"). A newer version for an installed id is what an update check finds. */
bdk_status_t bdk_harness_extension_package(bdk_runtime_t, const char* kind, const char* key,
                                           const char* manifest_json);
/* Attaches the page to a registered window (0 detaches it), as its view entering the window
   would. */
bdk_status_t bdk_harness_attach_to_window(bdk_contents_t, bdk_window_t);
/* Acts as an extension asking for a change to the window; the answer is BDK_UI_*, 0 before the
   handler ran. */
bdk_status_t bdk_harness_window_request(bdk_window_t, const char* request_json);
bdk_status_t bdk_harness_last_window_answer(uint32_t* out);
/* Acts as a default presenter asking for an anchor. */
bdk_status_t bdk_harness_window_anchor(bdk_window_t, uint32_t role, double out_rect[4],
                                       uint8_t* out_found);
/* Acts as the runtime updating the most recently raised request, and reads the last update the
   application sent to it. */
bdk_status_t bdk_harness_send_ui_update(bdk_runtime_t, const char* update_json);
bdk_status_t bdk_harness_last_ui_update(bdk_runtime_t, char* out, size_t cap);
/* How many times the default extensions menu was asked for since start. */
bdk_status_t bdk_harness_menus_presented(uint32_t* out);

#ifdef __cplusplus
}
#endif

#endif /* BDK_TEST_HARNESS_EXTENSIONS_H_ */
