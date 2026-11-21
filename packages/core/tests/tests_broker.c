/* Broker: values, application commands, cancellation, events, registration of settings, handlers,
 * providers; the checks every caller goes through and the built-in commands. */
#include <stdlib.h>

#include "harness_broker.h"
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

/* The profile the last app.profile call addressed. */
static bdk_profile_t g_call_profile;
static void profile_handler(void* ctx, bdk_context_t call, bdk_string_t input) {
    (void)ctx;
    (void)input;
    g_call_profile = bdk_app_command_profile(call);
    bdk_app_command_resolve(call, S("{}"));
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
    REQUIRE(bdk_app_command_register(t_runtime, S("app.profile"), S("{}"), profile_handler,
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

/* ---- The broker's checks, with built-in commands (services.md sections 2 to 4) ----
   Calls from the host go through the ABI. Calls from an internal page or an automation session go
   through the harness's hooks (harness_broker.h), because their transports arrive in M4 and M7; a
   runtime without the hooks skips these tests. */

#define NEED_HOOKS()                                                                               \
    do {                                                                                           \
        if (!bdk_harness_broker_hooks()) {                                                         \
            printf("    (skipped: the runtime has no broker test hooks)\n");                       \
            return;                                                                                \
        }                                                                                          \
    } while (0)

static bdk_invoke_options_t options_for(bdk_profile_t profile, bdk_contents_t contents) {
    bdk_invoke_options_t o;
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.profile = profile;
    o.contents = contents;
    return o;
}

/* One call by the host, run to its end. */
static void host_call(const char* command, const char* args, bdk_profile_t profile,
                      t_capture_t* cap) {
    bdk_invoke_options_t o = options_for(profile, 0);
    t_cap_init(cap);
    REQUIRE(bdk_invoke_named(t_runtime, S(command), S(args), &o, t_on_result, t_on_progress, cap,
                             t_on_release) != 0);
    t_pump();
    CHECK(cap->results == 1 && cap->releases == 1);
}

static bdk_harness_caller_t make_caller(uint32_t kind, const char* name, const char* capabilities,
                                        bdk_profile_t profile) {
    bdk_harness_caller_spec_t spec;
    memset(&spec, 0, sizeof spec);
    spec.kind = kind;
    spec.name = name;
    spec.capabilities_json = capabilities;
    spec.profile = profile;
    spec.all_profiles = kind == BDK_HARNESS_CALLER_SESSION && profile == 0;
    spec.all_contents = kind == BDK_HARNESS_CALLER_SESSION;
    bdk_harness_caller_t who = 0;
    bdk_error_t err = 0;
    REQUIRE(bdk_harness_caller_create(t_runtime, &spec, &who, &err) == BDK_OK);
    return who;
}

static bdk_harness_caller_t page_in(bdk_profile_t profile, const char* capabilities) {
    return make_caller(BDK_HARNESS_CALLER_PAGE, "settings", capabilities, profile);
}

static bdk_harness_caller_t session_with(const char* capabilities) {
    return make_caller(BDK_HARNESS_CALLER_SESSION, "agent", capabilities, 0);
}

/* One call by a page or a session, run to its end. */
static void caller_call(bdk_harness_caller_t who, const char* command, const char* args,
                        bdk_profile_t profile, int gesture, t_capture_t* cap) {
    bdk_invoke_options_t o = options_for(profile, 0);
    t_cap_init(cap);
    REQUIRE(bdk_harness_invoke_as(t_runtime, who, command, args, profile ? &o : NULL, gesture,
                                  t_on_result, t_on_progress, cap, t_on_release) != 0);
    t_pump();
    CHECK(cap->results == 1 && cap->releases == 1);
}

static int failed_with(const t_capture_t* cap, const char* domain, const char* code) {
    return cap->has_error && strcmp(cap->domain, domain) == 0 && strcmp(cap->code, code) == 0;
}

static unsigned reached_services(void) {
    return bdk_harness_service_count(t_runtime, BDK_HARNESS_COUNT_EXECUTED);
}

static void test_builtin_commands_resolve_by_name_and_by_identifier(void) {
    NEED_HOOKS();
    t_start_frozen();
    t_capture_t by_name, by_id;
    t_cap_init(&by_name);
    t_cap_init(&by_id);
    REQUIRE(bdk_invoke_named(t_runtime, S("runtime.info"), S("{}"), NULL, t_on_result, NULL,
                             &by_name, t_on_release) != 0);
    REQUIRE(bdk_invoke(t_runtime, BDK_COMMAND_RUNTIME_INFO, S("{}"), NULL, t_on_result, NULL,
                       &by_id, t_on_release) != 0);
    t_pump();
    CHECK(by_name.results == 1 && !by_name.has_error);
    CHECK(by_id.results == 1 && !by_id.has_error);
    CHECK(strcmp(by_name.json, by_id.json) == 0);
    CHECK(strstr(by_name.json, "\"bdkVersion\"") != NULL);
}

static void test_an_application_command_sees_the_profile_its_call_addresses(void) {
    start_with_commands();
    bdk_profile_t p = t_new_profile("Addressed");
    t_capture_t cap;
    g_call_profile = 1;
    host_call("app.profile", "{}", 0, &cap);
    CHECK(!cap.has_error);
    CHECK(g_call_profile == 0); /* none named */
    host_call("app.profile", "{}", p, &cap);
    CHECK(!cap.has_error);
    CHECK(g_call_profile == p); /* the host's caller identity carries no profile; the call does */
}

static void test_finished_builtin_calls_are_freed(void) {
    NEED_HOOKS();
    t_start_frozen();
    for (int i = 0; i < 4; ++i) {
        t_capture_t cap;
        t_cap_init(&cap);
        REQUIRE(bdk_invoke(t_runtime, BDK_COMMAND_RUNTIME_INFO, S("{}"), NULL, t_on_result, NULL,
                           &cap, t_on_release) != 0);
        t_pump();
        CHECK(cap.results == 1 && !cap.has_error && cap.releases == 1);
    }
    /* Answered and released: nothing keeps a call, nor its service's control, alive. */
    CHECK(bdk_harness_service_count(t_runtime, BDK_HARNESS_COUNT_LIVE_CALLS) == 0);
}

static void test_unknown_builtin_names_and_identifiers_are_refused(void) {
    start_with_commands();
    t_capture_t cap;
    const char* names[] = {"runtime.inf0", "history", "history.", "app.nothing", "settings.get "};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i) {
        host_call(names[i], "{}", 0, &cap);
        CHECK(failed_with(&cap, "broker", "unknownCommand"));
    }
    t_cap_init(&cap);
    REQUIRE(bdk_invoke(t_runtime, 0, S("{}"), NULL, t_on_result, NULL, &cap, t_on_release) != 0);
    t_pump();
    CHECK(failed_with(&cap, "broker", "unknownCommand"));
}

static void test_host_has_full_authority(void) {
    NEED_HOOKS();
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Host");
    t_capture_t cap;
    host_call("settings.set", "{\"key\":\"appearance.fontSize\",\"value\":18}", p, &cap);
    CHECK(!cap.has_error && strcmp(cap.json, "null") == 0);
    host_call("settings.get", "{\"key\":\"appearance.fontSize\"}", p, &cap);
    CHECK(!cap.has_error && strcmp(cap.json, "{\"value\":18}") == 0);
}

static void test_a_caller_without_the_capability_is_refused_and_the_service_never_runs(void) {
    NEED_HOOKS();
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Denied");
    bdk_harness_caller_t page = page_in(p, "[\"history.read\"]");
    t_capture_t cap;
    unsigned before = reached_services();
    /* history.deleteAll needs history.write, and a gesture the page also lacks: the capability is
       checked first. */
    caller_call(page, "history.deleteAll", "{}", 0, 0, &cap);
    CHECK(failed_with(&cap, "capability", "denied"));
    caller_call(page, "settings.get", "{\"key\":\"a\"}", 0, 1, &cap);
    CHECK(failed_with(&cap, "capability", "denied"));
    CHECK(reached_services() == before);
    caller_call(page, "history.search", "{}", 0, 0, &cap);
    CHECK(!cap.has_error);
    CHECK(reached_services() == before + 1);
}

static void test_a_caller_without_the_capability_learns_nothing_about_its_arguments(void) {
    NEED_HOOKS();
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Blind");
    bdk_harness_caller_t page = page_in(p, "[]");
    t_capture_t valid, invalid, nonsense;
    caller_call(page, "settings.set", "{\"key\":\"a\",\"value\":1}", 0, 0, &valid);
    caller_call(page, "settings.set", "{\"key\":7}", 0, 0, &invalid);
    caller_call(page, "settings.set", "{not json", 0, 0, &nonsense);
    CHECK(failed_with(&valid, "capability", "denied"));
    CHECK(failed_with(&invalid, "capability", "denied"));
    CHECK(failed_with(&nonsense, "capability", "denied"));
    CHECK(strcmp(valid.message, invalid.message) == 0 &&
          strcmp(valid.message, nonsense.message) == 0);
}

static void test_a_caller_cannot_be_granted_what_the_policy_refuses(void) {
    NEED_HOOKS();
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Policy");
    struct {
        uint32_t kind;
        const char* capabilities;
        int allowed;
    } cases[] = {
        {BDK_HARNESS_CALLER_PAGE, "[\"cookies.values\"]", 0}, /* never for pages */
        {BDK_HARNESS_CALLER_PAGE, "[\"contents.read\"]", 0},  /* contents.* never for pages */
        {BDK_HARNESS_CALLER_PAGE, "[\"history.*\"]", 0},      /* no wildcard outside app. */
        {BDK_HARNESS_CALLER_PAGE, "[\"*\"]", 0},              /* there is no "all" capability */
        {BDK_HARNESS_CALLER_PAGE, "[\"app.*\"]", 0},          /* a prefix is required */
        {BDK_HARNESS_CALLER_PAGE, "[\"app.tabs.*\"]", 1},
        {BDK_HARNESS_CALLER_PAGE, "[\"passwords.reveal\"]", 1}, /* pages may, with authentication */
        {BDK_HARNESS_CALLER_SESSION, "[\"passwords.reveal\"]", 0}, /* never for sessions */
        {BDK_HARNESS_CALLER_SESSION, "[\"cookies.values\",\"contents.script\"]", 1}};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        bdk_harness_caller_spec_t spec;
        memset(&spec, 0, sizeof spec);
        spec.kind = cases[i].kind;
        spec.name = "x";
        spec.capabilities_json = cases[i].capabilities;
        spec.profile = p;
        spec.all_profiles = 1;
        bdk_harness_caller_t who = 0;
        bdk_error_t err = 0;
        bdk_status_t s = bdk_harness_caller_create(t_runtime, &spec, &who, &err);
        if (cases[i].allowed) {
            CHECK(s == BDK_OK);
        } else {
            CHECK(s == BDK_ERR_FAILED);
            CHECK(t_error_is(err, "capability", "denied"));
        }
    }
}

static void test_application_commands_need_their_own_capability_or_a_prefix_pattern(void) {
    NEED_HOOKS();
    t_start();
    g_calls = 0;
    bdk_error_t err = 0;
    const char* names[] = {"app.tabs.open", "app.tabs.close", "app.other"};
    for (int i = 0; i < 3; ++i) {
        REQUIRE(bdk_app_command_register(t_runtime, S(names[i]), S("{}"), echo_handler, &g_calls,
                                         NULL, &err) == BDK_OK);
    }
    REQUIRE(bdk_app_command_register(t_runtime, S("app.caller"), S("{}"), caller_handler, &g_calls,
                                     NULL, &err) == BDK_OK);
    t_freeze();
    bdk_profile_t p = t_new_profile("App");
    bdk_harness_caller_t page = page_in(p, "[\"app.tabs.open\"]");
    t_capture_t cap;
    caller_call(page, "app.tabs.open", "{\"a\":1}", 0, 0, &cap);
    CHECK(!cap.has_error && strcmp(cap.json, "{\"a\":1}") == 0);
    int before = g_calls;
    caller_call(page, "app.tabs.close", "{}", 0, 0, &cap);
    CHECK(failed_with(&cap, "capability", "denied"));
    CHECK(g_calls == before); /* the handler never ran */
    /* A prefix pattern covers what is under the prefix and nothing else. */
    bdk_harness_caller_t wide = session_with("[\"app.tabs.*\"]");
    caller_call(wide, "app.tabs.close", "{}", 0, 0, &cap);
    CHECK(!cap.has_error);
    caller_call(wide, "app.other", "{}", 0, 0, &cap);
    CHECK(failed_with(&cap, "capability", "denied"));
    /* The handler sees who called. */
    bdk_harness_caller_t named =
        make_caller(BDK_HARNESS_CALLER_SESSION, "tester", "[\"app.caller\"]", 0);
    caller_call(named, "app.caller", "{}", 0, 0, &cap);
    CHECK(strstr(cap.json, "\"caller\":\"session\"") != NULL);
    CHECK(strstr(cap.json, "\"name\":\"tester\"") != NULL);
}

static void test_a_page_is_confined_to_its_own_profile_and_no_page(void) {
    NEED_HOOKS();
    t_start_frozen();
    bdk_profile_t a = t_new_profile("Alpha"), b = t_new_profile("Beta");
    bdk_contents_t contents = t_new_contents(a);
    bdk_harness_caller_t page = page_in(a, "[\"profiles.read\",\"settings.read\"]");
    t_capture_t cap;
    caller_call(page, "profiles.current", "{}", 0, 0, &cap); /* implied by the caller */
    CHECK(!cap.has_error && strstr(cap.json, "\"name\":\"Alpha\"") != NULL);
    caller_call(page, "profiles.current", "{}", a, 0, &cap);
    CHECK(!cap.has_error && strstr(cap.json, "\"name\":\"Alpha\"") != NULL);
    caller_call(page, "profiles.current", "{}", b, 0, &cap); /* a page cannot name another */
    CHECK(failed_with(&cap, "capability", "outOfScope"));
    bdk_invoke_options_t o = options_for(0, contents);
    t_cap_init(&cap);
    REQUIRE(bdk_harness_invoke_as(t_runtime, page, "settings.get", "{\"key\":\"k\"}", &o, 0,
                                  t_on_result, NULL, &cap, t_on_release) != 0);
    t_pump();
    CHECK(failed_with(&cap, "capability", "outOfScope")); /* nor address a page */
}

static void test_a_session_is_confined_to_the_profiles_and_pages_in_its_scope(void) {
    NEED_HOOKS();
    t_start_frozen();
    bdk_profile_t a = t_new_profile("Alpha"), b = t_new_profile("Beta"), c = t_new_profile("Gamma");
    bdk_contents_t in_scope = t_new_contents(a), outside = t_new_contents(b);
    bdk_harness_caller_spec_t spec;
    memset(&spec, 0, sizeof spec);
    spec.kind = BDK_HARNESS_CALLER_SESSION;
    spec.name = "scoped";
    spec.capabilities_json = "[\"profiles.read\"]";
    bdk_profile_t only_a[] = {a};
    bdk_contents_t pages[] = {in_scope};
    spec.scope_profiles = only_a;
    spec.scope_profile_count = 1;
    spec.scope_contents = pages;
    spec.scope_contents_count = 1;
    bdk_harness_caller_t one = 0, two = 0;
    bdk_error_t err = 0;
    REQUIRE(bdk_harness_caller_create(t_runtime, &spec, &one, &err) == BDK_OK);
    bdk_profile_t two_profiles[] = {a, b};
    spec.scope_profiles = two_profiles;
    spec.scope_profile_count = 2;
    REQUIRE(bdk_harness_caller_create(t_runtime, &spec, &two, &err) == BDK_OK);

    t_capture_t cap;
    caller_call(one, "profiles.current", "{}", 0, 0,
                &cap); /* defaulted: the only profile in scope */
    CHECK(!cap.has_error && strstr(cap.json, "\"name\":\"Alpha\"") != NULL);
    caller_call(one, "profiles.current", "{}", b, 0, &cap);
    CHECK(failed_with(&cap, "capability", "outOfScope"));
    caller_call(one, "profiles.current", "{}", c, 0, &cap);
    CHECK(failed_with(&cap, "capability", "outOfScope"));
    caller_call(two, "profiles.current", "{}", b, 0, &cap);
    CHECK(!cap.has_error && strstr(cap.json, "\"name\":\"Beta\"") != NULL);
    caller_call(two, "profiles.current", "{}", 0, 0, &cap); /* ambiguous: it must say which */
    CHECK(failed_with(&cap, "broker", "invalidArguments"));
    bdk_invoke_options_t o = options_for(a, outside);
    t_cap_init(&cap);
    REQUIRE(bdk_harness_invoke_as(t_runtime, one, "profiles.current", "{}", &o, 0, t_on_result,
                                  NULL, &cap, t_on_release) != 0);
    t_pump();
    CHECK(failed_with(&cap, "capability", "outOfScope"));
    o = options_for(a, in_scope);
    t_cap_init(&cap);
    REQUIRE(bdk_harness_invoke_as(t_runtime, one, "profiles.current", "{}", &o, 0, t_on_result,
                                  NULL, &cap, t_on_release) != 0);
    t_pump();
    CHECK(!cap.has_error);
}

static void test_a_page_needs_a_gesture_for_a_command_that_asks_for_one(void) {
    NEED_HOOKS();
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Gesture");
    bdk_harness_caller_t page = page_in(p, "[\"history.write\"]");
    bdk_harness_caller_t session = session_with("[\"history.write\"]");
    t_capture_t cap;
    unsigned before = reached_services();
    caller_call(page, "history.deleteAll", "{}", 0, 0, &cap);
    CHECK(failed_with(&cap, "capability", "gestureRequired"));
    CHECK(reached_services() == before);
    caller_call(page, "history.deleteAll", "{}", 0, 1, &cap);
    CHECK(!cap.has_error);
    /* A session has no gesture: holding the capability is enough. The host never needs one. */
    caller_call(session, "history.deleteAll", "{}", p, 0, &cap);
    CHECK(!cap.has_error);
    host_call("history.deleteAll", "{}", p, &cap);
    CHECK(!cap.has_error);
    /* A command with no gesture requirement does not ask for one. */
    caller_call(page, "history.deleteUrls", "{\"urls\":[]}", 0, 0, &cap);
    CHECK(!cap.has_error);
}

static void test_authentication_is_asked_of_every_caller_before_the_service(void) {
    NEED_HOOKS();
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Secrets");
    t_capture_t cap;
    host_call("passwords.add",
              "{\"origin\":\"https://a.example\",\"username\":\"ann\",\"password\":\"hunter2\"}", p,
              &cap);
    REQUIRE(!cap.has_error);
    char id[32];
    REQUIRE(t_json_string(cap.json, "id", id, sizeof id));
    char args[64];
    snprintf(args, sizeof args, "{\"id\":\"%s\"}", id);

    unsigned before = reached_services();
    CHECK(bdk_harness_set_authentication(t_runtime, 0) == BDK_OK); /* the user cancels */
    host_call("passwords.reveal", args, p, &cap);
    CHECK(failed_with(&cap, "capability", "reauthenticationFailed"));
    CHECK(reached_services() == before);
    CHECK(bdk_harness_service_count(t_runtime, BDK_HARNESS_COUNT_AUTHENTICATIONS) == 1);

    CHECK(bdk_harness_set_authentication(t_runtime, 1) == BDK_OK);
    host_call("passwords.reveal", args, p, &cap);
    CHECK(!cap.has_error && strstr(cap.json, "hunter2") != NULL);
    CHECK(bdk_harness_service_count(t_runtime, BDK_HARNESS_COUNT_AUTHENTICATIONS) == 2);

    /* A page that holds the capability still authenticates. Arguments are checked after. */
    bdk_harness_caller_t page = page_in(p, "[\"passwords.reveal\"]");
    CHECK(bdk_harness_set_authentication(t_runtime, 0) == BDK_OK);
    caller_call(page, "passwords.reveal", args, 0, 1, &cap);
    CHECK(failed_with(&cap, "capability", "reauthenticationFailed"));
    CHECK(bdk_harness_set_authentication(t_runtime, 1) == BDK_OK);
    caller_call(page, "passwords.reveal", "{}", 0, 1, &cap);
    CHECK(failed_with(&cap, "broker", "invalidArguments"));
    /* A session can never hold the capability, so it never reaches the prompt. */
    bdk_harness_caller_t session = session_with("[\"passwords.read\"]");
    unsigned asked = bdk_harness_service_count(t_runtime, BDK_HARNESS_COUNT_AUTHENTICATIONS);
    caller_call(session, "passwords.reveal", args, p, 0, &cap);
    CHECK(failed_with(&cap, "capability", "denied"));
    CHECK(bdk_harness_service_count(t_runtime, BDK_HARNESS_COUNT_AUTHENTICATIONS) == asked);
}

static void test_authentication_is_asked_only_when_the_password_changes(void) {
    NEED_HOOKS();
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Update");
    t_capture_t cap;
    host_call("passwords.add",
              "{\"origin\":\"https://b.example\",\"username\":\"bob\",\"password\":\"old\"}", p,
              &cap);
    char id[32], args[96];
    REQUIRE(t_json_string(cap.json, "id", id, sizeof id));
    CHECK(bdk_harness_set_authentication(t_runtime, 0) == BDK_OK);
    snprintf(args, sizeof args, "{\"id\":\"%s\",\"username\":\"robert\"}", id);
    host_call("passwords.update", args, p, &cap);
    CHECK(!cap.has_error && strstr(cap.json, "robert") != NULL);
    CHECK(bdk_harness_service_count(t_runtime, BDK_HARNESS_COUNT_AUTHENTICATIONS) == 0);
    snprintf(args, sizeof args, "{\"id\":\"%s\",\"password\":\"new\"}", id);
    host_call("passwords.update", args, p, &cap);
    CHECK(failed_with(&cap, "capability", "reauthenticationFailed"));
    CHECK(bdk_harness_service_count(t_runtime, BDK_HARNESS_COUNT_AUTHENTICATIONS) == 1);
}

static void test_a_call_cancelled_while_the_user_is_asked_never_reaches_the_service(void) {
    NEED_HOOKS();
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Waiting");
    t_capture_t cap;
    host_call("passwords.add",
              "{\"origin\":\"https://c.example\",\"username\":\"c\",\"password\":\"x\"}", p, &cap);
    char id[32], args[64];
    REQUIRE(t_json_string(cap.json, "id", id, sizeof id));
    snprintf(args, sizeof args, "{\"id\":\"%s\"}", id);
    CHECK(bdk_harness_set_authentication(t_runtime, 2) == BDK_OK); /* the prompt stays up */
    bdk_invoke_options_t o = options_for(p, 0);
    t_cap_init(&cap);
    unsigned before = reached_services();
    bdk_call_t call = bdk_invoke_named(t_runtime, S("passwords.reveal"), S(args), &o, t_on_result,
                                       NULL, &cap, t_on_release);
    REQUIRE(call != 0);
    t_pump();
    CHECK(cap.results == 0);
    CHECK(bdk_call_cancel(call) == BDK_OK);
    t_pump();
    CHECK(cap.results == 1 && failed_with(&cap, "broker", "cancelled"));
    CHECK(bdk_harness_answer_authentication(t_runtime, 1) == BDK_OK); /* the user says yes, late */
    t_pump();
    CHECK(cap.results == 1 && cap.releases == 1);
    CHECK(reached_services() == before);
}

static void test_arguments_are_checked_against_the_definition_before_the_service(void) {
    NEED_HOOKS();
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Args");
    struct {
        const char* command;
        const char* args;
    } bad[] = {
        {"settings.set", "{\"value\":1}"},                           /* a field is missing */
        {"settings.set", "{\"key\":1,\"value\":1}"},                 /* wrong type */
        {"settings.set", "{\"key\":\"a\",\"value\":1,\"extra\":0}"}, /* unknown field */
        {"settings.set", "[]"},                                      /* not an object */
        {"settings.set", "{\"key\":null,\"value\":1}"},              /* null for a required field */
        {"settings.getMany", "{\"keys\":[\"a\",2]}"},                /* wrong element type */
        {"settings.getMany", "{\"keys\":\"a\"}"},                    /* not an array */
        {"history.search", "{\"limit\":0}"},                         /* pagination: at least 1 */
        {"history.search", "{\"limit\":1.5}"},                       /* an int is an integer */
        {"history.search", "{\"limit\":\"3\"}"},
        {"history.search", "{\"range\":{\"start\":\"x\"}}"}, /* nested type */
        {"history.search", "{\"range\":{\"begin\":1}}"},     /* unknown nested field */
        {"history.add", "{\"url\":\"u\",\"time\":1e3}"},     /* timestamp: integer */
        {"downloads.list", "{\"state\":\"bogus\"}"},         /* enum value */
        {"downloads.list", "{\"state\":3}"},
        {"permissions.set", "{\"origin\":\"https://x\",\"kind\":\"camera\",\"value\":\"maybe\"}"},
        {"settings.get", "{\"key\":\"a\"} trailing"}, /* not JSON */
        {"settings.get", ""}};
    unsigned before = reached_services();
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        t_capture_t cap;
        host_call(bad[i].command, bad[i].args, p, &cap);
        if (!failed_with(&cap, "broker", "invalidArguments"))
            printf("    not refused: %s %s\n", bad[i].command, bad[i].args);
        CHECK(failed_with(&cap, "broker", "invalidArguments"));
    }
    CHECK(reached_services() == before);
    t_capture_t cap;
    host_call("history.search", "{\"limit\":5,\"range\":{\"start\":0,\"end\":1700000000000}}", p,
              &cap);
    CHECK(!cap.has_error);
    host_call("settings.set", "{\"key\":\"a\",\"value\":{\"deep\":[1,{\"x\":null}]}}", p, &cap);
    CHECK(!cap.has_error); /* a json field takes any value */
}

