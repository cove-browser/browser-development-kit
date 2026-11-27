/* Test framework and helpers for the conformance suite. C11, no dependency beyond bdk/bdk.h and
 * harness.h. */
#ifndef BDK_TEST_SUPPORT_H_
#define BDK_TEST_SUPPORT_H_

#include <stdio.h>
#include <string.h>

#include "bdk/bdk.h"
#include "harness.h"

typedef struct t_test {
    const char* name;
    void (*fn)(void);
} t_test_t;

/* Every test file defines one table, ended by { NULL, NULL }. */
extern const t_test_t t_base_tests[];
extern const t_test_t t_runtime_tests[];
extern const t_test_t t_broker_tests[];
extern const t_test_t t_profile_tests[];
extern const t_test_t t_contents_tests[];
extern const t_test_t t_ui_tests[];
extern const t_test_t t_extension_tests[];
extern const t_test_t t_automation_tests[];
extern const t_test_t t_endpoint_tests[];
extern const t_test_t t_provider_tests[];
extern const t_test_t t_pages_tests[];
extern const t_test_t t_navigation_policy_tests[];
extern const t_test_t t_profile_version_tests[];
extern const t_test_t t_robustness_tests[];
extern const t_test_t t_web_app_tests[];
#ifdef BDK_HARNESS_ENGINE_TESTS
extern const t_test_t t_engine_tests[];
extern const t_test_t t_engine_ui_tests[];
extern const t_test_t t_engine_prompt_tests[];
extern const t_test_t t_engine_extension_tests[];
extern const t_test_t t_engine_extension_panel_tests[];
extern const t_test_t t_engine_extension_probe_tests[];
extern const t_test_t t_engine_extension_ui_tests[];
extern const t_test_t t_engine_automation_tests[];
extern const t_test_t t_engine_provider_tests[];
extern const t_test_t t_engine_privacy_tests[];
extern const t_test_t t_engine_pages_tests[];
extern const t_test_t t_engine_webauthn_tests[];
extern const t_test_t t_engine_dialogs_tests[];
extern const t_test_t t_engine_chooser_tests[];
extern const t_test_t t_engine_web_app_tests[];
extern const t_test_t t_engine_payment_tests[];
extern const t_test_t t_engine_client_certificate_tests[];
extern const t_test_t t_engine_identity_tests[];
extern const t_test_t t_engine_arc_tests[];
extern const t_test_t t_engine_navigation_policy_tests[];
extern const t_test_t t_engine_profile_version_tests[];
#endif
#ifdef BDK_HARNESS_OWNS_MAIN
int bdk_conformance_main(int argc, char** argv);
#endif

void t_fail(const char* file, int line, const char* expr);
void t_abort(void);

/* CHECK records a failure and continues. REQUIRE records it and ends the test. */
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c))                                                                                  \
            t_fail(__FILE__, __LINE__, #c);                                                        \
    } while (0)
#define REQUIRE(c)                                                                                 \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            t_fail(__FILE__, __LINE__, #c);                                                        \
            t_abort();                                                                             \
        }                                                                                          \
    } while (0)

bdk_string_t S(const char* s);
int str_is(bdk_string_t s, const char* expected);
int str_has(bdk_string_t s, const char* needle);

/* The runtime of the current test. t_start fills it in; the runner shuts it down after the test. */
extern bdk_runtime_t t_runtime;
bdk_runtime_t t_start(void); /* start, registrations open */
/* Start with this resolved configuration instead of the suite's minimal one. */
bdk_runtime_t t_start_with(const char* resolved_json);
bdk_runtime_t t_start_frozen(void); /* start and freeze */
void t_freeze(void);
void t_stop(void); /* shut down and pump; REQUIREs success */
void t_fill_start_config(bdk_start_config_t* c);
/* Members added to the resolved configuration of the next starts (such as
   "\"automation\":{...}"); NULL for none. A harness that writes its own configuration keeps them.
   The runner resets it after each test. */
extern const char* t_config_extra;
/* The Google key of the harness's first start, which Chromium keeps for the process: set by a
   launch's prepare hook (engine harness only; tests_engine_providers.c). */
extern const char* t_first_start_google_key;
void t_pump(void);

/* Records the callbacks of an asynchronous call. */
typedef struct t_capture {
    int results, releases, progress;
    int result_seq, release_seq;
    int has_error, has_binary;
    char domain[48], code[48], message[160], json[32768];
    double fraction;
    void (*on_result)(struct t_capture*); /* optional hook, runs inside the result callback */
    void* hook_data;
} t_capture_t;

void t_cap_init(t_capture_t*);
void t_on_result(void* ctx, bdk_error_t error, bdk_string_t json, bdk_buffer_t binary);
void t_on_progress(void* ctx, double fraction, bdk_string_t message);
void t_on_release(void* ctx);
int t_next_seq(void);

/* Error helpers: copy the domain and code of an error handle, then release it. */
void t_error_text(bdk_error_t e, char* domain, size_t dn, char* code, size_t cn);
int t_error_is(bdk_error_t e, const char* domain, const char* code); /* releases e */

/* Records contents events. */
#define T_MAX_EVENTS 24
typedef struct t_events {
    int count, releases;
    uint32_t kind[T_MAX_EVENTS], flags[T_MAX_EVENTS];
    int32_t net_error[T_MAX_EVENTS];
    char url[T_MAX_EVENTS][64], detail[T_MAX_EVENTS][96];
} t_events_t;
void t_ev_init(t_events_t*);
void t_on_contents_event(void* ctx, const bdk_contents_event_t* ev);
void t_on_event_release(void* ctx);

/* Shorthand for common sequences. */
void t_create_profile(const char* name, char* id_out, size_t n); /* creates and pumps; REQUIREs */
bdk_profile_t t_open_profile(const char* id);                    /* opens and pumps; REQUIREs */
bdk_profile_t t_new_profile(const char* name);                   /* create + open */
bdk_contents_t t_new_contents(bdk_profile_t);                    /* needs a frozen runtime */
int t_json_string(const char* json, const char* key, char* out, size_t n); /* first "key":"value" */
void t_buffer_text(bdk_buffer_t b, char* out, size_t n);

#endif /* BDK_TEST_SUPPORT_H_ */
