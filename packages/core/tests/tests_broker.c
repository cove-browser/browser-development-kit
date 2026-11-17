/* Broker: values, application commands, cancellation, events, registration of settings, handlers,
 * providers. */
#include "test_support.h"

/* An application command that echoes its input back. */
static void echo_handler(void* ctx, bdk_context_t call, bdk_string_t input) {
    ++*(int*)ctx;
    bdk_app_command_resolve(call, input);
}

/* An application command that never answers by itself; the test keeps the call context. */
static bdk_context_t g_held;
static int g_held_seen_cancelled;
static void hold_handler(void* ctx, bdk_context_t call, bdk_string_t input) {
    (void)input;
    ++*(int*)ctx;
    g_held = call;
}

static void progress_handler(void* ctx, bdk_context_t call, bdk_string_t input) {
    (void)ctx;
    (void)input;
    bdk_app_command_progress(call, 0.25, S("a quarter"));
    bdk_app_command_progress(call, 0.75, S("three quarters"));
    bdk_app_command_resolve(call, S("{\"done\":true}"));
}

static void reject_handler(void* ctx, bdk_context_t call, bdk_string_t input) {
    (void)ctx;
    (void)input;
    bdk_app_command_reject(call, S("capability"), S("denied"), S("not for you"));
}

static void caller_handler(void* ctx, bdk_context_t call, bdk_string_t input) {
    (void)ctx;
    (void)input;
    bdk_string_t who = bdk_app_command_caller_json(call);
    char out[160];
    snprintf(out, sizeof out, "{\"caller\":%.*s}", (int)who.length, who.data);
    bdk_app_command_resolve(call, S(out));
}

static void bad_output_handler(void* ctx, bdk_context_t call, bdk_string_t input) {
    (void)ctx;
    (void)input;
    bdk_app_command_resolve(call, S("{not json"));
}

static int g_calls;

static void register_all(void) {
    bdk_error_t err = 0;
    REQUIRE(bdk_app_command_register(t_runtime, S("app.echo"), S("{}"), echo_handler, &g_calls,
                                     NULL, &err) == BDK_OK);
    REQUIRE(bdk_app_command_register(t_runtime, S("app.hold"), S("{}"), hold_handler, &g_calls,
                                     NULL, &err) == BDK_OK);
    REQUIRE(bdk_app_command_register(t_runtime, S("app.progress"), S("{}"), progress_handler,
                                     &g_calls, NULL, &err) == BDK_OK);
    REQUIRE(bdk_app_command_register(t_runtime, S("app.reject"), S("{}"), reject_handler, &g_calls,
                                     NULL, &err) == BDK_OK);
    REQUIRE(bdk_app_command_register(t_runtime, S("app.caller"), S("{}"), caller_handler, &g_calls,
                                     NULL, &err) == BDK_OK);
    REQUIRE(bdk_app_command_register(t_runtime, S("app.badOutput"), S("{}"), bad_output_handler,
                                     &g_calls, NULL, &err) == BDK_OK);
}

static void start_with_commands(void) {
    g_calls = 0;
    g_held = 0;
    t_start();
    register_all();
    t_freeze();
}

static void test_json_values_cross_verbatim(void) {
    start_with_commands();
    const char* inputs[] = {
        "{\"a\":[1,2.5e3,-0],\"b\":\"caf\\u00e9 \\\"q\\\" \\n\",\"c\":null,\"d\":true}",
        "[]",
        "\"x\"",
        "42",
        "{\"nested\":{\"deep\":{\"deeper\":[{}]}}}",
        "{\"utf8\":\"caf\xc3\xa9 \xe2\x82\xac\"}"};
    for (size_t i = 0; i < sizeof inputs / sizeof inputs[0]; ++i) {
        t_capture_t cap;
        t_cap_init(&cap);
        REQUIRE(bdk_invoke_named(t_runtime, S("app.echo"), S(inputs[i]), NULL, t_on_result, NULL,
                                 &cap, t_on_release) != 0);
        CHECK(cap.results == 0);
        t_pump();
        CHECK(cap.results == 1 && cap.releases == 1);
        CHECK(!cap.has_error);
        CHECK(strcmp(cap.json, inputs[i]) == 0);
    }
    CHECK(g_calls == (int)(sizeof inputs / sizeof inputs[0]));
}