static void test_a_command_that_needs_a_profile_says_so(void) {
    NEED_HOOKS();
    t_start_frozen();
    t_capture_t cap;
    host_call("history.search", "{}", 0, &cap);
    CHECK(failed_with(&cap, "broker", "invalidArguments"));
    host_call("runtime.info", "{}", 0, &cap); /* profile "none" */
    CHECK(!cap.has_error);
}

static void test_arguments_and_results_are_capped(void) {
    NEED_HOOKS();
    start_with_commands();
    size_t limit = 4u * 1024u * 1024u;
    char* big = (char*)malloc(limit + 64);
    REQUIRE(big != NULL);
    t_capture_t cap;
    /* Exactly at the limit passes; one byte more does not; for built-in and application commands.
     */
    const char head[] = "{\"key\":\"";
    const char tail[] = "\",\"value\":1}";
    bdk_profile_t p = t_new_profile("Big");
    size_t pad = limit - (sizeof head - 1) - (sizeof tail - 1);
    memcpy(big, head, sizeof head - 1);
    memset(big + sizeof head - 1, 'a', pad);
    memcpy(big + sizeof head - 1 + pad, tail, sizeof tail);
    host_call("settings.set", big, p, &cap);
    CHECK(!cap.has_error);
    memset(big + sizeof head - 1, 'a', pad + 1);
    memcpy(big + sizeof head - 1 + pad + 1, tail, sizeof tail);
    host_call("settings.set", big, p, &cap);
    CHECK(failed_with(&cap, "broker", "payloadTooLarge"));
    big[0] = '"';
    memset(big + 1, 'b', limit);
    big[limit + 1] = '"';
    big[limit + 2] = '\0';
    host_call("app.echo", big, 0, &cap);
    CHECK(failed_with(&cap, "broker", "payloadTooLarge"));
    free(big);
}

