/* Start-up, shutdown, the registration freeze, callback rules, subscriptions. */
#include <stddef.h>

#include "test_support.h"

static void test_start_and_shutdown(void) {
    t_start();
    CHECK(t_runtime != 0);
    t_stop();
}

static void test_start_twice_fails(void) {
    t_start();
    bdk_start_config_t c;
    t_fill_start_config(&c);
    bdk_runtime_t second = 7;
    bdk_error_t err = 0;
    CHECK(bdk_runtime_start(&c, &second, &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "runtime", "alreadyStarted"));
}

static void test_restart_after_shutdown(void) {
    t_start();
    t_stop();
    t_start();
    bdk_error_t err = 0;
    CHECK(bdk_runtime_freeze_registrations(t_runtime, &err) == BDK_OK);
}

static void test_version_mismatch_fails_before_anything_starts(void) {
    bdk_start_config_t c;
    t_fill_start_config(&c);
    c.host_abi_version = bdk_abi_version() + 1;
    bdk_runtime_t rt = 9;
    bdk_error_t err = 0;
    CHECK(bdk_runtime_start(&c, &rt, &err) == BDK_ERR_FAILED);
    CHECK(rt == 0);
    REQUIRE(err != 0);
    CHECK(str_is(bdk_error_domain(err), "abi"));
    CHECK(str_is(bdk_error_code(err), "versionMismatch"));
    CHECK(bdk_error_message(err).length > 0);
    bdk_error_release(err);
    t_start(); /* a failed start leaves nothing behind */
}

static void test_hash_mismatch_fails(void) {
    bdk_start_config_t c;
    t_fill_start_config(&c);
    c.host_abi_hash = S("0000000000000000000000000000000000000000000000000000000000000000");
    bdk_runtime_t rt = 0;
    bdk_error_t err = 0;
    CHECK(bdk_runtime_start(&c, &rt, &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "abi", "hashMismatch"));
    c.host_abi_hash = S("");
    CHECK(bdk_runtime_start(&c, &rt, &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "abi", "hashMismatch"));
}

static void test_invalid_resolved_configuration_fails(void) {
    const char* bad[] = {"not json", "[1,2]", "{\"a\":", "42", "{\"a\":1} trailing"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        bdk_start_config_t c;
        t_fill_start_config(&c);
        c.resolved_config_json = S(bad[i]);
        bdk_runtime_t rt = 0;
        bdk_error_t err = 0;
        CHECK(bdk_runtime_start(&c, &rt, &err) == BDK_ERR_FAILED);
        CHECK(t_error_is(err, "config", "resolvedInvalid"));
    }
    bdk_start_config_t c;
    t_fill_start_config(&c);
    c.resolved_config_json = S("");
    bdk_runtime_t rt = 0;
    bdk_error_t err = 0;
    CHECK(bdk_runtime_start(&c, &rt, &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "config", "resolvedMissing"));
}

static void test_start_rejects_missing_arguments(void) {
    bdk_start_config_t c;
    t_fill_start_config(&c);
    bdk_runtime_t rt = 0;
    bdk_error_t err = 0;
    CHECK(bdk_runtime_start(NULL, &rt, &err) == BDK_ERR_INVALID_ARGUMENT);
    CHECK(bdk_runtime_start(&c, NULL, &err) == BDK_ERR_INVALID_ARGUMENT);
    c.struct_size = 0;
    CHECK(bdk_runtime_start(&c, &rt, &err) == BDK_ERR_INVALID_ARGUMENT);
}

static void test_start_config_from_an_older_header_reads_missing_fields_as_zero(void) {
    bdk_start_config_t c;
    t_fill_start_config(&c);
    c.struct_size = offsetof(bdk_start_config_t, host_abi_version); /* stops before the version */
    bdk_runtime_t rt = 0;
    bdk_error_t err = 0;
    CHECK(bdk_runtime_start(&c, &rt, &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "abi", "versionMismatch")); /* version read as 0 */
}

static void test_start_config_from_a_newer_header_ignores_extra_fields(void) {
    struct {
        bdk_start_config_t base;
        unsigned char future[64];
    } big;
    memset(&big, 0x5A, sizeof big);
    t_fill_start_config(&big.base);
    big.base.struct_size = sizeof big;
    bdk_error_t err = 0;
    CHECK(bdk_runtime_start(&big.base, &t_runtime, &err) == BDK_OK);
}

static int g_registered_released;
static void app_noop(void* ctx, bdk_context_t call, bdk_string_t input) {
    (void)ctx;
    (void)call;
    (void)input;
}
static void count_release(void* ctx) {
    ++*(int*)ctx;
}