static void test_invalid_json_arguments_are_rejected_before_the_handler(void) {
    start_with_commands();
    const char* bad[] = {"", "{", "{'a':1}", "[1,]", "nul", "{\"a\":1}x", "01"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        t_capture_t cap;
        t_cap_init(&cap);
        REQUIRE(bdk_invoke_named(t_runtime, S("app.echo"), S(bad[i]), NULL, t_on_result, NULL, &cap,
                                 t_on_release) != 0);
        t_pump();
        CHECK(cap.results == 1 && cap.has_error);
        CHECK(strcmp(cap.domain, "broker") == 0 && strcmp(cap.code, "invalidArguments") == 0);
    }
    CHECK(g_calls == 0);
}

static void test_unknown_command_is_an_error_not_a_crash(void) {
    start_with_commands();
    t_capture_t a, b;
    t_cap_init(&a);
    t_cap_init(&b);
    REQUIRE(bdk_invoke_named(t_runtime, S("app.nothing"), S("{}"), NULL, t_on_result, NULL, &a,
                             t_on_release) != 0);
    REQUIRE(bdk_invoke(t_runtime, 0xFFFFFF00u, S("{}"), NULL, t_on_result, NULL, &b,
                       t_on_release) != 0);
    t_pump();
    CHECK(a.results == 1 && a.has_error && strcmp(a.code, "unknownCommand") == 0);
    CHECK(b.results == 1 && b.has_error && strcmp(b.code, "unknownCommand") == 0);
    CHECK(strcmp(a.domain, "broker") == 0 && strcmp(b.domain, "broker") == 0);
}

static void test_invoke_options_must_name_live_objects(void) {
    start_with_commands();
    t_capture_t cap;
    t_cap_init(&cap);
    bdk_invoke_options_t o;
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    bdk_profile_t gone = t_new_profile("Gone");
    bdk_error_t err = 0;
    REQUIRE(bdk_profile_close(gone, &err) == BDK_OK);
    o.profile = gone; /* a released handle */
    CHECK(bdk_invoke_named(t_runtime, S("app.echo"), S("{}"), &o, t_on_result, NULL, &cap,
                           t_on_release) == 0);
    o.profile = 0;
    o.struct_size = 0;
    CHECK(bdk_invoke_named(t_runtime, S("app.echo"), S("{}"), &o, t_on_result, NULL, &cap,
                           t_on_release) == 0);
    t_pump();
    CHECK(cap.results == 0 && cap.releases == 0);
    bdk_profile_t p = t_new_profile("Scope");
    o.struct_size = sizeof o;
    o.profile = p;
    CHECK(bdk_invoke_named(t_runtime, S("app.echo"), S("{}"), &o, t_on_result, NULL, &cap,
                           t_on_release) != 0);
    t_pump();
    CHECK(cap.results == 1 && !cap.has_error);
}

static void test_progress_arrives_before_the_result_in_order(void) {
    start_with_commands();
    t_capture_t cap;
    t_cap_init(&cap);
    REQUIRE(bdk_invoke_named(t_runtime, S("app.progress"), S("{}"), NULL, t_on_result,
                             t_on_progress, &cap, t_on_release) != 0);
    t_pump();
    CHECK(cap.progress == 2);
    CHECK(cap.fraction == 0.75);
    CHECK(cap.results == 1 && strcmp(cap.json, "{\"done\":true}") == 0);
}

