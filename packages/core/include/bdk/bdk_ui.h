/* BDK C ABI: UI request delivery and completion. See c-abi.md section 8. Surfaces are BDK_SURFACE_*
   in bdk_ids.h. */
#ifndef BDK_UI_H_
#define BDK_UI_H_

#include "bdk_base.h"
#include "bdk_ids.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bdk_ui_request_info {
    size_t struct_size;
    bdk_request_t request;
    uint32_t surface;        /* BDK_SURFACE_* */
    bdk_contents_t contents; /* 0 if none */
    bdk_window_t window;     /* 0 if none */
    bdk_profile_t profile;
    bdk_string_t payload_json; /* the fields in ui-requests.md for this surface */
    double anchor[4];          /* x, y, width, height in window coordinates; all zero if none */
    bdk_native_handle_t secure_view; /* for popups, side panels and hosted front ends */
} bdk_ui_request_info_t;

typedef enum bdk_ui_response {
    BDK_UI_HANDLED = 1,
    BDK_UI_USE_DEFAULT = 2,
    BDK_UI_REJECT = 3
} bdk_ui_response_t;

typedef bdk_ui_response_t (*bdk_ui_handler_cb)(void* context, const bdk_ui_request_info_t*);

BDK_EXPORT bdk_status_t bdk_ui_set_handler(bdk_runtime_t, uint32_t surface, bdk_ui_handler_cb,
                                           void* ctx, bdk_release_cb, bdk_error_t*);
/* A completion marked as granting requires gesture_event (kind != 0); otherwise ui.gestureRequired.
 */
BDK_EXPORT bdk_status_t bdk_ui_complete(bdk_request_t,
                                        bdk_string_t completion /* "grant", "deny", "accept" */,
                                        bdk_string_t arguments_json,
                                        bdk_native_handle_t gesture_event, bdk_error_t*);
BDK_EXPORT bdk_status_t bdk_ui_use_default(bdk_request_t, bdk_error_t*);
BDK_EXPORT void bdk_ui_set_withdrawn_handler(bdk_request_t, void (*cb)(void*), void* ctx,
                                             bdk_release_cb);
/* An uncompleted request is cancelled. */
BDK_EXPORT void bdk_ui_request_release(bdk_request_t);

/* Some requests stay open while their UI is up, and end with a completion or a withdrawal: an
   extension popup or side panel (extensions.md 4.2, 4.3). While one is open, either side can
   report a change without ending it.

   The runtime's changes (a popup's preferred size, whether a page-scoped panel should be visible)
   reach the update handler as JSON objects, in order, later, until the request finishes. Setting
   a handler releases the previous one; the last is released when the request finishes. */
typedef void (*bdk_ui_update_cb)(void* context, bdk_request_t request, bdk_string_t update_json);
BDK_EXPORT void bdk_ui_set_update_handler(bdk_request_t, bdk_ui_update_cb, void* ctx,
                                          bdk_release_cb);
/* The application's changes (it placed the view, the side a panel is on), as a JSON object. Fails
   with ui.requestCompleted once the request finished. */
BDK_EXPORT bdk_status_t bdk_ui_update(bdk_request_t, bdk_string_t update_json, bdk_error_t*);

#ifdef __cplusplus
}
#endif

#endif /* BDK_UI_H_ */
