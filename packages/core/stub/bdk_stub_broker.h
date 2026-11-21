/* Test hooks of the stub runtime for the broker. Not part of the ABI. A page's or a session's calls
   reach the broker through transports that arrive in M4 and M7; until then the conformance suite
   makes those callers here (through tests/harness_broker.h), and controls the stub's services so a
   test can see that a refused call never reached one. */
#ifndef BDK_STUB_BROKER_H_
#define BDK_STUB_BROKER_H_

#include "bdk/bdk.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BDK_STUB_CALLER_PAGE 1u
#define BDK_STUB_CALLER_SESSION 2u

typedef uint64_t bdk_stub_caller_t;

typedef struct bdk_stub_caller_spec {
    size_t struct_size;
    uint32_t kind; /* BDK_STUB_CALLER_PAGE or BDK_STUB_CALLER_SESSION */
    bdk_string_t name;
    bdk_string_t
        capabilities_json; /* a JSON array of capability names and app.<prefix>.* patterns */
    bdk_profile_t profile; /* a page: the profile it is in */
    /* A session's scope. */
    uint8_t all_profiles;
    const bdk_profile_t* scope_profiles;
    size_t scope_profile_count;
    uint8_t all_contents;
    const bdk_contents_t* scope_contents;
    size_t scope_contents_count;
} bdk_stub_caller_spec_t;

/* Creates a caller. Fails with BDK_ERR_FAILED (capability.denied) when a capability may not be
   granted to that kind of caller (a wildcard outside app., or one on the never-for-pages or
   never-for-sessions list). */
BDK_EXPORT bdk_status_t bdk_stub_caller_create(bdk_runtime_t, const bdk_stub_caller_spec_t*,
                                               bdk_stub_caller_t* out, bdk_error_t*);

/* bdk_invoke_named, as that caller. gesture is whether the call comes from a user gesture in the
   caller's page. Returns 0 when rejected synchronously. */
BDK_EXPORT bdk_call_t bdk_stub_invoke_as(bdk_runtime_t, bdk_stub_caller_t,
                                         bdk_string_t command_name, bdk_string_t arguments_json,
                                         const bdk_invoke_options_t*, uint8_t gesture,
                                         bdk_result_cb, bdk_progress_cb, void* ctx, bdk_release_cb);
/* The same, naming the command by identifier. */
BDK_EXPORT bdk_call_t bdk_stub_invoke_as_id(bdk_runtime_t, bdk_stub_caller_t, uint32_t command_id,
                                            bdk_string_t arguments_json,
                                            const bdk_invoke_options_t*, uint8_t gesture,
                                            bdk_result_cb, bdk_progress_cb, void* ctx,
                                            bdk_release_cb);

/* The caller's page navigated or closed, or its session was revoked. */
BDK_EXPORT bdk_status_t bdk_stub_caller_revoke(bdk_runtime_t, bdk_stub_caller_t);

/* How the operating system answers the next authentication requests: 0 refuses (the default),
   1 accepts, 2 waits for bdk_stub_answer_authentication. */
BDK_EXPORT bdk_status_t bdk_stub_set_authentication(bdk_runtime_t, uint32_t mode);
BDK_EXPORT bdk_status_t bdk_stub_answer_authentication(bdk_runtime_t, uint8_t granted);

/* When held, the stub's services take every built-in command and never answer it, so a test can
   cancel it. bdk_stub_release_held_commands answers them all, late. */
BDK_EXPORT bdk_status_t bdk_stub_hold_services(bdk_runtime_t, uint8_t hold);
BDK_EXPORT bdk_status_t bdk_stub_release_held_commands(bdk_runtime_t);

/* Internal pages (T-WUI-04). Asks for the page broker as the page's primary main frame would, or a
   subframe of it (subframe 1), with the URL the page committed. On success *out is a caller to use
   with bdk_stub_invoke_as, with the capabilities of the page's route; it is revoked when the page
   navigates or closes. Otherwise BDK_ERR_FAILED with capability.denied, and the page's renderer is
   reported gone (reason "badMessage"), as Chromium terminates a renderer that asks for an interface
   its page may not have. */
BDK_EXPORT bdk_status_t bdk_stub_page_bind(bdk_runtime_t, bdk_contents_t, uint8_t subframe,
                                           bdk_stub_caller_t* out, bdk_error_t*);
/* A navigation the page starts itself (a link, location.href), not the application. */
BDK_EXPORT bdk_status_t bdk_stub_page_navigate(bdk_contents_t, bdk_string_t url);
/* Subscribes to an event as that caller (broker_subscribe_as). 0 on refusal, with *out_error set.
 */
BDK_EXPORT bdk_subscription_t bdk_stub_subscribe_as(bdk_runtime_t, bdk_stub_caller_t,
                                                    bdk_string_t event, bdk_event_cb, void* ctx,
                                                    bdk_release_cb, bdk_error_t* out_error);

#define BDK_STUB_COUNT_EXECUTED 0u        /* calls that reached a service */
#define BDK_STUB_COUNT_HELD 1u            /* calls a service is holding */
#define BDK_STUB_COUNT_STOPPED 2u         /* calls a service was told to stop */
#define BDK_STUB_COUNT_AUTHENTICATIONS 3u /* times the operating system was asked */
#define BDK_STUB_COUNT_LIVE_CALLS 4u      /* calls of the running runtime still allocated */
BDK_EXPORT uint32_t bdk_stub_service_count(bdk_runtime_t, uint32_t which);

#ifdef __cplusplus
}
#endif

#endif /* BDK_STUB_BROKER_H_ */
