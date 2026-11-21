/* BDK C ABI: command invocation, events, application command registration. See c-abi.md section 6.
 */
#ifndef BDK_BROKER_H_
#define BDK_BROKER_H_

#include "bdk_base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bdk_invoke_options {
    size_t struct_size;
    bdk_profile_t profile;   /* 0 for application-scoped commands */
    bdk_contents_t contents; /* 0 unless the command targets a page */
} bdk_invoke_options_t;

/* PROVISIONAL: c-abi.md names bdk_event_cb without a signature. */
typedef void (*bdk_event_cb)(void* context, uint32_t event_id, bdk_string_t name,
                             bdk_string_t payload_json);

/* Calls made through the ABI have full authority. A return of 0 means the call was rejected
   synchronously (stale handle, wrong thread, bad struct): nothing is called back, not even the
   release callback. Otherwise the result callback is called exactly once, later, then the release
   callback. */
BDK_EXPORT bdk_call_t bdk_invoke(bdk_runtime_t, uint32_t command_id, bdk_string_t arguments_json,
                                 const bdk_invoke_options_t*, bdk_result_cb,
                                 bdk_progress_cb /* may be null */, void* ctx, bdk_release_cb);
BDK_EXPORT bdk_call_t bdk_invoke_named(bdk_runtime_t, bdk_string_t command_name,
                                       bdk_string_t arguments_json, const bdk_invoke_options_t*,
                                       bdk_result_cb, bdk_progress_cb, void* ctx, bdk_release_cb);

/* filter_json may be empty. Released by bdk_unsubscribe or bdk_release. */
BDK_EXPORT bdk_subscription_t bdk_subscribe(bdk_runtime_t, uint32_t event_id,
                                            bdk_string_t filter_json, const bdk_invoke_options_t*,
                                            bdk_event_cb, void* ctx, bdk_release_cb);
/* PROVISIONAL, not in c-abi.md: the by-name pair of bdk_subscribe, needed to observe application
   events, which have names and no generated identifier. */
BDK_EXPORT bdk_subscription_t bdk_subscribe_named(bdk_runtime_t, bdk_string_t event_name,
                                                  bdk_string_t filter_json,
                                                  const bdk_invoke_options_t*, bdk_event_cb,
                                                  void* ctx, bdk_release_cb);

/* Application commands. The handler answers later with one of progress, resolve or reject. */
typedef void (*bdk_app_command_cb)(void* context, bdk_context_t call_context,
                                   bdk_string_t input_json);
BDK_EXPORT void bdk_app_command_progress(bdk_context_t, double fraction, bdk_string_t message);
BDK_EXPORT void bdk_app_command_resolve(bdk_context_t, bdk_string_t output_json);
BDK_EXPORT void bdk_app_command_reject(bdk_context_t, bdk_string_t domain, bdk_string_t code,
                                       bdk_string_t message);
BDK_EXPORT bdk_string_t bdk_app_command_caller_json(bdk_context_t);
/* The profile the call addresses, apart from who called: the one the invoke options named, or a
   page caller's own. 0 when the call addresses none or the profile has closed. */
BDK_EXPORT bdk_profile_t bdk_app_command_profile(bdk_context_t);
BDK_EXPORT uint8_t bdk_app_command_is_cancelled(bdk_context_t);

/* PROVISIONAL: c-abi.md names bdk_native_page_cb without a signature. A navigation to a route in
   native mode calls it later, on the UI thread, with request_json
   {"route":"downloads","url":"<scheme>://downloads/recent?x=1","path":"/recent","query":"x=1"}.
   The navigation is cancelled in the page that asked. The handler answers once, now or later, with
   bdk_native_page_complete; BDK_NATIVE_PAGE_BLOCKED shows the standard error page. The page that
   asked is bdk_native_page_source (0 for none) and its profile bdk_app_command_profile. */
typedef void (*bdk_native_page_cb)(void* context, bdk_context_t call_context,
                                   bdk_string_t request_json);
BDK_EXPORT void bdk_native_page_complete(bdk_context_t, uint32_t result);
BDK_EXPORT bdk_contents_t bdk_native_page_source(bdk_context_t);

/* All registration functions must be called between bdk_runtime_start and
   bdk_runtime_freeze_registrations; afterwards they fail with config.registrationAfterStart. */
BDK_EXPORT bdk_status_t bdk_app_command_register(bdk_runtime_t, bdk_string_t name,
                                                 bdk_string_t declaration_json, bdk_app_command_cb,
                                                 void* ctx, bdk_release_cb, bdk_error_t*);
BDK_EXPORT bdk_status_t bdk_app_event_declare(bdk_runtime_t, bdk_string_t name,
                                              bdk_string_t declaration_json, bdk_error_t*);
BDK_EXPORT bdk_status_t bdk_app_event_emit(bdk_runtime_t, bdk_string_t name,
                                           bdk_string_t payload_json, bdk_string_t target_json,
                                           bdk_error_t*);
BDK_EXPORT bdk_status_t bdk_app_setting_register(bdk_runtime_t, bdk_string_t declaration_json,
                                                 bdk_error_t*);
/* Sets how a logical route is served, over the resolved configuration (architecture.md 8.2): mode
   is a BDK_PAGE_MODE_*. CHROMIUM needs a route with a built-in page (config.noBuiltin); BUNDLED a
   route that is a bundled page in the resolved configuration (config.routeUnknown); NATIVE a
   handler; DISABLED any route but credits (config.routeMode). An alias, a reserved name or a name
   that is not a route name is config.routeInvalid. The callback is used only by NATIVE and may be
   NULL otherwise; ctx is released once at shutdown in every mode. */
BDK_EXPORT bdk_status_t bdk_page_handler_register(bdk_runtime_t, bdk_string_t route, uint32_t mode,
                                                  bdk_native_page_cb, void* ctx, bdk_release_cb,
                                                  bdk_error_t*);

#ifdef __cplusplus
}
#endif

#endif /* BDK_BROKER_H_ */