static void big_result_handler(void* ctx, bdk_context_t call, bdk_string_t input) {
    (void)ctx;
    (void)input;
    size_t n = 4u * 1024u * 1024u + 1u;
    char* out = (char*)malloc(n + 3);
    out[0] = '"';
    memset(out + 1, 'r', n);
    out[n + 1] = '"';
    out[n + 2] = '\0';
    bdk_app_command_resolve(call, S(out));
    free(out);
}

static void test_a_result_larger_than_the_cap_fails_the_call(void) {
    t_start();
    bdk_error_t err = 0;
    REQUIRE(bdk_app_command_register(t_runtime, S("app.big"), S("{}"), big_result_handler, NULL,
                                     NULL, &err) == BDK_OK);
    t_freeze();
    t_capture_t cap;
    host_call("app.big", "{}", 0, &cap);
    CHECK(failed_with(&cap, "broker", "payloadTooLarge"));
}

static void test_progress_and_result_arrive_in_order_for_a_builtin_command(void) {
    NEED_HOOKS();
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Progress");
    t_capture_t cap;
    host_call("privacy.clear", "{\"types\":[\"history\"]}", p, &cap);
    CHECK(!cap.has_error && strcmp(cap.json, "null") == 0);
    CHECK(cap.progress == 2 && cap.fraction == 0.5); /* the service reported 0.1, then 0.5 */
    /* A command that is not marked P reports no progress even if its service tries. */
    host_call("settings.get", "{\"key\":\"a\"}", p, &cap);
    CHECK(cap.progress == 0);
}

static void test_cancellation_tells_the_service_to_stop_and_ignores_its_late_answer(void) {
    NEED_HOOKS();
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Cancel");
    CHECK(bdk_harness_hold_services(t_runtime, 1) == BDK_OK);
    t_capture_t cap;
    t_cap_init(&cap);
    bdk_invoke_options_t o = options_for(p, 0);
    bdk_call_t call = bdk_invoke_named(t_runtime, S("privacy.clear"), S("{\"types\":[\"cache\"]}"),
                                       &o, t_on_result, NULL, &cap, t_on_release);
    REQUIRE(call != 0);
    t_pump();
    CHECK(cap.results == 0);
    CHECK(bdk_harness_service_count(t_runtime, BDK_HARNESS_COUNT_HELD) == 1);
    CHECK(bdk_harness_service_count(t_runtime, BDK_HARNESS_COUNT_STOPPED) == 0);
    CHECK(bdk_call_cancel(call) == BDK_OK);
    CHECK(cap.results == 0); /* never synchronously */
    t_pump();
    CHECK(cap.results == 1 && failed_with(&cap, "broker", "cancelled"));
    CHECK(cap.releases == 1);
    CHECK(bdk_harness_service_count(t_runtime, BDK_HARNESS_COUNT_STOPPED) == 1);
    CHECK(bdk_call_cancel(call) == BDK_ERR_STALE_HANDLE);
    CHECK(bdk_harness_release_held_commands(t_runtime) == BDK_OK); /* the service answers late */
    t_pump();
    CHECK(cap.results == 1 && cap.releases == 1);
}

