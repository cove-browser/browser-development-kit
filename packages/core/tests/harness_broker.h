/* What the broker tests need from the runtime under test beyond tests/harness.h: callers that are
   not the host (an internal page, an automation session) and control of the services behind the
   broker. Those callers' transports arrive in M4 and M7, so a runtime offers them through test
   hooks, as the stub does (stub/bdk_stub_broker.h). A runtime without the hooks answers 0 from
   bdk_harness_broker_hooks and the tests that need them are skipped. Plain C. */
#ifndef BDK_TEST_HARNESS_BROKER_H_
#define BDK_TEST_HARNESS_BROKER_H_

#include "bdk/bdk.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 1 if the runtime has these hooks and implements the stub's in-memory services (settings, history,
   bookmarks, permissions, passwords, addresses, privacy.clear, ...), 0 if not. */
int bdk_harness_broker_hooks(void);

#define BDK_HARNESS_CALLER_PAGE 1u
#define BDK_HARNESS_CALLER_SESSION 2u

typedef uint64_t bdk_harness_caller_t;

typedef struct bdk_harness_caller_spec {
    uint32_t kind; /* BDK_HARNESS_CALLER_* */
    const char* name;
    const char*
        capabilities_json; /* a JSON array of capability names and app.<prefix>.* patterns */
    bdk_profile_t profile; /* a page: the profile it is in */
    int all_profiles;      /* a session's scope */
    const bdk_profile_t* scope_profiles;
    size_t scope_profile_count;
    int all_contents;
    const bdk_contents_t* scope_contents;
    size_t scope_contents_count;
} bdk_harness_caller_spec_t;

/* BDK_ERR_FAILED with capability.denied when a capability may not be granted to that kind. */
bdk_status_t bdk_harness_caller_create(bdk_runtime_t, const bdk_harness_caller_spec_t*,
                                       bdk_harness_caller_t* out, bdk_error_t*);
/* bdk_invoke_named as that caller; gesture says the page had a user gesture. */
bdk_call_t bdk_harness_invoke_as(bdk_runtime_t, bdk_harness_caller_t, const char* command,
                                 const char* arguments_json, const bdk_invoke_options_t*,
                                 int gesture, bdk_result_cb, bdk_progress_cb, void* ctx,
                                 bdk_release_cb);
bdk_call_t bdk_harness_invoke_as_id(bdk_runtime_t, bdk_harness_caller_t, uint32_t command_id,
                                    const char* arguments_json, const bdk_invoke_options_t*,
                                    int gesture, bdk_result_cb, bdk_progress_cb, void* ctx,
                                    bdk_release_cb);
/* The caller's page navigated or closed, or its session was revoked. */
bdk_status_t bdk_harness_caller_revoke(bdk_runtime_t, bdk_harness_caller_t);

/* How the operating system answers authentication: 0 refuses, 1 accepts, 2 waits for
   bdk_harness_answer_authentication. */
bdk_status_t bdk_harness_set_authentication(bdk_runtime_t, int mode);
bdk_status_t bdk_harness_answer_authentication(bdk_runtime_t, int granted);

/* While held, services take built-in commands and never answer them. */
bdk_status_t bdk_harness_hold_services(bdk_runtime_t, int hold);
/* Answers every held command, late. */
bdk_status_t bdk_harness_release_held_commands(bdk_runtime_t);

/* Internal pages (T-WUI-04). 1 if the runtime can act as an internal page asking for the broker
   and as a page starting a navigation itself; 0 skips the tests that need it. */
int bdk_harness_page_hooks(void);
/* Asks for the page broker as the page's primary main frame (subframe 0) or a subframe would. On
   success the caller works with bdk_harness_invoke_as; on refusal BDK_ERR_FAILED and the page's
   renderer is reported gone. */
bdk_status_t bdk_harness_page_bind(bdk_runtime_t, bdk_contents_t, int subframe,
                                   bdk_harness_caller_t* out, bdk_error_t*);
/* A navigation the page starts (a link or location.href), not the application. */
bdk_status_t bdk_harness_page_navigate(bdk_contents_t, const char* url);
/* Subscribes to an event as that caller; 0 on refusal with *out_error set. */
bdk_subscription_t bdk_harness_subscribe_as(bdk_runtime_t, bdk_harness_caller_t, const char* event,
                                            bdk_event_cb, void* ctx, bdk_release_cb,
                                            bdk_error_t* out_error);

#define BDK_HARNESS_COUNT_EXECUTED 0u        /* calls that reached a service */
#define BDK_HARNESS_COUNT_HELD 1u            /* calls a service is holding */
#define BDK_HARNESS_COUNT_STOPPED 2u         /* calls a service was told to stop */
#define BDK_HARNESS_COUNT_AUTHENTICATIONS 3u /* times the operating system was asked */
#define BDK_HARNESS_COUNT_LIVE_CALLS 4u      /* calls of the running runtime still allocated */
unsigned bdk_harness_service_count(bdk_runtime_t, unsigned which);

#ifdef __cplusplus
}
#endif

#endif /* BDK_TEST_HARNESS_BROKER_H_ */