static void test_registration_after_freeze_fails(void) {
    t_start();
    bdk_error_t err = 0;
    CHECK(bdk_app_command_register(t_runtime, S("app.before"), S("{}"), app_noop, NULL, NULL,
                                   &err) == BDK_OK);
    CHECK(bdk_app_event_declare(t_runtime, S("app.beforeEvent"), S("{}"), &err) == BDK_OK);
    t_freeze();
    CHECK(bdk_app_command_register(t_runtime, S("app.after"), S("{}"), app_noop, NULL, NULL,
                                   &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "config", "registrationAfterStart"));
    CHECK(bdk_app_event_declare(t_runtime, S("app.afterEvent"), S("{}"), &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "config", "registrationAfterStart"));
    CHECK(bdk_app_setting_register(t_runtime, S("{\"key\":\"x\"}"), &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "config", "registrationAfterStart"));
    CHECK(bdk_page_handler_register(t_runtime, S("/x"), 1, NULL, NULL, NULL, &err) != BDK_OK);
    CHECK(bdk_ui_set_handler(t_runtime, BDK_SURFACE_PERMISSIONS, NULL, NULL, NULL, &err) != BDK_OK);
}

static void test_registered_context_is_released_once_at_shutdown_not_on_failure(void) {
    t_start();
    g_registered_released = 0;
    bdk_error_t err = 0;
    CHECK(bdk_app_command_register(t_runtime, S("app.one"), S("{}"), app_noop,
                                   &g_registered_released, count_release, &err) == BDK_OK);
    /* A failed registration leaves the context with the caller: no release callback. */
    CHECK(bdk_app_command_register(t_runtime, S("app.one"), S("{}"), app_noop,
                                   &g_registered_released, count_release, &err) == BDK_ERR_FAILED);
    bdk_error_release(err);
    t_pump();
    CHECK(g_registered_released == 0);
    t_freeze();
    CHECK(g_registered_released == 0);
    t_stop();
    CHECK(g_registered_released == 1);
}

static void test_contents_cannot_be_created_before_the_freeze(void) {
    t_start();
    bdk_profile_t p = 0;
    bdk_error_t err = 0;
    REQUIRE(bdk_profile_open_private(t_runtime, &p, &err) == BDK_OK);
    bdk_contents_options_t o;
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.profile = p;
    bdk_contents_t c = 0;
    CHECK(bdk_contents_create(t_runtime, &o, &c, &err) == BDK_ERR_NOT_STARTED);
    CHECK(c == 0);
    t_freeze();
    CHECK(bdk_contents_create(t_runtime, &o, &c, &err) == BDK_OK);
}

static void test_freezing_twice_fails(void) {
    t_start_frozen();
    bdk_error_t err = 0;
    CHECK(bdk_runtime_freeze_registrations(t_runtime, &err) == BDK_ERR_FAILED);
    CHECK(err != 0);
    bdk_error_release(err);
}

static void test_shutdown_stales_every_derived_handle(void) {
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Gone");
    bdk_contents_t c = t_new_contents(p);
    bdk_runtime_t rt = t_runtime;
    t_stop();
    bdk_error_t err = 0;
    CHECK(bdk_profile_close(p, &err) == BDK_ERR_STALE_HANDLE);
    CHECK(bdk_contents_destroy(c) == BDK_ERR_STALE_HANDLE);
    CHECK(bdk_runtime_freeze_registrations(rt, &err) == BDK_ERR_STALE_HANDLE);
    CHECK(bdk_runtime_shutdown(rt, t_on_result, NULL, NULL) == 0);
}

static void test_shutdown_cancels_pending_calls_before_its_own_result(void) {
    t_start_frozen();
    t_capture_t pending, down;
    t_cap_init(&pending);
    t_cap_init(&down);
    REQUIRE(bdk_profiles_list(t_runtime, t_on_result, &pending, t_on_release) != 0);
    REQUIRE(bdk_runtime_shutdown(t_runtime, t_on_result, &down, t_on_release) != 0);
    t_pump();
    t_runtime = 0;
    CHECK(pending.results == 1 && pending.releases == 1);
    CHECK(pending.has_error && strcmp(pending.domain, "broker") == 0 &&
          strcmp(pending.code, "cancelled") == 0);
    CHECK(down.results == 1 && down.releases == 1 && !down.has_error);
    CHECK(pending.result_seq < down.result_seq);
}

static void test_async_result_is_never_synchronous(void) {
    t_start_frozen();
    t_capture_t cap;
    t_cap_init(&cap);
    bdk_call_t call = bdk_profiles_list(t_runtime, t_on_result, &cap, t_on_release);
    CHECK(call != 0);
    CHECK(cap.results == 0 && cap.releases == 0);
    t_pump();
    CHECK(cap.results == 1 && cap.releases == 1);
    CHECK(cap.result_seq < cap.release_seq); /* the release callback follows the result */
    CHECK(!cap.has_error);
    CHECK(strcmp(cap.json, "[]") == 0);
}