static void test_rejection_carries_domain_code_and_message(void) {
    start_with_commands();
    t_capture_t cap;
    t_cap_init(&cap);
    REQUIRE(bdk_invoke_named(t_runtime, S("app.reject"), S("{}"), NULL, t_on_result, NULL, &cap,
                             t_on_release) != 0);
    t_pump();
    CHECK(cap.results == 1 && cap.has_error);
    CHECK(strcmp(cap.domain, "capability") == 0 && strcmp(cap.code, "denied") == 0);
    CHECK(strcmp(cap.message, "not for you") == 0);
    CHECK(cap.json[0] == '\0');
}

static void test_command_resolving_invalid_json_fails_the_call(void) {
    start_with_commands();
    t_capture_t cap;
    t_cap_init(&cap);
    REQUIRE(bdk_invoke_named(t_runtime, S("app.badOutput"), S("{}"), NULL, t_on_result, NULL, &cap,
                             t_on_release) != 0);
    t_pump();
    CHECK(cap.results == 1 && cap.has_error);
}

static void test_caller_json_is_json(void) {
    start_with_commands();
    t_capture_t cap;
    t_cap_init(&cap);
    REQUIRE(bdk_invoke_named(t_runtime, S("app.caller"), S("{}"), NULL, t_on_result, NULL, &cap,
                             t_on_release) != 0);
    t_pump();
    CHECK(cap.results == 1 && !cap.has_error);
    CHECK(strstr(cap.json, "{\"caller\":{") == cap.json);
}

static void test_handler_is_never_called_synchronously(void) {
    start_with_commands();
    t_capture_t cap;
    t_cap_init(&cap);
    REQUIRE(bdk_invoke_named(t_runtime, S("app.echo"), S("{}"), NULL, t_on_result, NULL, &cap,
                             t_on_release) != 0);
    CHECK(g_calls == 0);
    t_pump();
    CHECK(g_calls == 1);
}

static void test_cancelling_a_command_marks_its_context_and_ignores_a_late_answer(void) {
    start_with_commands();
    t_capture_t cap;
    t_cap_init(&cap);
    bdk_call_t call = bdk_invoke_named(t_runtime, S("app.hold"), S("{}"), NULL, t_on_result, NULL,
                                       &cap, t_on_release);
    REQUIRE(call != 0);
    t_pump();
    REQUIRE(g_held != 0);
    CHECK(bdk_app_command_is_cancelled(g_held) == 0);
    CHECK(cap.results == 0);
    CHECK(bdk_call_cancel(call) == BDK_OK);
    g_held_seen_cancelled = bdk_app_command_is_cancelled(g_held);
    CHECK(g_held_seen_cancelled == 1);
    t_pump();
    CHECK(cap.results == 1 && cap.has_error && strcmp(cap.code, "cancelled") == 0 &&
          strcmp(cap.domain, "broker") == 0);
    bdk_app_command_resolve(g_held, S("{\"late\":true}")); /* ignored */
    t_pump();
    CHECK(cap.results == 1 && cap.releases == 1);
}

static void test_a_held_command_can_be_answered_later(void) {
    start_with_commands();
    t_capture_t cap;
    t_cap_init(&cap);
    REQUIRE(bdk_invoke_named(t_runtime, S("app.hold"), S("{}"), NULL, t_on_result, NULL, &cap,
                             t_on_release) != 0);
    t_pump();
    CHECK(cap.results == 0);
    bdk_app_command_resolve(g_held, S("[1,2,3]"));
    CHECK(cap.results == 0); /* resolve never calls back synchronously either */
    t_pump();
    CHECK(cap.results == 1 && strcmp(cap.json, "[1,2,3]") == 0);
    bdk_app_command_resolve(g_held, S("[4]")); /* the context is spent: ignored */
    t_pump();
    CHECK(cap.results == 1);
}