static void test_a_call_cancelled_before_it_starts_never_reaches_the_service(void) {
    NEED_HOOKS();
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Early");
    t_capture_t cap;
    t_cap_init(&cap);
    bdk_invoke_options_t o = options_for(p, 0);
    unsigned before = reached_services();
    bdk_call_t call = bdk_invoke_named(t_runtime, S("settings.get"), S("{\"key\":\"a\"}"), &o,
                                       t_on_result, NULL, &cap, t_on_release);
    REQUIRE(call != 0);
    CHECK(bdk_call_cancel(call) == BDK_OK);
    t_pump();
    CHECK(cap.results == 1 && failed_with(&cap, "broker", "cancelled"));
    CHECK(reached_services() == before);
}

static void test_shutdown_tells_the_services_to_stop(void) {
    NEED_HOOKS();
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Shutdown");
    CHECK(bdk_harness_hold_services(t_runtime, 1) == BDK_OK);
    t_capture_t a, b;
    t_cap_init(&a);
    t_cap_init(&b);
    bdk_invoke_options_t o = options_for(p, 0);
    REQUIRE(bdk_invoke_named(t_runtime, S("privacy.clear"), S("{\"types\":[]}"), &o, t_on_result,
                             NULL, &a, t_on_release) != 0);
    REQUIRE(bdk_invoke_named(t_runtime, S("history.deleteAll"), S("{}"), &o, t_on_result, NULL, &b,
                             t_on_release) != 0);
    t_pump();
    CHECK(bdk_harness_service_count(t_runtime, BDK_HARNESS_COUNT_HELD) == 2);
    t_stop();
    CHECK(a.results == 1 && failed_with(&a, "broker", "cancelled") && a.releases == 1);
    CHECK(b.results == 1 && failed_with(&b, "broker", "cancelled") && b.releases == 1);
    CHECK(bdk_harness_service_count(t_runtime, BDK_HARNESS_COUNT_STOPPED) == 2);
}

static void test_revoking_a_caller_cancels_its_calls_and_refuses_later_ones(void) {
    NEED_HOOKS();
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Revoke");
    bdk_harness_caller_t page = page_in(p, "[\"history.write\",\"history.read\"]");
    bdk_harness_caller_t other = page_in(p, "[\"history.write\"]");
    CHECK(bdk_harness_hold_services(t_runtime, 1) == BDK_OK);
    t_capture_t a, b, c;
    t_cap_init(&a);
    t_cap_init(&b);
    t_cap_init(&c);
    REQUIRE(bdk_harness_invoke_as(t_runtime, page, "history.deleteAll", "{}", NULL, 1, t_on_result,
                                  NULL, &a, t_on_release) != 0);
    REQUIRE(bdk_harness_invoke_as(t_runtime, page, "history.search", "{}", NULL, 1, t_on_result,
                                  NULL, &b, t_on_release) != 0);
    REQUIRE(bdk_harness_invoke_as(t_runtime, other, "history.deleteAll", "{}", NULL, 1, t_on_result,
                                  NULL, &c, t_on_release) != 0);
    t_pump();
    CHECK(bdk_harness_service_count(t_runtime, BDK_HARNESS_COUNT_HELD) == 3);
    CHECK(bdk_harness_caller_revoke(t_runtime, page) == BDK_OK); /* the page navigated */
    CHECK(a.results == 0 && b.results == 0);                     /* never synchronously */
    t_pump();
    CHECK(a.results == 1 && failed_with(&a, "broker", "cancelled"));
    CHECK(b.results == 1 && failed_with(&b, "broker", "cancelled"));
    CHECK(c.results == 0); /* another caller's call is untouched */
    CHECK(bdk_harness_service_count(t_runtime, BDK_HARNESS_COUNT_STOPPED) == 2);
    t_capture_t later;
    caller_call(page, "history.search", "{}", 0, 1, &later);
    CHECK(failed_with(&later, "capability", "denied"));
    CHECK(bdk_harness_release_held_commands(t_runtime) == BDK_OK);
    t_pump();
    CHECK(a.results == 1 && b.results == 1 && c.results == 1 && !c.has_error);
}

static void test_calls_from_one_caller_start_in_the_order_sent(void) {
    NEED_HOOKS();
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Order");
    bdk_harness_caller_t page = page_in(p, "[\"settings.read\",\"settings.write\"]");
    t_capture_t caps[4];
    const char* commands[] = {"settings.set", "settings.set", "settings.set", "settings.get"};
    const char* args[] = {"{\"key\":\"n\",\"value\":1}", "{\"key\":\"n\",\"value\":2}",
                          "{\"key\":\"n\",\"value\":3}", "{\"key\":\"n\"}"};
    for (int i = 0; i < 4; ++i) {
        t_cap_init(&caps[i]);
        REQUIRE(bdk_harness_invoke_as(t_runtime, page, commands[i], args[i], NULL, 0, t_on_result,
                                      NULL, &caps[i], t_on_release) != 0);
    }
    t_pump();
    CHECK(strcmp(caps[3].json, "{\"value\":3}") == 0);
}

static void test_runtime_caller_tells_a_caller_what_it_is_and_needs_no_capability(void) {
    NEED_HOOKS();
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Who");
    t_capture_t cap;
    host_call("runtime.caller", "{}", 0, &cap);
    CHECK(!cap.has_error && strstr(cap.json, "\"kind\":\"swift\"") != NULL);
    bdk_harness_caller_t page = page_in(p, "[\"history.read\",\"app.tabs.*\"]");
    caller_call(page, "runtime.caller", "{}", 0, 0, &cap);
    CHECK(!cap.has_error);
    CHECK(strstr(cap.json, "\"kind\":\"page\"") != NULL);
    CHECK(strstr(cap.json, "\"name\":\"settings\"") != NULL);
    CHECK(strstr(cap.json, "\"profile\":\"") != NULL);
    CHECK(strstr(cap.json, "\"history.read\",\"app.tabs.*\"") != NULL);
    bdk_harness_caller_t session = session_with("[]");
    caller_call(session, "runtime.caller", "{}", 0, 0, &cap);
    CHECK(!cap.has_error && strstr(cap.json, "\"kind\":\"session\"") != NULL);
    CHECK(strstr(cap.json, "\"capabilities\":[]") != NULL);
}

static void test_a_command_the_runtime_does_not_implement_says_so(void) {
    NEED_HOOKS();
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Missing");
    t_capture_t cap;
    host_call("downloads.pause", "{\"id\":\"d1\"}", p, &cap);
    CHECK(failed_with(&cap, "broker", "notImplemented"));
}

static void test_settings_history_and_permissions_are_per_profile(void) {
    NEED_HOOKS();
    t_start_frozen();
    bdk_profile_t a = t_new_profile("One"), b = t_new_profile("Two");
    t_capture_t cap;
    host_call("settings.set", "{\"key\":\"k\",\"value\":\"in a\"}", a, &cap);
    host_call("settings.get", "{\"key\":\"k\"}", b, &cap);
    CHECK(strcmp(cap.json, "{\"value\":null}") == 0);
    host_call("settings.get", "{\"key\":\"k\"}", a, &cap);
    CHECK(strcmp(cap.json, "{\"value\":\"in a\"}") == 0);
    host_call("settings.getMany", "{\"keys\":[\"k\",\"z\"]}", a, &cap);
    CHECK(strcmp(cap.json, "{\"values\":{\"k\":\"in a\",\"z\":null}}") == 0);
    host_call("settings.reset", "{\"key\":\"k\"}", a, &cap);
    host_call("settings.get", "{\"key\":\"k\"}", a, &cap);
    CHECK(strcmp(cap.json, "{\"value\":null}") == 0);

    host_call("permissions.set",
              "{\"origin\":\"https://x.example\",\"kind\":\"camera\",\"value\":\"allow\"}", a,
              &cap);
    host_call("permissions.get", "{\"origin\":\"https://x.example\",\"kind\":\"camera\"}", a, &cap);
    CHECK(strstr(cap.json, "\"value\":\"allow\"") != NULL &&
          strstr(cap.json, "\"source\":\"user\"") != NULL);
    host_call("permissions.get", "{\"origin\":\"https://x.example\",\"kind\":\"camera\"}", b, &cap);
    CHECK(strstr(cap.json, "\"value\":\"ask\"") != NULL &&
          strstr(cap.json, "\"source\":\"default\"") != NULL);
    host_call("permissions.reset", "{\"origin\":\"https://x.example\"}", a, &cap);
    host_call("permissions.get", "{\"origin\":\"https://x.example\",\"kind\":\"camera\"}", a, &cap);
    CHECK(strstr(cap.json, "\"source\":\"default\"") != NULL);
}

static void test_history_is_paginated_with_an_opaque_cursor(void) {
    NEED_HOOKS();
    t_start_frozen();
    bdk_profile_t p = t_new_profile("History");
    t_capture_t cap;
    const char* urls[] = {"https://a.example/", "https://b.example/", "https://c.example/"};
    for (int i = 0; i < 3; ++i) {
        char args[96];
        snprintf(args, sizeof args, "{\"url\":\"%s\",\"title\":\"t%d\"}", urls[i], i);
        host_call("history.add", args, p, &cap);
        CHECK(!cap.has_error);
    }
    host_call("history.search", "{\"limit\":2}", p, &cap);
    CHECK(!cap.has_error && strstr(cap.json, "c.example") != NULL &&
          strstr(cap.json, "b.example") != NULL);
    CHECK(strstr(cap.json, "a.example") == NULL);
    char cursor[32], next[96];
    REQUIRE(t_json_string(cap.json, "nextCursor", cursor, sizeof cursor));
    snprintf(next, sizeof next, "{\"limit\":2,\"cursor\":\"%s\"}", cursor);
    host_call("history.search", next, p, &cap);
    CHECK(strstr(cap.json, "a.example") != NULL && strstr(cap.json, "\"nextCursor\":null") != NULL);
    host_call("history.search", "{\"cursor\":\"not-a-cursor\"}", p, &cap);
    CHECK(failed_with(&cap, "broker", "invalidArguments"));
    /* Digits only, but more than any cursor could hold: refused, not a crash. */
    host_call("history.search", "{\"cursor\":\"99999999999999999999999999999999\"}", p, &cap);
    CHECK(failed_with(&cap, "broker", "invalidArguments"));
    host_call("history.search", "{\"text\":\"b.example\"}", p, &cap);
    CHECK(strstr(cap.json, "b.example") != NULL && strstr(cap.json, "c.example") == NULL);
    host_call("history.deleteUrls", "{\"urls\":[\"https://b.example/\"]}", p, &cap);
    host_call("history.search", "{}", p, &cap);
    CHECK(strstr(cap.json, "b.example") == NULL && strstr(cap.json, "a.example") != NULL);
}