static void test_call_handle_is_stale_after_completion(void) {
    t_start_frozen();
    t_capture_t cap;
    t_cap_init(&cap);
    bdk_call_t call = bdk_profiles_list(t_runtime, t_on_result, &cap, t_on_release);
    REQUIRE(call != 0);
    t_pump();
    CHECK(bdk_call_cancel(call) == BDK_ERR_STALE_HANDLE);
    t_pump();
    CHECK(cap.results == 1 && cap.releases == 1);
}

static void test_cancel_delivers_cancelled_exactly_once(void) {
    t_start_frozen();
    t_capture_t cap;
    t_cap_init(&cap);
    bdk_call_t call = bdk_profiles_list(t_runtime, t_on_result, &cap, t_on_release);
    REQUIRE(call != 0);
    CHECK(bdk_call_cancel(call) == BDK_OK);
    CHECK(bdk_call_cancel(call) == BDK_OK); /* a second cancel changes nothing */
    CHECK(cap.results == 0);                /* still never synchronous */
    t_pump();
    CHECK(cap.results == 1 && cap.releases == 1);
    CHECK(cap.has_error);
    CHECK(strcmp(cap.domain, "broker") == 0 && strcmp(cap.code, "cancelled") == 0);
    CHECK(cap.result_seq < cap.release_seq);
}

static bdk_runtime_t g_reentrant_runtime;
static void reenter(t_capture_t* outer) {
    /* A callback may call back into the ABI. */
    t_capture_t* inner = (t_capture_t*)outer->hook_data;
    CHECK(bdk_profiles_list(g_reentrant_runtime, t_on_result, inner, t_on_release) != 0);
    CHECK(inner->results == 0);
}

static void test_a_callback_may_call_back_into_the_abi(void) {
    t_start_frozen();
    g_reentrant_runtime = t_runtime;
    t_capture_t outer, inner;
    t_cap_init(&outer);
    t_cap_init(&inner);
    outer.on_result = reenter;
    outer.hook_data = &inner;
    REQUIRE(bdk_profiles_list(t_runtime, t_on_result, &outer, t_on_release) != 0);
    t_pump();
    CHECK(outer.results == 1 && inner.results == 1 && inner.releases == 1);
}

static void test_rejected_call_calls_nothing_back(void) {
    t_start_frozen();
    t_capture_t cap;
    t_cap_init(&cap);
    CHECK(bdk_profiles_list(0, t_on_result, &cap, t_on_release) == 0);
    CHECK(bdk_profiles_list(t_runtime, NULL, &cap, t_on_release) == 0);
    t_pump();
    CHECK(cap.results == 0 && cap.releases == 0);
}

const t_test_t t_runtime_tests[] = {
    {"runtime.start_and_shutdown", test_start_and_shutdown},
    {"runtime.start_twice_fails", test_start_twice_fails},
    {"runtime.restart_after_shutdown", test_restart_after_shutdown},
    {"runtime.version_mismatch_fails", test_version_mismatch_fails_before_anything_starts},
    {"runtime.hash_mismatch_fails", test_hash_mismatch_fails},
    {"runtime.invalid_resolved_configuration_fails", test_invalid_resolved_configuration_fails},
    {"runtime.start_rejects_missing_arguments", test_start_rejects_missing_arguments},
    {"runtime.start_config_older_header_reads_zero",
     test_start_config_from_an_older_header_reads_missing_fields_as_zero},
    {"runtime.start_config_newer_header_ignores_extra",
     test_start_config_from_a_newer_header_ignores_extra_fields},
    {"runtime.registration_after_freeze_fails", test_registration_after_freeze_fails},
    {"runtime.registered_context_released_once_at_shutdown",
     test_registered_context_is_released_once_at_shutdown_not_on_failure},
    {"runtime.contents_cannot_be_created_before_freeze",
     test_contents_cannot_be_created_before_the_freeze},
    {"runtime.freezing_twice_fails", test_freezing_twice_fails},
    {"runtime.shutdown_stales_derived_handles", test_shutdown_stales_every_derived_handle},
    {"runtime.shutdown_cancels_pending_calls_first",
     test_shutdown_cancels_pending_calls_before_its_own_result},
    {"callbacks.async_result_is_never_synchronous", test_async_result_is_never_synchronous},
    {"callbacks.call_handle_is_stale_after_completion", test_call_handle_is_stale_after_completion},
    {"callbacks.cancel_delivers_cancelled_exactly_once",
     test_cancel_delivers_cancelled_exactly_once},
    {"callbacks.callback_may_reenter_the_abi", test_a_callback_may_call_back_into_the_abi},
    {"callbacks.rejected_call_calls_nothing_back", test_rejected_call_calls_nothing_back},
    {NULL, NULL}};