static void test_shutdown_cancels_a_held_command(void) {
    start_with_commands();
    t_capture_t cap;
    t_cap_init(&cap);
    REQUIRE(bdk_invoke_named(t_runtime, S("app.hold"), S("{}"), NULL, t_on_result, NULL, &cap,
                             t_on_release) != 0);
    t_pump();
    t_stop();
    CHECK(cap.results == 1 && cap.has_error && strcmp(cap.code, "cancelled") == 0);
    CHECK(cap.releases == 1);
}

static void test_command_names_are_validated(void) {
    t_start();
    bdk_error_t err = 0;
    CHECK(bdk_app_command_register(t_runtime, S("history.search"), S("{}"), echo_handler, &g_calls,
                                   NULL, &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "config", "nameNotApp"));
    CHECK(bdk_app_command_register(t_runtime, S("app.ok"), S("{bad"), echo_handler, &g_calls, NULL,
                                   &err) == BDK_ERR_INVALID_ARGUMENT);
    CHECK(bdk_app_command_register(t_runtime, S("app.ok"), S("{}"), NULL, &g_calls, NULL, &err) ==
          BDK_ERR_INVALID_ARGUMENT);
    CHECK(bdk_app_command_register(t_runtime, S("app.ok"), S("{}"), echo_handler, &g_calls, NULL,
                                   &err) == BDK_OK);
    CHECK(bdk_app_command_register(t_runtime, S("app.ok"), S("{}"), echo_handler, &g_calls, NULL,
                                   &err) == BDK_ERR_FAILED);
    bdk_error_release(err);
}

/* Application events */
static int g_event_count;
static char g_event_name[64], g_event_payload[128];
static int g_event_released;
static void on_app_event(void* ctx, uint32_t id, bdk_string_t name, bdk_string_t payload) {
    (void)ctx;
    (void)id;
    ++g_event_count;
    snprintf(g_event_name, sizeof g_event_name, "%.*s", (int)name.length, name.data);
    snprintf(g_event_payload, sizeof g_event_payload, "%.*s", (int)payload.length, payload.data);
}
static void on_app_event_release(void* ctx) {
    (void)ctx;
    ++g_event_released;
}

static void test_application_events_are_declared_emitted_and_delivered_in_order(void) {
    t_start();
    g_event_count = g_event_released = 0;
    bdk_error_t err = 0;
    REQUIRE(bdk_app_event_declare(t_runtime, S("app.changed"), S("{}"), &err) == BDK_OK);
    CHECK(bdk_app_event_declare(t_runtime, S("app.changed"), S("{}"), &err) == BDK_ERR_FAILED);
    bdk_error_release(err);
    t_freeze();
    bdk_subscription_t sub = bdk_subscribe_named(t_runtime, S("app.changed"), S(""), NULL,
                                                 on_app_event, NULL, on_app_event_release);
    REQUIRE(sub != 0);
    CHECK(bdk_app_event_emit(t_runtime, S("app.changed"), S("{\"n\":1}"), S(""), &err) == BDK_OK);
    CHECK(g_event_count == 0);
    CHECK(bdk_app_event_emit(t_runtime, S("app.changed"), S("{\"n\":2}"), S("{}"), &err) == BDK_OK);
    t_pump();
    CHECK(g_event_count == 2);
    CHECK(strcmp(g_event_name, "app.changed") == 0);
    CHECK(strcmp(g_event_payload, "{\"n\":2}") == 0); /* the later one arrived last */
    CHECK(bdk_app_event_emit(t_runtime, S("app.undeclared"), S("{}"), S(""), &err) ==
          BDK_ERR_FAILED);
    bdk_error_release(err);
    CHECK(bdk_app_event_emit(t_runtime, S("app.changed"), S("{bad"), S(""), &err) ==
          BDK_ERR_INVALID_ARGUMENT);
    bdk_unsubscribe(sub);
    CHECK(g_event_released == 0); /* the release callback is never synchronous */
    t_pump();
    CHECK(g_event_released == 1);
    CHECK(bdk_app_event_emit(t_runtime, S("app.changed"), S("{\"n\":3}"), S(""), &err) == BDK_OK);
    t_pump();
    CHECK(g_event_count == 2); /* nothing after unsubscribe */
    bdk_unsubscribe(sub);      /* a second unsubscribe is harmless */
    t_pump();
    CHECK(g_event_released == 1);
}

