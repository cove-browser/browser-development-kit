/* BDK C ABI: native view handles, focus and keyboard. See c-abi.md section 5.4. The native handle
   types are in bdk_base.h because the start configuration also carries one. */
#ifndef BDK_VIEW_H_
#define BDK_VIEW_H_

#include "bdk_base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* PROVISIONAL: c-abi.md names both callbacks without signatures. The event is an opaque tagged
   native handle (BDK_NATIVE_NSEVENT on macOS). A reserve callback returns 1 to keep the key for the
   application. */
typedef uint8_t (*bdk_key_reserve_cb)(void* context, bdk_native_handle_t key_event);
typedef void (*bdk_key_unhandled_cb)(void* context, bdk_native_handle_t key_event);

/* Attachment has no function: the engine observes the view entering and leaving a native window. */
BDK_EXPORT bdk_status_t bdk_contents_native_view(bdk_contents_t, bdk_native_handle_t* out,
                                                 bdk_error_t*);
BDK_EXPORT bdk_status_t bdk_contents_focus(bdk_contents_t);
BDK_EXPORT void bdk_contents_set_keyboard_handler(bdk_contents_t, bdk_key_reserve_cb,
                                                  bdk_key_unhandled_cb, void* ctx, bdk_release_cb);

#ifdef __cplusplus
}
#endif

#endif /* BDK_VIEW_H_ */
