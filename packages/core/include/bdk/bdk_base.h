/* BDK C ABI: base types. See docs/planning/sdk/c-abi.md section 2 and
 * packages/core/docs/conventions.md. */
#ifndef BDK_BASE_H_
#define BDK_BASE_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define BDK_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define BDK_EXPORT __attribute__((visibility("default")))
#else
#define BDK_EXPORT
#endif

/* Marks a function or callback that may be used on any thread. Everything else is UI thread only.
 */
#define BDK_ANY_THREAD

#define BDK_ABI_VERSION 1

/* A handle is an identifier, not a pointer: type tag, generation, index. Zero is never valid. */
typedef uint64_t bdk_handle_t;
typedef bdk_handle_t bdk_runtime_t, bdk_profile_t, bdk_contents_t, bdk_window_t, bdk_view_t,
    bdk_request_t, bdk_call_t, bdk_subscription_t, bdk_session_t, bdk_task_t, bdk_error_t,
    bdk_buffer_t, bdk_context_t;

/* UTF-8, explicit length, not necessarily terminated. Borrowed for the duration of the call or
 * callback. */
typedef struct bdk_string {
    const char* data;
    size_t length;
} bdk_string_t;

/* Bytes. Borrowed for the duration of the call or callback. */
typedef struct bdk_bytes {
    const uint8_t* data;
    size_t length;
} bdk_bytes_t;

typedef enum bdk_status {
    BDK_OK = 0,
    BDK_ERR_INVALID_ARGUMENT = 1,
    BDK_ERR_STALE_HANDLE = 2,
    BDK_ERR_WRONG_THREAD = 3,
    BDK_ERR_NOT_STARTED = 4,
    BDK_ERR_FAILED = 5 /* details are in the error out-parameter */
} bdk_status_t;

/* The one place platform objects cross, and only as an opaque tagged value (section 5.4). */
typedef enum bdk_native_kind {
    BDK_NATIVE_NSVIEW = 1,
    BDK_NATIVE_NSWINDOW = 2,
    BDK_NATIVE_NSAPPLICATION = 3,
    BDK_NATIVE_NSEVENT = 4,
    BDK_NATIVE_HWND = 16,
    BDK_NATIVE_X11_WINDOW = 32,
    BDK_NATIVE_WAYLAND_SURFACE = 33
} bdk_native_kind_t;

typedef struct bdk_native_handle {
    uint32_t kind; /* bdk_native_kind_t; 0 means none */
    void* pointer;
    uint64_t id;
} bdk_native_handle_t;

/* Releases any owned handle (error, buffer, contents, profile, subscription, ...). Returns
   BDK_ERR_STALE_HANDLE for a handle that is stale, mistyped or already released, and
   BDK_ERR_INVALID_ARGUMENT for a live handle that is not released this way: a runtime
   (bdk_runtime_shutdown), a call (BDK releases it after the result callback) and a call context
   (bdk_app_command_resolve or _reject). */
BDK_EXPORT bdk_status_t bdk_release(bdk_handle_t);

/* Errors (section 2.1). Strings are borrowed from the error and valid until it is released. */
BDK_EXPORT bdk_string_t bdk_error_domain(bdk_error_t);
BDK_EXPORT bdk_string_t bdk_error_code(bdk_error_t);
BDK_EXPORT bdk_string_t bdk_error_message(bdk_error_t);
BDK_EXPORT bdk_string_t bdk_error_details_json(bdk_error_t);
BDK_EXPORT void bdk_error_release(bdk_error_t);

/* Buffers. A buffer is owned by the caller and released with bdk_release. The bytes are borrowed
   from the buffer and valid until it is released. */
BDK_EXPORT bdk_bytes_t bdk_buffer_bytes(bdk_buffer_t);

/* Asynchronous calls (section 2.2). */
typedef void (*bdk_result_cb)(void* context, bdk_error_t error /* 0 on success */,
                              bdk_string_t result_json,
                              bdk_buffer_t binary /* 0 if none; borrowed */);
typedef void (*bdk_progress_cb)(void* context, double fraction, bdk_string_t message);
/* Called exactly once, when BDK will not use context again. */
typedef void (*bdk_release_cb)(void* context);

BDK_EXPORT bdk_status_t bdk_call_cancel(bdk_call_t);

/* Version and hash (section 3). */
BDK_EXPORT BDK_ANY_THREAD uint32_t bdk_abi_version(void);
BDK_EXPORT BDK_ANY_THREAD bdk_string_t bdk_abi_hash(void); /* lower-case hex SHA-256 */
BDK_EXPORT BDK_ANY_THREAD bdk_string_t bdk_runtime_manifest_json(void);

#ifdef __cplusplus
}
#endif

#endif /* BDK_BASE_H_ */
