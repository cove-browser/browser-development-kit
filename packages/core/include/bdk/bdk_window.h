/* BDK C ABI: window registration. See c-abi.md section 5.5.
   Declared: registration. Named in c-abi.md but not declared because nothing states their shape:
   bdk_window_set_anchor_provider, bdk_window_set_request_handler, bdk_window_for_contents,
   bdk_window_contents, bdk_window_active_contents. */
#ifndef BDK_WINDOW_H_
#define BDK_WINDOW_H_

#include "bdk_base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* PROVISIONAL signatures. Registers a native window (BDK_NATIVE_NSWINDOW, HWND, ...) with its kind.
   Released by bdk_window_unregister. */
BDK_EXPORT bdk_status_t bdk_window_register(bdk_runtime_t, bdk_native_handle_t window,
                                            uint32_t window_kind, bdk_window_t* out, bdk_error_t*);
BDK_EXPORT bdk_status_t bdk_window_unregister(bdk_window_t, bdk_error_t*);

#ifdef __cplusplus
}
#endif

#endif /* BDK_WINDOW_H_ */
