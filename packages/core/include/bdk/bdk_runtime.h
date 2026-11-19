/* BDK C ABI: runtime start-up and shutdown. See c-abi.md section 4. */
#ifndef BDK_RUNTIME_H_
#define BDK_RUNTIME_H_

#include "bdk_base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Every field after struct_size is optional for a caller built against an older header: a field
   beyond the declared struct_size reads as zero or empty. */
typedef struct bdk_start_config {
    size_t struct_size;
    bdk_string_t resolved_config_json; /* the contents of bdk.resolved.json */
    bdk_string_t bundle_path;    /* the application bundle; pages and scripts are read from here */
    bdk_string_t framework_path; /* where the runtime was loaded from */
    bdk_string_t user_data_path_override; /* tests only */
    bdk_string_t google_api_key;          /* empty unless a service uses provider = "google" */
    uint32_t host_abi_version;
    bdk_string_t host_abi_hash;
    bdk_native_handle_t native_app; /* tagged platform handle: the NSApplication */
} bdk_start_config_t;

/* Runtime events. PROVISIONAL: c-abi.md names bdk_runtime_event_cb without a signature. Kinds are
   BDK_RUNTIME_EVENT_* in bdk_ids.h; the strings are borrowed until the callback returns. */
typedef void (*bdk_runtime_event_cb)(void* context, uint32_t kind, bdk_string_t detail_json);

/* Checks the ABI, validates the resolved configuration and returns a runtime. The calling thread
   becomes the UI thread. On BDK_ERR_FAILED the caller owns *out_error. The runtime is released by
   bdk_runtime_shutdown. */
BDK_EXPORT bdk_status_t bdk_runtime_start(const bdk_start_config_t* config, bdk_runtime_t* out,
                                          bdk_error_t* out_error);

/* Shuts the runtime down. Every handle derived from it is stale as soon as this returns. A pending
   call is completed with broker.cancelled. The callback result is "{}" on success. */
BDK_EXPORT bdk_call_t bdk_runtime_shutdown(bdk_runtime_t, bdk_result_cb, void* ctx, bdk_release_cb);

/* Freezes the route and capability tables. Registration calls after this return
   config.registrationAfterStart. Contents cannot be created before the freeze. */
BDK_EXPORT bdk_status_t bdk_runtime_freeze_registrations(bdk_runtime_t, bdk_error_t* out_error);

BDK_EXPORT bdk_subscription_t bdk_runtime_subscribe(bdk_runtime_t, bdk_runtime_event_cb, void* ctx,
                                                    bdk_release_cb);
BDK_EXPORT void bdk_unsubscribe(bdk_subscription_t);

/* Runs fn(ctx) on the UI thread, never synchronously from inside this function. */
BDK_EXPORT BDK_ANY_THREAD void bdk_post_to_ui_thread(void (*fn)(void*), void* ctx);

#ifdef __cplusplus
}
#endif

#endif /* BDK_RUNTIME_H_ */