typedef struct {
    int count;
    char payload[256];
} event_log_t;

static void on_event(void* ctx, uint32_t id, bdk_string_t name, bdk_string_t payload) {
    event_log_t* log = (event_log_t*)ctx;
    (void)id;
    (void)name;
    ++log->count;
    snprintf(log->payload, sizeof log->payload, "%.*s", (int)payload.length, payload.data);
}

/* A built-in event reaches the subscriptions for its name and profile, with the payload the
   interface defines, and no others. */
static void test_a_built_in_event_reaches_the_subscriptions_of_its_profile(void) {
    t_start_frozen();
    bdk_profile_t a = t_new_profile("EventsA");
    bdk_profile_t b = t_new_profile("EventsB");
    event_log_t for_a, for_b, for_all;
    memset(&for_a, 0, sizeof for_a);
    memset(&for_b, 0, sizeof for_b);
    memset(&for_all, 0, sizeof for_all);
    bdk_invoke_options_t oa = options_for(a, 0), ob = options_for(b, 0), none = options_for(0, 0);
    REQUIRE(bdk_subscribe_named(t_runtime, S("history.visited"), S(""), &oa, on_event, &for_a,
                                NULL) != 0);
    REQUIRE(bdk_subscribe_named(t_runtime, S("history.visited"), S(""), &ob, on_event, &for_b,
                                NULL) != 0);
    REQUIRE(bdk_subscribe_named(t_runtime, S("history.visited"), S(""), &none, on_event, &for_all,
                                NULL) != 0);
    t_capture_t cap;
    host_call("history.add", "{\"url\":\"https://events.example/\",\"title\":\"E\"}", a, &cap);
    CHECK(!cap.has_error);
    for (int i = 0; i < 300 && for_all.count == 0; ++i)
        t_pump();
    CHECK(for_a.count == 1 && for_all.count == 1 && for_b.count == 0);
    CHECK(strstr(for_a.payload, "\"entry\"") != NULL &&
          strstr(for_a.payload, "events.example") != NULL &&
          strstr(for_a.payload, "\"source\"") != NULL);
}

/* Options of an older caller declare only struct_size: what lies past it is not the profile, so the
   subscription is for every profile. */
static void test_short_subscription_options_read_only_the_declared_bytes(void) {
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Short options");
    event_log_t log;
    memset(&log, 0, sizeof log);
    union {
        bdk_invoke_options_t o;
        unsigned char bytes[sizeof(bdk_invoke_options_t)];
    } u;
    memset(&u, 0xff, sizeof u);
    u.o.struct_size = sizeof(size_t);
    bdk_subscription_t sub =
        bdk_subscribe_named(t_runtime, S("history.visited"), S(""), &u.o, on_event, &log, NULL);
    REQUIRE(sub != 0);
    t_capture_t cap;
    host_call("history.add", "{\"url\":\"https://short.example/\",\"title\":\"S\"}", p, &cap);
    CHECK(!cap.has_error);
    for (int i = 0; i < 300 && log.count == 0; ++i)
        t_pump();
    CHECK(log.count == 1);
    bdk_unsubscribe(sub);
    t_pump();
}

static void test_bookmarks_nest_and_remove_with_their_contents(void) {
    NEED_HOOKS();
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Marks");
    t_capture_t cap;
    host_call("bookmarks.roots", "{}", p, &cap);
    CHECK(strstr(cap.json, "\"id\":\"bar\"") != NULL &&
          strstr(cap.json, "\"id\":\"other\"") != NULL);
    host_call("bookmarks.createFolder", "{\"parentId\":\"bar\",\"title\":\"News\"}", p, &cap);
    char folder[32], args[160], mark[32];
    REQUIRE(t_json_string(cap.json, "id", folder, sizeof folder));
    snprintf(args, sizeof args,
             "{\"parentId\":\"%s\",\"title\":\"Site\",\"url\":\"https://site.example/\"}", folder);
    host_call("bookmarks.create", args, p, &cap);
    REQUIRE(!cap.has_error);
    REQUIRE(t_json_string(cap.json, "id", mark, sizeof mark));
    host_call("bookmarks.forUrl", "{\"url\":\"https://site.example/\"}", p, &cap);
    CHECK(strstr(cap.json, mark) != NULL);
    snprintf(args, sizeof args, "{\"id\":\"%s\"}", folder);
    host_call("bookmarks.children", args, p, &cap);
    CHECK(strstr(cap.json, mark) != NULL);
    host_call("bookmarks.remove", args, p, &cap); /* a folder goes with its contents */
    CHECK(!cap.has_error);
    host_call("bookmarks.forUrl", "{\"url\":\"https://site.example/\"}", p, &cap);
    CHECK(strcmp(cap.json, "{\"items\":[]}") == 0);
    host_call("bookmarks.remove", "{\"id\":\"bar\"}", p, &cap); /* a permanent folder stays */
    CHECK(cap.has_error);
}

static void test_a_listing_never_carries_a_secret(void) {
    NEED_HOOKS();
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Listing");
    t_capture_t cap;
    host_call(
        "passwords.add",
        "{\"origin\":\"https://d.example\",\"username\":\"dee\",\"password\":\"s3cret-pass\"}", p,
        &cap);
    host_call("passwords.list", "{}", p, &cap);
    CHECK(strstr(cap.json, "dee") != NULL && strstr(cap.json, "s3cret") == NULL);
    host_call("autofill.addresses.add", "{\"fields\":{\"city\":\"Oslo\",\"country\":\"NO\"}}", p,
              &cap);
    CHECK(!cap.has_error && strstr(cap.json, "Oslo") != NULL);
    host_call("autofill.addresses.list", "{}", p, &cap);
    CHECK(strstr(cap.json, "Oslo") != NULL);
}

/* Application settings (T-SVC-08): registered before the freeze, answered by the core. */

static void register_setting(const char* declaration) {
    bdk_error_t err = 0;
    REQUIRE(bdk_app_setting_register(t_runtime, S(declaration), &err) == BDK_OK);
}

static void start_with_settings(void) {
    t_start();
    register_setting("{\"key\":\"app.sidebar.width\",\"default\":240,\"scope\":\"profile\","
                     "\"exposedToPages\":true}");
    register_setting("{\"key\":\"app.secret\",\"default\":\"x\",\"scope\":\"profile\","
                     "\"exposedToPages\":false}");
    register_setting("{\"key\":\"app.lastProfile\",\"default\":\"\",\"scope\":\"application\","
                     "\"exposedToPages\":true}");
    t_freeze();
}

static void test_a_number_too_large_for_a_double_is_refused_and_nothing_is_saved(void) {
    t_start();
    register_setting("{\"key\":\"app.layout\",\"default\":{\"panes\":[1]},\"scope\":\"profile\"}");
    register_setting("{\"key\":\"app.zoom\",\"default\":1,\"scope\":\"profile\"}");
    t_freeze();
    bdk_profile_t p = t_new_profile("Huge numbers");
    t_capture_t cap;
    host_call("settings.set", "{\"key\":\"app.zoom\",\"value\":1e999}", p, &cap);
    CHECK(failed_with(&cap, "broker", "invalidArguments"));
    host_call("settings.set", "{\"key\":\"app.layout\",\"value\":{\"panes\":[2,-1e999]}}", p, &cap);
    CHECK(failed_with(&cap, "broker", "invalidArguments"));
    host_call("settings.getMany", "{\"keys\":[\"app.zoom\",\"app.layout\"]}", p, &cap);
    CHECK(strcmp(cap.json, "{\"values\":{\"app.zoom\":1,\"app.layout\":{\"panes\":[1]}}}") == 0);
}

static void test_setting_registration_is_validated(void) {
    t_start();
    bdk_error_t err = 0;
    CHECK(bdk_app_setting_register(t_runtime, S("{\"key\":\"appearance.fontSize\",\"default\":1}"),
                                   &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "config", "nameNotApp"));
    CHECK(bdk_app_setting_register(t_runtime, S("{\"key\":\"app.\",\"default\":1}"), &err) ==
          BDK_ERR_FAILED);
    CHECK(t_error_is(err, "config", "nameNotApp"));
    CHECK(bdk_app_setting_register(t_runtime, S("{\"default\":1}"), &err) ==
          BDK_ERR_INVALID_ARGUMENT);
    CHECK(bdk_app_setting_register(t_runtime, S("{\"key\":\"app.a\",\"scope\":\"world\"}"), &err) ==
          BDK_ERR_INVALID_ARGUMENT);
    CHECK(bdk_app_setting_register(t_runtime, S("{\"key\":\"app.a\",\"exposedToPages\":1}"),
                                   &err) == BDK_ERR_INVALID_ARGUMENT);
    CHECK(bdk_app_setting_register(t_runtime, S("{\"key\":\"app.a\",\"default\":1}"), &err) ==
          BDK_OK);
    CHECK(bdk_app_setting_register(t_runtime, S("{\"key\":\"app.a\",\"default\":2}"), &err) ==
          BDK_ERR_FAILED);
    CHECK(t_error_is(err, "config", "duplicateName"));
    t_freeze();
    CHECK(bdk_app_setting_register(t_runtime, S("{\"key\":\"app.b\",\"default\":1}"), &err) ==
          BDK_ERR_FAILED);
    CHECK(t_error_is(err, "config", "registrationAfterStart"));
}