static void test_subscribe_by_id_rejects_the_zero_identifier(void) {
    t_start_frozen();
    CHECK(bdk_subscribe(t_runtime, 0, S(""), NULL, on_app_event, NULL, NULL) == 0);
    CHECK(bdk_subscribe(t_runtime, 5, S("{bad"), NULL, on_app_event, NULL, NULL) == 0);
    CHECK(bdk_subscribe(t_runtime, 5, S(""), NULL, on_app_event, NULL, NULL) != 0);
}

static void test_settings_handlers_and_providers_register_before_the_freeze(void) {
    t_start();
    bdk_error_t err = 0;
    CHECK(bdk_app_setting_register(t_runtime, S("{\"key\":\"app.theme\"}"), &err) == BDK_OK);
    CHECK(bdk_app_setting_register(t_runtime, S("[1]"), &err) == BDK_ERR_INVALID_ARGUMENT);
    CHECK(bdk_provider_set_key(t_runtime, S("translation"), S("key"), &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "config", "providerMissing"));
    bdk_provider_vtable_t vt;
    memset(&vt, 0, sizeof vt);
    vt.struct_size = sizeof vt;
    CHECK(bdk_provider_register(t_runtime, S("translation"), &vt, NULL, NULL, &err) == BDK_OK);
    CHECK(bdk_provider_register(t_runtime, S("translation"), &vt, NULL, NULL, &err) ==
          BDK_ERR_FAILED);
    bdk_error_release(err);
    CHECK(bdk_provider_set_key(t_runtime, S("translation"), S("key"), &err) == BDK_OK);
    CHECK(bdk_provider_register(t_runtime, S("geolocation"), NULL, NULL, NULL, &err) ==
          BDK_ERR_INVALID_ARGUMENT);
}

const t_test_t t_broker_tests[] = {
    {"values.json_crosses_verbatim", test_json_values_cross_verbatim},
    {"values.invalid_json_arguments_are_rejected",
     test_invalid_json_arguments_are_rejected_before_the_handler},
    {"broker.unknown_command_is_an_error", test_unknown_command_is_an_error_not_a_crash},
    {"broker.invoke_options_must_name_live_objects", test_invoke_options_must_name_live_objects},
    {"broker.progress_arrives_in_order", test_progress_arrives_before_the_result_in_order},
    {"broker.rejection_carries_domain_code_message",
     test_rejection_carries_domain_code_and_message},
    {"broker.resolving_invalid_json_fails_the_call",
     test_command_resolving_invalid_json_fails_the_call},
    {"broker.caller_json_is_json", test_caller_json_is_json},
    {"broker.handler_is_never_called_synchronously", test_handler_is_never_called_synchronously},
    {"broker.cancel_marks_context_and_ignores_late_answer",
     test_cancelling_a_command_marks_its_context_and_ignores_a_late_answer},
    {"broker.held_command_can_be_answered_later", test_a_held_command_can_be_answered_later},
    {"broker.shutdown_cancels_a_held_command", test_shutdown_cancels_a_held_command},
    {"broker.command_names_are_validated", test_command_names_are_validated},
    {"broker.application_events_are_delivered_in_order",
     test_application_events_are_declared_emitted_and_delivered_in_order},
    {"broker.subscribe_by_id_validates", test_subscribe_by_id_rejects_the_zero_identifier},
    {"broker.settings_and_providers_register_before_freeze",
     test_settings_handlers_and_providers_register_before_the_freeze},
    {NULL, NULL}};