static void test_a_registered_setting_reads_its_default_and_validates_what_is_set(void) {
    start_with_settings();
    bdk_profile_t p = t_new_profile("Settings");
    t_capture_t cap;
    host_call("settings.get", "{\"key\":\"app.sidebar.width\"}", p, &cap);
    CHECK(!cap.has_error && strcmp(cap.json, "{\"value\":240}") == 0);
    host_call("settings.describe", "{\"key\":\"app.sidebar.width\"}", p, &cap);
    CHECK(!cap.has_error && strstr(cap.json, "\"type\":\"number\"") != NULL &&
          strstr(cap.json, "\"default\":240") != NULL &&
          strstr(cap.json, "\"source\":\"product\"") != NULL &&
          strstr(cap.json, "\"scope\":\"profile\"") != NULL);
    host_call("settings.set", "{\"key\":\"app.sidebar.width\",\"value\":300}", p, &cap);
    CHECK(!cap.has_error && strcmp(cap.json, "null") == 0);
    host_call("settings.get", "{\"key\":\"app.sidebar.width\"}", p, &cap);
    CHECK(strcmp(cap.json, "{\"value\":300}") == 0);
    host_call("settings.describe", "{\"key\":\"app.sidebar.width\"}", p, &cap);
    CHECK(strstr(cap.json, "\"source\":\"user\"") != NULL);
    host_call("settings.getMany", "{\"keys\":[\"app.sidebar.width\",\"app.nothing\"]}", p, &cap);
    CHECK(strcmp(cap.json, "{\"values\":{\"app.sidebar.width\":300,\"app.nothing\":null}}") == 0);
    host_call("settings.set", "{\"key\":\"app.sidebar.width\",\"value\":\"wide\"}", p, &cap);
    CHECK(failed_with(&cap, "broker", "invalidArguments"));
    host_call("settings.get", "{\"key\":\"app.sidebar.width\"}", p, &cap);
    CHECK(strcmp(cap.json, "{\"value\":300}") == 0); /* a refused value changes nothing */
    host_call("settings.reset", "{\"key\":\"app.sidebar.width\"}", p, &cap);
    CHECK(!cap.has_error);
    host_call("settings.get", "{\"key\":\"app.sidebar.width\"}", p, &cap);
    CHECK(strcmp(cap.json, "{\"value\":240}") == 0);
    /* An unregistered key reads as null and is refused for writing, like any unknown key. */
    host_call("settings.get", "{\"key\":\"app.unknown\"}", p, &cap);
    CHECK(strcmp(cap.json, "{\"value\":null}") == 0);
    host_call("settings.set", "{\"key\":\"app.unknown\",\"value\":1}", p, &cap);
    CHECK(failed_with(&cap, "broker", "invalidArguments"));
    host_call("settings.describe", "{\"key\":\"app.unknown\"}", p, &cap);
    CHECK(failed_with(&cap, "broker", "invalidArguments"));
    /* Application and browser settings do not mix in one getMany. */
    host_call("settings.getMany", "{\"keys\":[\"app.sidebar.width\",\"appearance.fontSize\"]}", p,
              &cap);
    CHECK(failed_with(&cap, "broker", "invalidArguments"));
}

static void test_profile_scope_needs_a_profile_and_application_scope_does_not(void) {
    start_with_settings();
    t_capture_t cap;
    host_call("settings.get", "{\"key\":\"app.sidebar.width\"}", 0, &cap);
    CHECK(failed_with(&cap, "broker", "invalidArguments"));
    host_call("settings.set", "{\"key\":\"app.lastProfile\",\"value\":\"work\"}", 0, &cap);
    CHECK(!cap.has_error);
    bdk_profile_t a = t_new_profile("One"), b = t_new_profile("Two");
    host_call("settings.get", "{\"key\":\"app.lastProfile\"}", a, &cap);
    CHECK(strcmp(cap.json, "{\"value\":\"work\"}") == 0);
    host_call("settings.get", "{\"key\":\"app.lastProfile\"}", b, &cap);
    CHECK(strcmp(cap.json, "{\"value\":\"work\"}") == 0); /* one value for the application */
    host_call("settings.set", "{\"key\":\"app.sidebar.width\",\"value\":10}", a, &cap);
    host_call("settings.get", "{\"key\":\"app.sidebar.width\"}", b, &cap);
    CHECK(strcmp(cap.json, "{\"value\":240}") == 0); /* a profile's value is its own */
}

static void test_a_profile_scoped_setting_survives_closing_and_reopening_the_profile(void) {
    start_with_settings();
    char id[64];
    t_create_profile("Keeper", id, sizeof id);
    bdk_profile_t p = t_open_profile(id);
    t_capture_t cap;
    host_call("settings.set", "{\"key\":\"app.sidebar.width\",\"value\":321}", p, &cap);
    CHECK(!cap.has_error);
    host_call("settings.set", "{\"key\":\"app.lastProfile\",\"value\":\"keeper\"}", p, &cap);
    CHECK(!cap.has_error);
    bdk_error_t err = 0;
    REQUIRE(bdk_profile_close(p, &err) == BDK_OK);
    t_pump();
    bdk_profile_t again = t_open_profile(id);
    host_call("settings.get", "{\"key\":\"app.sidebar.width\"}", again, &cap);
    CHECK(!cap.has_error && strcmp(cap.json, "{\"value\":321}") == 0);
    host_call("settings.describe", "{\"key\":\"app.sidebar.width\"}", again, &cap);
    CHECK(strstr(cap.json, "\"source\":\"user\"") != NULL);
    host_call("settings.get", "{\"key\":\"app.lastProfile\"}", again, &cap);
    CHECK(strcmp(cap.json, "{\"value\":\"keeper\"}") == 0);
    host_call("settings.reset", "{\"key\":\"app.sidebar.width\"}", again, &cap);
    host_call("settings.get", "{\"key\":\"app.sidebar.width\"}", again, &cap);
    CHECK(strcmp(cap.json, "{\"value\":240}") == 0);
}

static int g_setting_events;
static char g_setting_event[256];
static void on_setting_event(void* ctx, uint32_t id, bdk_string_t name, bdk_string_t payload) {
    (void)ctx;
    (void)id;
    (void)name;
    ++g_setting_events;
    snprintf(g_setting_event, sizeof g_setting_event, "%.*s", (int)payload.length, payload.data);
}

static void test_changing_an_application_setting_announces_settings_changed(void) {
    start_with_settings();
    bdk_profile_t p = t_new_profile("Events");
    g_setting_events = 0;
    bdk_subscription_t sub = bdk_subscribe_named(t_runtime, S("settings.changed"), S(""), NULL,
                                                 on_setting_event, NULL, NULL);
    REQUIRE(sub != 0);
    t_capture_t cap;
    host_call("settings.set", "{\"key\":\"app.sidebar.width\",\"value\":250}", p, &cap);
    t_pump();
    CHECK(g_setting_events == 1);
    CHECK(strstr(g_setting_event, "\"key\":\"app.sidebar.width\"") != NULL &&
          strstr(g_setting_event, "\"oldValue\":240") != NULL &&
          strstr(g_setting_event, "\"newValue\":250") != NULL &&
          strstr(g_setting_event, "\"source\":\"application\"") != NULL);
    host_call("settings.set", "{\"key\":\"app.sidebar.width\",\"value\":250}", p, &cap);
    t_pump();
    CHECK(g_setting_events == 1); /* no change, no event */
    bdk_unsubscribe(sub);
}

static void test_a_setting_not_exposed_to_pages_is_refused_to_every_page_and_session(void) {
    NEED_HOOKS();
    start_with_settings();
    bdk_profile_t p = t_new_profile("Exposure");
    /* Even a page that holds both capabilities. */
    bdk_harness_caller_t page = page_in(p, "[\"settings.read\",\"settings.write\"]");
    bdk_harness_caller_t session = session_with("[\"settings.read\",\"settings.write\"]");
    t_capture_t cap;
    unsigned before = reached_services();
    caller_call(page, "settings.get", "{\"key\":\"app.secret\"}", 0, 0, &cap);
    CHECK(failed_with(&cap, "capability", "denied"));
    caller_call(page, "settings.set", "{\"key\":\"app.secret\",\"value\":\"y\"}", 0, 0, &cap);
    CHECK(failed_with(&cap, "capability", "denied"));
    caller_call(page, "settings.describe", "{\"key\":\"app.secret\"}", 0, 0, &cap);
    CHECK(failed_with(&cap, "capability", "denied"));
    caller_call(page, "settings.reset", "{\"key\":\"app.secret\"}", 0, 0, &cap);
    CHECK(failed_with(&cap, "capability", "denied"));
    caller_call(page, "settings.getMany", "{\"keys\":[\"app.sidebar.width\",\"app.secret\"]}", 0, 0,
                &cap);
    CHECK(failed_with(&cap, "capability", "denied"));
    caller_call(session, "settings.get", "{\"key\":\"app.secret\"}", p, 0, &cap);
    CHECK(failed_with(&cap, "capability", "denied"));
    CHECK(reached_services() == before);
    /* The host reads it; an exposed key is open to a page with the capability. */
    host_call("settings.get", "{\"key\":\"app.secret\"}", p, &cap);
    CHECK(strcmp(cap.json, "{\"value\":\"x\"}") == 0);
    caller_call(page, "settings.set", "{\"key\":\"app.sidebar.width\",\"value\":260}", 0, 0, &cap);
    CHECK(!cap.has_error);
    caller_call(page, "settings.get", "{\"key\":\"app.sidebar.width\"}", 0, 0, &cap);
    CHECK(strcmp(cap.json, "{\"value\":260}") == 0);
    caller_call(session, "settings.get", "{\"key\":\"app.sidebar.width\"}", p, 0, &cap);
    CHECK(strcmp(cap.json, "{\"value\":260}") == 0);
    /* The capability is still needed. */
    bdk_harness_caller_t reader = page_in(p, "[\"settings.read\"]");
    caller_call(reader, "settings.set", "{\"key\":\"app.sidebar.width\",\"value\":1}", 0, 0, &cap);
    CHECK(failed_with(&cap, "capability", "denied"));
}

/* Search (T-SVC-10): the stub's engines, classification and suggestions. */

static void add_engine(bdk_profile_t p, const char* name, const char* keyword, const char* url,
                       char* id, size_t id_size) {
    char args[320];
    t_capture_t cap;
    snprintf(args, sizeof args, "{\"name\":\"%s\",\"keyword\":\"%s\",\"searchUrl\":\"%s\"}", name,
             keyword, url);
    host_call("search.addEngine", args, p, &cap);
    REQUIRE(!cap.has_error);
    REQUIRE(t_json_string(cap.json, "id", id, id_size));
}

static void test_search_engines_are_added_changed_and_removed_per_profile(void) {
    NEED_HOOKS();
    t_start_frozen();
    bdk_profile_t a = t_new_profile("SearchA"), b = t_new_profile("SearchB");
    t_capture_t cap;
    host_call("search.engines", "{}", a, &cap);
    CHECK(strcmp(cap.json, "{\"engines\":[]}") == 0); /* none is compiled in */
    char ddg[32], other[32], args[256];
    add_engine(a, "DuckDuckGo", "ddg", "https://ddg.example/?q={searchTerms}", ddg, sizeof ddg);
    host_call(
        "search.addEngine",
        "{\"name\":\"N\",\"keyword\":\"ddg\",\"searchUrl\":\"https://n.example/{searchTerms}\"}", a,
        &cap);
    CHECK(failed_with(&cap, "broker", "invalidArguments") && strstr(cap.message, "keyword"));
    host_call("search.addEngine",
              "{\"name\":\"\",\"keyword\":\"x\",\"searchUrl\":\"https://n.example/{searchTerms}\"}",
              a, &cap);
    CHECK(failed_with(&cap, "broker", "invalidArguments") && strstr(cap.message, "name"));
    host_call("search.addEngine",
              "{\"name\":\"N\",\"keyword\":\"\",\"searchUrl\":\"https://n.example/{searchTerms}\"}",
              a, &cap);
    CHECK(failed_with(&cap, "broker", "invalidArguments") && strstr(cap.message, "keyword"));
    host_call("search.addEngine",
              "{\"name\":\"N\",\"keyword\":\"n\",\"searchUrl\":\"https://n.example/\"}", a, &cap);
    CHECK(failed_with(&cap, "broker", "invalidArguments") && strstr(cap.message, "searchUrl"));
    host_call("search.addEngine",
              "{\"name\":\"N\",\"keyword\":\"n\",\"searchUrl\":\"ftp://n.example/{searchTerms}\"}",
              a, &cap);
    CHECK(failed_with(&cap, "broker", "invalidArguments") && strstr(cap.message, "searchUrl"));
    /* The keyword is unique within a profile only. */
    add_engine(b, "Other", "ddg", "http://o.example/?q={searchTerms}", other, sizeof other);
    add_engine(a, "Other", "oth", "http://o.example/?q={searchTerms}", other, sizeof other);
    host_call("search.engines", "{}", a, &cap);
    CHECK(strstr(cap.json, "\"source\":\"user\"") && strstr(cap.json, "\"isDefault\":false"));

    snprintf(args, sizeof args, "{\"id\":\"%s\",\"keyword\":\"ddg\"}", other);
    host_call("search.updateEngine", args, a, &cap);
    CHECK(failed_with(&cap, "broker", "invalidArguments") && strstr(cap.message, "keyword"));
    snprintf(args, sizeof args, "{\"id\":\"%s\",\"name\":\"Renamed\",\"keyword\":\"r\"}", other);
    host_call("search.updateEngine", args, a, &cap);
    CHECK(!cap.has_error && strstr(cap.json, "\"name\":\"Renamed\"") &&
          strstr(cap.json, "o.example"));
    host_call("search.updateEngine", "{\"id\":\"nope\",\"name\":\"x\"}", a, &cap);
    CHECK(failed_with(&cap, "broker", "invalidArguments"));
    host_call("search.removeEngine", "{\"id\":\"nope\"}", a, &cap);
    CHECK(failed_with(&cap, "broker", "invalidArguments"));

    snprintf(args, sizeof args, "{\"id\":\"%s\"}", ddg);
    host_call("search.setDefault", args, a, &cap);
    CHECK(!cap.has_error);
    host_call("search.removeEngine", args, a, &cap);
    CHECK(failed_with(&cap, "broker", "invalidArguments") && strstr(cap.message, "default"));
    snprintf(args, sizeof args, "{\"id\":\"%s\"}", other);
    host_call("search.setDefault", args, a, &cap);
    host_call("search.engines", "{}", a, &cap);
    CHECK(strstr(cap.json, "\"isDefault\":true") != NULL);
    snprintf(args, sizeof args, "{\"id\":\"%s\"}", ddg);
    host_call("search.removeEngine", args, a, &cap);
    CHECK(!cap.has_error);
    host_call("search.engines", "{}", a, &cap);
    CHECK(strstr(cap.json, "DuckDuckGo") == NULL && strstr(cap.json, "Renamed") != NULL);
    host_call("search.engines", "{}", b, &cap);
    CHECK(strstr(cap.json, "\"name\":\"Other\"") != NULL && strstr(cap.json, "Renamed") == NULL);
}

static void test_search_classify_tells_a_url_from_a_query(void) {
    NEED_HOOKS();
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Classify");
    t_capture_t cap;
    host_call("search.classify", "{\"text\":\"  \"}", p, &cap);
    CHECK(failed_with(&cap, "broker", "invalidArguments"));
    /* No engine: a query has no URL. */
    host_call("search.classify", "{\"text\":\"cats and dogs\"}", p, &cap);
    CHECK(strcmp(cap.json, "{\"kind\":\"search\",\"url\":null,\"engineId\":null}") == 0);
    struct {
        const char* text;
        const char* url;
    } urls[] = {{"https://x.example/a b", NULL},
                {"about:blank", "\"about:blank\""},
                {"conf://settings", "\"conf://settings\""},
                {"example.com", "\"https://example.com\""},
                {" example.com/path?q=1 ", "\"https://example.com/path?q=1\""},
                {"localhost", "\"http://localhost\""},
                {"localhost:3000/x", "\"http://localhost:3000/x\""},
                {"192.168.0.1:80", "\"https://192.168.0.1:80\""}};
    for (size_t i = 0; i < sizeof urls / sizeof urls[0]; ++i) {
        char args[96];
        snprintf(args, sizeof args, "{\"text\":\"%s\"}", urls[i].text);
        host_call("search.classify", args, p, &cap);
        if (!urls[i].url) { /* a space after the scheme: a query */
            CHECK(strstr(cap.json, "\"kind\":\"search\"") != NULL);
            continue;
        }
        CHECK(strstr(cap.json, "\"kind\":\"url\"") != NULL);
        CHECK(strstr(cap.json, urls[i].url) != NULL);
    }
    char ddg[32], args[96];
    add_engine(p, "DDG", "ddg", "https://ddg.example/?q={searchTerms}", ddg, sizeof ddg);
    /* A keyword engine is used without being the default. */
    host_call("search.classify", "{\"text\":\"ddg hello world&x\"}", p, &cap);
    CHECK(strstr(cap.json, "\"kind\":\"search\"") &&
          strstr(cap.json, "https://ddg.example/?q=hello%20world%26x") && strstr(cap.json, ddg));
    host_call("search.classify", "{\"text\":\"hello world\"}", p, &cap);
    CHECK(strstr(cap.json, "\"url\":null") != NULL); /* an engine, but no default */
    snprintf(args, sizeof args, "{\"id\":\"%s\"}", ddg);
    host_call("search.setDefault", args, p, &cap);
    host_call("search.classify", "{\"text\":\"hello world\"}", p, &cap);
    CHECK(strstr(cap.json, "\"kind\":\"search\"") &&
          strstr(cap.json, "https://ddg.example/?q=hello%20world") && strstr(cap.json, ddg));
    host_call("search.classify", "{\"text\":\"ddg\"}", p, &cap); /* a keyword alone is a query */
    CHECK(strstr(cap.json, "?q=ddg") != NULL);
    host_call("search.classify", "{\"text\":\"example.com\"}", p, &cap);
    CHECK(strstr(cap.json, "\"kind\":\"url\"") != NULL);
}

static void test_search_suggestions_come_from_history_and_bookmarks_only(void) {
    NEED_HOOKS();
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Suggest");
    t_capture_t cap;
    host_call("history.add", "{\"url\":\"https://www.cats.example/page\",\"title\":\"Cat page\"}",
              p, &cap);
    host_call("history.add", "{\"url\":\"https://dogs.example/\",\"title\":\"Dogs\"}", p, &cap);
    host_call(
        "bookmarks.create",
        "{\"parentId\":\"bar\",\"title\":\"Cats forever\",\"url\":\"https://cats2.example/\"}", p,
        &cap);
    host_call("search.suggestions", "{\"text\":\"cats\"}", p, &cap);
    CHECK(strstr(cap.json, "\"type\":\"history\"") && strstr(cap.json, "\"type\":\"bookmark\"") &&
          strstr(cap.json, "dogs.example") == NULL &&
          strstr(cap.json, "\"type\":\"search\"") == NULL);
    CHECK(strstr(cap.json, "\"inlineCompletion\":\".example/page\"") != NULL);
    CHECK(strstr(cap.json, "\"inlineCompletion\":\"2.example/\"") != NULL);
    /* Bookmarks rank above history. */
    CHECK(strstr(cap.json, "bookmark") < strstr(cap.json, "history"));
    host_call("search.suggestions", "{\"text\":\"CAT\",\"limit\":1}", p, &cap);
    CHECK(strstr(cap.json, "bookmark") && strstr(cap.json, "history") == NULL);
    host_call("search.suggestions", "{\"text\":\"cats\",\"limit\":0}", p, &cap);
    CHECK(failed_with(&cap, "broker", "invalidArguments"));
    host_call("search.suggestions", "{\"text\":\"\"}", p, &cap);
    CHECK(strcmp(cap.json, "{\"suggestions\":[]}") == 0);

    char ddg[32], args[96];
    add_engine(p, "DDG", "ddg", "https://ddg.example/?q={searchTerms}", ddg, sizeof ddg);
    snprintf(args, sizeof args, "{\"id\":\"%s\"}", ddg);
    host_call("search.setDefault", args, p, &cap);
    host_call("search.suggestions", "{\"text\":\"dogs.example\"}", p, &cap);
    CHECK(strstr(cap.json, "\"type\":\"url\"") && strstr(cap.json, "\"type\":\"history\"") &&
          strstr(cap.json, "\"type\":\"search\"") &&
          strstr(cap.json, "ddg.example/?q=dogs.example"));
    host_call("search.suggestions", "{\"text\":\"ddg cats\"}", p, &cap);
    CHECK(strstr(cap.json, "\"type\":\"keyword\"") != NULL);

    host_call("search.deleteSuggestion", "{\"url\":\"https://www.cats.example/page\"}", p, &cap);
    CHECK(!cap.has_error);
    host_call("history.search", "{\"text\":\"cats\"}", p, &cap);
    CHECK(strstr(cap.json, "cats.example") == NULL);
    host_call("search.suggestions", "{\"text\":\"cats\"}", p, &cap);
    CHECK(strstr(cap.json, "\"type\":\"history\"") == NULL && strstr(cap.json, "cats2") != NULL);
}

static void test_search_commands_need_their_capability_and_set_default_a_gesture(void) {
    NEED_HOOKS();
    t_start_frozen();
    bdk_profile_t p = t_new_profile("SearchCaps");
    t_capture_t cap;
    char ddg[32], args[96];
    add_engine(p, "DDG", "ddg", "https://ddg.example/?q={searchTerms}", ddg, sizeof ddg);
    snprintf(args, sizeof args, "{\"id\":\"%s\"}", ddg);
    bdk_harness_caller_t manager = page_in(p, "[\"search.manage\"]");
    bdk_harness_caller_t reader = page_in(p, "[\"search.read\"]");
    unsigned before = reached_services();
    caller_call(manager, "search.engines", "{}", 0, 1, &cap);
    CHECK(failed_with(&cap, "capability", "denied"));
    caller_call(manager, "search.classify", "{\"text\":\"a b\"}", 0, 1, &cap);
    CHECK(failed_with(&cap, "capability", "denied"));
    caller_call(
        reader, "search.addEngine",
        "{\"name\":\"N\",\"keyword\":\"n\",\"searchUrl\":\"https://n.example/{searchTerms}\"}", 0,
        1, &cap);
    CHECK(failed_with(&cap, "capability", "denied"));
    caller_call(reader, "search.deleteSuggestion", "{\"url\":\"https://x.example/\"}", 0, 1, &cap);
    CHECK(failed_with(&cap, "capability", "denied")); /* it needs history.write */
    CHECK(reached_services() == before);
    caller_call(manager, "search.setDefault", args, 0, 0, &cap);
    CHECK(failed_with(&cap, "capability", "gestureRequired"));
    CHECK(reached_services() == before);
    caller_call(manager, "search.setDefault", args, 0, 1, &cap);
    CHECK(!cap.has_error);
    caller_call(reader, "search.engines", "{}", 0, 0, &cap);
    CHECK(!cap.has_error && strstr(cap.json, "\"isDefault\":true") != NULL);
}

static void test_every_engine_change_is_announced_with_all_engines(void) {
    NEED_HOOKS();
    t_start_frozen();
    bdk_profile_t a = t_new_profile("SearchEventsA"), b = t_new_profile("SearchEventsB");
    event_log_t for_a, for_b;
    memset(&for_a, 0, sizeof for_a);
    memset(&for_b, 0, sizeof for_b);
    bdk_invoke_options_t oa = options_for(a, 0), ob = options_for(b, 0);
    REQUIRE(bdk_subscribe_named(t_runtime, S("search.enginesChanged"), S(""), &oa, on_event, &for_a,
                                NULL) != 0);
    REQUIRE(bdk_subscribe_named(t_runtime, S("search.enginesChanged"), S(""), &ob, on_event, &for_b,
                                NULL) != 0);
    char ddg[32], args[96];
    add_engine(a, "DDG", "ddg", "https://ddg.example/?q={searchTerms}", ddg, sizeof ddg);
    for (int i = 0; i < 300 && for_a.count == 0; ++i)
        t_pump();
    CHECK(for_a.count == 1 && for_b.count == 0);
    CHECK(strstr(for_a.payload, "\"engines\":[{") && strstr(for_a.payload, ddg) &&
          strstr(for_a.payload, "\"source\":\"application\""));
    t_capture_t cap;
    snprintf(args, sizeof args, "{\"id\":\"%s\"}", ddg);
    host_call("search.setDefault", args, a, &cap);
    for (int i = 0; i < 300 && for_a.count == 1; ++i)
        t_pump();
    CHECK(for_a.count == 2 && strstr(for_a.payload, "\"isDefault\":true") != NULL);
    /* A refused change announces nothing. */
    host_call("search.removeEngine", args, a, &cap);
    CHECK(cap.has_error);
    for (int i = 0; i < 20; ++i)
        t_pump();
    CHECK(for_a.count == 2);
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
    {"broker.short_subscription_options_read_only_their_bytes",
     test_short_subscription_options_read_only_the_declared_bytes},
    {"broker.subscribe_by_id_validates", test_subscribe_by_id_rejects_the_zero_identifier},
    {"broker.settings_and_providers_register_before_freeze",
     test_settings_handlers_and_providers_register_before_the_freeze},
    {"broker.builtin_commands_resolve_by_name_and_id",
     test_builtin_commands_resolve_by_name_and_by_identifier},
    {"broker.finished_builtin_calls_are_freed", test_finished_builtin_calls_are_freed},
    {"broker.application_command_sees_the_call_profile",
     test_an_application_command_sees_the_profile_its_call_addresses},
    {"broker.unknown_builtin_names_and_ids_are_refused",
     test_unknown_builtin_names_and_identifiers_are_refused},
    {"broker.host_has_full_authority", test_host_has_full_authority},
    {"broker.no_capability_is_refused_and_the_service_never_runs",
     test_a_caller_without_the_capability_is_refused_and_the_service_never_runs},
    {"broker.no_capability_learns_nothing_about_arguments",
     test_a_caller_without_the_capability_learns_nothing_about_its_arguments},
    {"broker.policy_limits_what_can_be_granted",
     test_a_caller_cannot_be_granted_what_the_policy_refuses},
    {"broker.application_commands_need_their_capability",
     test_application_commands_need_their_own_capability_or_a_prefix_pattern},
    {"broker.page_is_confined_to_its_profile",
     test_a_page_is_confined_to_its_own_profile_and_no_page},
    {"broker.session_is_confined_to_its_scope",
     test_a_session_is_confined_to_the_profiles_and_pages_in_its_scope},
    {"broker.page_needs_a_gesture", test_a_page_needs_a_gesture_for_a_command_that_asks_for_one},
    {"broker.authentication_before_the_service",
     test_authentication_is_asked_of_every_caller_before_the_service},
    {"broker.authentication_only_when_the_password_changes",
     test_authentication_is_asked_only_when_the_password_changes},
    {"broker.cancel_while_authenticating",
     test_a_call_cancelled_while_the_user_is_asked_never_reaches_the_service},
    {"broker.arguments_are_checked_before_the_service",
     test_arguments_are_checked_against_the_definition_before_the_service},
    {"broker.profile_is_required_when_the_command_says_so",
     test_a_command_that_needs_a_profile_says_so},
    {"broker.payloads_are_capped", test_arguments_and_results_are_capped},
    {"broker.result_cap_fails_the_call", test_a_result_larger_than_the_cap_fails_the_call},
    {"broker.builtin_progress_arrives_in_order",
     test_progress_and_result_arrive_in_order_for_a_builtin_command},
    {"broker.cancellation_stops_the_service",
     test_cancellation_tells_the_service_to_stop_and_ignores_its_late_answer},
    {"broker.cancel_before_start",
     test_a_call_cancelled_before_it_starts_never_reaches_the_service},
    {"broker.shutdown_stops_the_services", test_shutdown_tells_the_services_to_stop},
    {"broker.revoking_a_caller_cancels_its_calls",
     test_revoking_a_caller_cancels_its_calls_and_refuses_later_ones},
    {"broker.calls_start_in_order", test_calls_from_one_caller_start_in_the_order_sent},
    {"broker.runtime_caller",
     test_runtime_caller_tells_a_caller_what_it_is_and_needs_no_capability},
    {"broker.unimplemented_command_says_so", test_a_command_the_runtime_does_not_implement_says_so},
    {"services.settings_and_permissions_are_per_profile",
     test_settings_history_and_permissions_are_per_profile},
    {"services.a_built_in_event_reaches_its_profile",
     test_a_built_in_event_reaches_the_subscriptions_of_its_profile},
    {"services.history_is_paginated", test_history_is_paginated_with_an_opaque_cursor},
    {"services.bookmarks_nest", test_bookmarks_nest_and_remove_with_their_contents},
    {"services.listings_carry_no_secret", test_a_listing_never_carries_a_secret},
    {"search.engines_are_managed_per_profile",
     test_search_engines_are_added_changed_and_removed_per_profile},
    {"search.classify", test_search_classify_tells_a_url_from_a_query},
    {"search.suggestions", test_search_suggestions_come_from_history_and_bookmarks_only},
    {"search.capabilities_and_gesture",
     test_search_commands_need_their_capability_and_set_default_a_gesture},
    {"search.changes_are_announced", test_every_engine_change_is_announced_with_all_engines},
    {"settings.registration_is_validated", test_setting_registration_is_validated},
    {"settings.registered_setting_reads_default_and_validates",
     test_a_registered_setting_reads_its_default_and_validates_what_is_set},
    {"settings.a_number_too_large_for_a_double_is_refused",
     test_a_number_too_large_for_a_double_is_refused_and_nothing_is_saved},
    {"settings.scope_decides_whether_a_profile_is_needed",
     test_profile_scope_needs_a_profile_and_application_scope_does_not},
    {"settings.profile_scoped_setting_survives_reopening_the_profile",
     test_a_profile_scoped_setting_survives_closing_and_reopening_the_profile},
    {"settings.change_is_announced",
     test_changing_an_application_setting_announces_settings_changed},
    {"settings.not_exposed_is_refused_to_pages_and_sessions",
     test_a_setting_not_exposed_to_pages_is_refused_to_every_page_and_session},
    {NULL, NULL}};
