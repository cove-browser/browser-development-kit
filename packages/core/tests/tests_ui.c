/* UI requests. Rules from ui-requests.md sections 1 to 3 that the ABI can express: surface modes,
 * the handler's answers, queueing, gestures, completion once, withdrawal and what no configuration
 * or handler can override. */
#include "test_support.h"

static int g_handler_calls, g_handler_released, g_withdrawn, g_withdrawn_released;
static bdk_request_t g_request;
static uint32_t g_seen_surface;
static bdk_ui_response_t g_answer;
static char g_payload[96];

static void on_withdrawn_fwd(void* ctx);
static void on_withdrawn_rel_fwd(void* ctx);
static int g_install_withdrawn; /* the handler sets a withdrawn handler on every request */

static bdk_ui_response_t ui_handler(void* ctx, const bdk_ui_request_info_t* info) {
    (void)ctx;
    ++g_handler_calls;
    g_request = info->request;
    if (g_install_withdrawn)
        bdk_ui_set_withdrawn_handler(info->request, on_withdrawn_fwd, NULL, on_withdrawn_rel_fwd);
    g_seen_surface = info->surface;
    snprintf(g_payload, sizeof g_payload, "%.*s", (int)info->payload_json.length,
             info->payload_json.data);
    return g_answer;
}
static void ui_released(void* ctx) {
    (void)ctx;
    ++g_handler_released;
}
static void on_withdrawn(void* ctx) {
    (void)ctx;
    ++g_withdrawn;
}
static void on_withdrawn_released(void* ctx) {
    (void)ctx;
    ++g_withdrawn_released;
}

static bdk_contents_t setup(uint32_t surface, bdk_ui_response_t answer) {
    g_handler_calls = g_handler_released = g_withdrawn = g_withdrawn_released = 0;
    g_install_withdrawn = 0;
    g_request = 0;
    g_answer = answer;
    t_start();
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_set_handler(t_runtime, surface, ui_handler, NULL, ui_released, &err) == BDK_OK);
    t_freeze();
    return t_new_contents(t_new_profile("UI"));
}

static bdk_native_handle_t gesture(void) {
    bdk_native_handle_t g;
    memset(&g, 0, sizeof g);
    g.kind = BDK_NATIVE_NSEVENT;
    g.id = 7;
    return g;
}
static bdk_native_handle_t no_gesture(void) {
    bdk_native_handle_t g;
    memset(&g, 0, sizeof g);
    return g;
}

static void test_handler_receives_the_request_and_completes_it_once(void) {
    bdk_contents_t c = setup(BDK_SURFACE_PERMISSIONS, BDK_UI_HANDLED);
    REQUIRE(bdk_harness_raise_ui_request(t_runtime, BDK_SURFACE_PERMISSIONS, c,
                                         "{\"permission\":\"camera\"}") == BDK_OK);
    CHECK(g_handler_calls == 0); /* never synchronous */
    t_pump();
    REQUIRE(g_handler_calls == 1 && g_request != 0);
    CHECK(g_seen_surface == BDK_SURFACE_PERMISSIONS);
    CHECK(strcmp(g_payload, "{\"permission\":\"camera\"}") == 0);
    bdk_error_t err = 0;
    CHECK(bdk_ui_complete(g_request, S("deny"), S("{}"), no_gesture(), &err) == BDK_OK);
    bdk_harness_ui_outcome_t o;
    REQUIRE(bdk_harness_last_ui_outcome(t_runtime, &o) == BDK_OK);
    CHECK(o.kind == BDK_HARNESS_UI_COMPLETED && strcmp(o.completion, "deny") == 0);
    CHECK(bdk_ui_complete(g_request, S("deny"), S("{}"), no_gesture(), &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "ui", "requestCompleted"));
    bdk_ui_request_release(g_request);
    CHECK(bdk_ui_complete(g_request, S("deny"), S("{}"), no_gesture(), &err) ==
          BDK_ERR_STALE_HANDLE);
}

static void test_granting_needs_a_gesture(void) {
    bdk_contents_t c = setup(BDK_SURFACE_PERMISSIONS, BDK_UI_HANDLED);
    REQUIRE(bdk_harness_raise_ui_request(t_runtime, BDK_SURFACE_PERMISSIONS, c, "{}") == BDK_OK);
    t_pump();
    REQUIRE(g_request != 0);
    bdk_error_t err = 0;
    CHECK(bdk_ui_complete(g_request, S("grant"), S("{}"), no_gesture(), &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "ui", "gestureRequired"));
    bdk_harness_ui_outcome_t o;
    REQUIRE(bdk_harness_last_ui_outcome(t_runtime, &o) == BDK_OK);
    CHECK(o.kind == BDK_HARNESS_UI_NONE); /* a refused grant did not complete the request */
    CHECK(bdk_ui_complete(g_request, S("grant"), S("{\"scope\":\"once\"}"), gesture(), &err) ==
          BDK_OK);
    REQUIRE(bdk_harness_last_ui_outcome(t_runtime, &o) == BDK_OK);
    CHECK(o.kind == BDK_HARNESS_UI_COMPLETED && strcmp(o.completion, "grant") == 0);
    CHECK(strcmp(o.arguments_json, "{\"scope\":\"once\"}") == 0);
}

static void test_complete_validates_its_arguments(void) {
    bdk_contents_t c = setup(BDK_SURFACE_DOWNLOADS, BDK_UI_HANDLED);
    REQUIRE(bdk_harness_raise_ui_request(t_runtime, BDK_SURFACE_DOWNLOADS, c, "{}") == BDK_OK);
    t_pump();
    bdk_error_t err = 0;
    CHECK(bdk_ui_complete(g_request, S(""), S("{}"), no_gesture(), &err) ==
          BDK_ERR_INVALID_ARGUMENT);
    CHECK(bdk_ui_complete(g_request, S("deny"), S("{bad"), no_gesture(), &err) ==
          BDK_ERR_INVALID_ARGUMENT);
}

static void test_use_default_and_reject_responses(void) {
    bdk_contents_t c = setup(BDK_SURFACE_FIND, BDK_UI_USE_DEFAULT);
    bdk_harness_ui_outcome_t o;
    REQUIRE(bdk_harness_raise_ui_request(t_runtime, BDK_SURFACE_FIND, c, "{}") == BDK_OK);
    t_pump();
    REQUIRE(bdk_harness_last_ui_outcome(t_runtime, &o) == BDK_OK);
    CHECK(o.kind == BDK_HARNESS_UI_DEFAULTED);
    g_answer = BDK_UI_REJECT;
    REQUIRE(bdk_harness_raise_ui_request(t_runtime, BDK_SURFACE_FIND, c, "{}") == BDK_OK);
    t_pump();
    REQUIRE(bdk_harness_last_ui_outcome(t_runtime, &o) == BDK_OK);
    CHECK(o.kind == BDK_HARNESS_UI_REJECTED);
}

static void test_handled_request_can_use_the_default_later(void) {
    bdk_contents_t c = setup(BDK_SURFACE_FIND, BDK_UI_HANDLED);
    REQUIRE(bdk_harness_raise_ui_request(t_runtime, BDK_SURFACE_FIND, c, "{}") == BDK_OK);
    t_pump();
    bdk_error_t err = 0;
    CHECK(bdk_ui_use_default(g_request, &err) == BDK_OK);
    bdk_harness_ui_outcome_t o;
    REQUIRE(bdk_harness_last_ui_outcome(t_runtime, &o) == BDK_OK);
    CHECK(o.kind == BDK_HARNESS_UI_DEFAULTED);
    CHECK(bdk_ui_use_default(g_request, &err) == BDK_ERR_FAILED);
    bdk_error_release(err);
}

static void test_present_raises_a_find_request_the_handler_completes(void) {
    bdk_contents_t c = setup(BDK_SURFACE_FIND, BDK_UI_HANDLED);
    bdk_error_t err = 0;
    CHECK(bdk_contents_find_present(c, &err) == BDK_OK);
    CHECK(g_handler_calls == 0); /* never synchronous */
    t_pump();
    REQUIRE(g_handler_calls == 1);
    CHECK(g_seen_surface == BDK_SURFACE_FIND);
    /* One find UI is open per page: asking again raises nothing. */
    CHECK(bdk_contents_find_present(c, &err) == BDK_OK);
    t_pump();
    CHECK(g_handler_calls == 1);
    CHECK(bdk_ui_complete(g_request, S("accept"), S("{\"action\":\"close\"}"), no_gesture(),
                          &err) == BDK_OK);
    CHECK(bdk_contents_find_present(c, &err) == BDK_OK);
    t_pump();
    CHECK(g_handler_calls == 2);
}

static void test_print_raises_a_user_initiated_printing_request(void) {
    bdk_contents_t c = setup(BDK_SURFACE_PRINTING, BDK_UI_HANDLED);
    bdk_error_t err = 0;
    CHECK(bdk_contents_print(c, &err) == BDK_OK);
    CHECK(g_handler_calls == 0);
    t_pump();
    REQUIRE(g_handler_calls == 1);
    CHECK(g_seen_surface == BDK_SURFACE_PRINTING);
    CHECK(strstr(g_payload, "\"isUserInitiated\":true") != NULL);
    CHECK(bdk_ui_complete(g_request, S("deny"), S("{}"), no_gesture(), &err) == BDK_OK);
}

static void test_a_surface_with_no_handler_uses_the_default(void) {
    bdk_contents_t c = setup(BDK_SURFACE_FIND, BDK_UI_HANDLED);
    REQUIRE(bdk_harness_raise_ui_request(t_runtime, BDK_SURFACE_PRINTING, c, "{}") == BDK_OK);
    t_pump();
    CHECK(g_handler_calls == 0);
    bdk_harness_ui_outcome_t o;
    REQUIRE(bdk_harness_last_ui_outcome(t_runtime, &o) == BDK_OK);
    CHECK(o.kind == BDK_HARNESS_UI_DEFAULTED);
}

static void test_withdrawn_request_notifies_and_cannot_be_completed(void) {
    bdk_contents_t c = setup(BDK_SURFACE_JAVASCRIPT_DIALOGS, BDK_UI_HANDLED);
    REQUIRE(bdk_harness_raise_ui_request(t_runtime, BDK_SURFACE_JAVASCRIPT_DIALOGS, c, "{}") ==
            BDK_OK);
    t_pump();
    bdk_ui_set_withdrawn_handler(g_request, on_withdrawn, NULL, on_withdrawn_released);
    REQUIRE(bdk_harness_withdraw_ui_requests(t_runtime) == BDK_OK);
    CHECK(g_withdrawn == 0);
    t_pump();
    CHECK(g_withdrawn == 1 && g_withdrawn_released == 1);
    bdk_harness_ui_outcome_t o;
    REQUIRE(bdk_harness_last_ui_outcome(t_runtime, &o) == BDK_OK);
    CHECK(o.kind == BDK_HARNESS_UI_WITHDRAWN);
    bdk_error_t err = 0;
    CHECK(bdk_ui_complete(g_request, S("accept"), S("{}"), no_gesture(), &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "ui", "requestCompleted"));
}

static void test_releasing_an_uncompleted_request_cancels_it(void) {
    bdk_contents_t c = setup(BDK_SURFACE_HTTP_AUTH, BDK_UI_HANDLED);
    REQUIRE(bdk_harness_raise_ui_request(t_runtime, BDK_SURFACE_HTTP_AUTH, c, "{}") == BDK_OK);
    t_pump();
    bdk_ui_request_release(g_request);
    bdk_harness_ui_outcome_t o;
    REQUIRE(bdk_harness_last_ui_outcome(t_runtime, &o) == BDK_OK);
    CHECK(o.kind == BDK_HARNESS_UI_CANCELLED);
    bdk_ui_request_release(g_request); /* harmless */
}

static void test_handler_registration_rules(void) {
    t_start();
    bdk_error_t err = 0;
    g_handler_released = 0;
    CHECK(bdk_ui_set_handler(t_runtime, 0, ui_handler, NULL, NULL, &err) ==
          BDK_ERR_INVALID_ARGUMENT);
    CHECK(bdk_ui_set_handler(t_runtime, 9999, ui_handler, NULL, NULL, &err) ==
          BDK_ERR_INVALID_ARGUMENT);
    CHECK(bdk_ui_set_handler(t_runtime, BDK_SURFACE_FIND, NULL, NULL, NULL, &err) ==
          BDK_ERR_INVALID_ARGUMENT);
    CHECK(bdk_ui_set_handler(t_runtime, BDK_SURFACE_FIND, ui_handler, NULL, ui_released, &err) ==
          BDK_OK);
    CHECK(bdk_ui_set_handler(t_runtime, BDK_SURFACE_FIND, ui_handler, NULL, ui_released, &err) ==
          BDK_OK); /* replaces */
    t_pump();
    CHECK(g_handler_released == 1);
    t_stop();
    CHECK(g_handler_released == 2);
}

/* The withdrawn handler's release runs exactly once however the request ends; the withdrawn
 * callback itself runs only when the request is withdrawn. */
static void raise_with_withdrawn_handler(uint32_t surface, bdk_contents_t c) {
    g_install_withdrawn = 1;
    REQUIRE(bdk_harness_raise_ui_request(t_runtime, surface, c, "{}") == BDK_OK);
    t_pump();
    REQUIRE(g_handler_calls == 1);
}
static void expect_withdrawn_counts(int withdrawn, int released) {
    t_pump();
    t_pump();
    CHECK(g_withdrawn == withdrawn);
    CHECK(g_withdrawn_released == released);
}

static void test_withdrawn_release_when_the_handler_uses_the_default(void) {
    bdk_contents_t c = setup(BDK_SURFACE_FIND, BDK_UI_USE_DEFAULT);
    raise_with_withdrawn_handler(BDK_SURFACE_FIND, c);
    expect_withdrawn_counts(0, 1);
    REQUIRE(bdk_harness_withdraw_ui_requests(t_runtime) == BDK_OK); /* nothing left to withdraw */
    expect_withdrawn_counts(0, 1);
    t_stop();
    CHECK(g_withdrawn == 0 && g_withdrawn_released == 1);
}

static void test_withdrawn_release_when_the_handler_rejects(void) {
    bdk_contents_t c = setup(BDK_SURFACE_FIND, BDK_UI_REJECT);
    raise_with_withdrawn_handler(BDK_SURFACE_FIND, c);
    expect_withdrawn_counts(0, 1);
    t_stop();
    CHECK(g_withdrawn == 0 && g_withdrawn_released == 1);
}

static void test_withdrawn_release_when_completed(void) {
    bdk_contents_t c = setup(BDK_SURFACE_FIND, BDK_UI_HANDLED);
    raise_with_withdrawn_handler(BDK_SURFACE_FIND, c);
    expect_withdrawn_counts(0, 0); /* still pending */
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_complete(g_request, S("deny"), S("{}"), no_gesture(), &err) == BDK_OK);
    expect_withdrawn_counts(0, 1);
    bdk_ui_request_release(g_request);
    REQUIRE(bdk_harness_withdraw_ui_requests(t_runtime) == BDK_OK);
    t_stop();
    CHECK(g_withdrawn == 0 && g_withdrawn_released == 1);
}

static void test_withdrawn_release_when_the_default_is_used_later(void) {
    bdk_contents_t c = setup(BDK_SURFACE_FIND, BDK_UI_HANDLED);
    raise_with_withdrawn_handler(BDK_SURFACE_FIND, c);
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_use_default(g_request, &err) == BDK_OK);
    expect_withdrawn_counts(0, 1);
    t_stop();
    CHECK(g_withdrawn == 0 && g_withdrawn_released == 1);
}

static void test_withdrawn_callback_then_release_when_withdrawn(void) {
    bdk_contents_t c = setup(BDK_SURFACE_FIND, BDK_UI_HANDLED);
    raise_with_withdrawn_handler(BDK_SURFACE_FIND, c);
    REQUIRE(bdk_harness_withdraw_ui_requests(t_runtime) == BDK_OK);
    expect_withdrawn_counts(1, 1);
    REQUIRE(bdk_harness_withdraw_ui_requests(t_runtime) == BDK_OK);
    t_stop();
    CHECK(g_withdrawn == 1 && g_withdrawn_released == 1);
}

static void test_withdrawn_release_when_the_request_is_released(void) {
    bdk_contents_t c = setup(BDK_SURFACE_FIND, BDK_UI_HANDLED);
    raise_with_withdrawn_handler(BDK_SURFACE_FIND, c);
    bdk_ui_request_release(g_request);
    expect_withdrawn_counts(0, 1);
    t_stop();
    CHECK(g_withdrawn == 0 && g_withdrawn_released == 1);
}

static void test_withdrawn_when_the_contents_is_closed(void) {
    bdk_contents_t c = setup(BDK_SURFACE_FIND, BDK_UI_HANDLED);
    raise_with_withdrawn_handler(BDK_SURFACE_FIND, c);
    REQUIRE(bdk_contents_close(c, NULL) == BDK_OK);
    expect_withdrawn_counts(1, 1);
    REQUIRE(bdk_contents_destroy(c) == BDK_OK);
    t_stop();
    CHECK(g_withdrawn == 1 && g_withdrawn_released == 1);
}

static void test_withdrawn_when_the_contents_is_destroyed(void) {
    bdk_contents_t c = setup(BDK_SURFACE_FIND, BDK_UI_HANDLED);
    raise_with_withdrawn_handler(BDK_SURFACE_FIND, c);
    REQUIRE(bdk_contents_destroy(c) == BDK_OK);
    expect_withdrawn_counts(1, 1);
    t_stop();
    CHECK(g_withdrawn == 1 && g_withdrawn_released == 1);
}

static void test_withdrawn_release_at_runtime_shutdown(void) {
    bdk_contents_t c = setup(BDK_SURFACE_FIND, BDK_UI_HANDLED);
    raise_with_withdrawn_handler(BDK_SURFACE_FIND, c);
    expect_withdrawn_counts(0, 0);
    t_stop();
    CHECK(g_withdrawn == 0); /* shutdown is not a withdrawal */
    CHECK(g_withdrawn_released == 1);
    CHECK(g_handler_released == 1);
    t_pump();
    CHECK(g_withdrawn_released == 1);
}

static void test_replacing_the_withdrawn_handler_releases_the_old_one(void) {
    bdk_contents_t c = setup(BDK_SURFACE_FIND, BDK_UI_HANDLED);
    raise_with_withdrawn_handler(BDK_SURFACE_FIND, c);
    bdk_ui_set_withdrawn_handler(g_request, NULL, NULL, NULL); /* replaces: the first is released */
    expect_withdrawn_counts(0, 1);
    REQUIRE(bdk_harness_withdraw_ui_requests(t_runtime) == BDK_OK);
    expect_withdrawn_counts(0, 1);
    t_stop();
}

static void test_a_withdrawn_handler_set_too_late_is_released(void) {
    bdk_contents_t c = setup(BDK_SURFACE_FIND, BDK_UI_HANDLED);
    REQUIRE(bdk_harness_raise_ui_request(t_runtime, BDK_SURFACE_FIND, c, "{}") == BDK_OK);
    t_pump();
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_use_default(g_request, &err) == BDK_OK);
    bdk_ui_set_withdrawn_handler(g_request, on_withdrawn, NULL, on_withdrawn_released);
    expect_withdrawn_counts(0, 1); /* the request is over: never used, so released */
    bdk_ui_request_release(g_request);
    bdk_ui_set_withdrawn_handler(g_request, on_withdrawn, NULL, on_withdrawn_released); /* stale */
    expect_withdrawn_counts(0, 2);
    t_stop();
    CHECK(g_withdrawn == 0 && g_withdrawn_released == 2);
}

/* ---- Surface modes, routing and the rules of ui-requests.md section 3 ---------------------- */

static bdk_status_t start_with_surfaces(const char* surfaces, bdk_error_t* err) {
    /* The harness's own resolved configuration (a runtime may need its fields), plus surfaces. */
    bdk_start_config_t c;
    t_fill_start_config(&c);
    const bdk_string_t base = c.resolved_config_json;
    REQUIRE(base.length > 1 && base.data[base.length - 1] == '}');
    char resolved[2048];
    snprintf(resolved, sizeof resolved, "%.*s%s\"surfaces\":%s}", (int)(base.length - 1), base.data,
             base.length > 2 ? "," : "", surfaces);
    c.resolved_config_json = S(resolved);
    return bdk_runtime_start(&c, &t_runtime, err);
}

static void reset_counters(bdk_ui_response_t answer) {
    g_handler_calls = g_handler_released = g_withdrawn = g_withdrawn_released = 0;
    g_install_withdrawn = 0;
    g_request = 0;
    g_answer = answer;
}

/* Like setup, with the surfaces of the resolved configuration given. */
static bdk_contents_t setup_surfaces(const char* surfaces, uint32_t surface,
                                     bdk_ui_response_t answer) {
    reset_counters(answer);
    bdk_error_t err = 0;
    REQUIRE(start_with_surfaces(surfaces, &err) == BDK_OK);
    REQUIRE(bdk_ui_set_handler(t_runtime, surface, ui_handler, NULL, ui_released, &err) == BDK_OK);
    t_freeze();
    return t_new_contents(t_new_profile("UI"));
}

static bdk_harness_ui_outcome_t last_outcome(void) {
    bdk_harness_ui_outcome_t o;
    memset(&o, 0, sizeof o);
    REQUIRE(bdk_harness_last_ui_outcome(t_runtime, &o) == BDK_OK);
    return o;
}

static bdk_native_handle_t gesture_of_kind(uint32_t kind) {
    bdk_native_handle_t g = gesture();
    g.kind = kind;
    return g;
}

/* The modes ui-requests.md section 4 does not allow. */
static const char* const k_bad_modes[] = {
    "{\"permissions\":\"disabled\"}",
    "{\"javascript_dialogs\":\"disabled\"}",
    "{\"http_auth\":\"disabled\"}",
    "{\"client_certificates\":\"disabled\"}",
    "{\"new_windows\":\"disabled\"}",
    "{\"autofill_suggestions\":\"disabled\"}",
    "{\"find\":\"disabled\"}",
    "{\"exclusive_access\":\"disabled\"}",
    "{\"certificate_errors\":\"disabled\"}",
    "{\"reputation_warnings\":\"disabled\"}",
};

static void test_a_disallowed_surface_mode_fails_the_start(void) {
    for (size_t i = 0; i < sizeof k_bad_modes / sizeof k_bad_modes[0]; ++i) {
        bdk_error_t err = 0;
        CHECK(start_with_surfaces(k_bad_modes[i], &err) == BDK_ERR_FAILED);
        CHECK(t_error_is(err, "ui", "surfaceMode"));
        CHECK(t_runtime == 0);
    }
}

static void test_unknown_surfaces_and_modes_fail_the_start(void) {
    bdk_error_t err = 0;
    CHECK(start_with_surfaces("{\"toolbar\":\"default\"}", &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "ui", "surfaceUnknown"));
    CHECK(start_with_surfaces("{\"permissions\":\"auto\"}", &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "config", "resolvedInvalid"));
    CHECK(start_with_surfaces("{\"permissions\":true}", &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "config", "resolvedInvalid"));
    CHECK(start_with_surfaces("[]", &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "config", "resolvedInvalid"));
    CHECK(t_runtime == 0);
}

static void test_every_surface_name_is_accepted(void) {
    /* The resolved configuration names every surface of the schema; the S3 surfaces are known too.
     */
    const char* names[] = {"permissions",
                           "javascript_dialogs",
                           "http_auth",
                           "client_certificates",
                           "file_pickers",
                           "context_menus",
                           "new_windows",
                           "downloads",
                           "passwords",
                           "autofill",
                           "autofill_suggestions",
                           "printing",
                           "translation",
                           "find",
                           "fullscreen",
                           "exclusive_access",
                           "notifications",
                           "certificate_errors",
                           "reputation_warnings",
                           "devtools",
                           "device_choosers",
                           "screen_capture",
                           "webauthn",
                           "external_protocols"};
    char surfaces[1024] = "{";
    for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i) {
        size_t n = strlen(surfaces);
        snprintf(surfaces + n, sizeof surfaces - n, "%s\"%s\":\"default\"", i ? "," : "", names[i]);
    }
    strcat(surfaces, "}");
    bdk_error_t err = 0;
    REQUIRE(start_with_surfaces(surfaces, &err) == BDK_OK);
    t_freeze(); /* default needs no handler */
    t_stop();
    REQUIRE(start_with_surfaces("{\"permissions\":\"custom\",\"downloads\":\"disabled\","
                                "\"certificate_errors\":\"custom\"}",
                                &err) == BDK_OK);
}

static void test_a_custom_surface_needs_a_handler_before_the_freeze(void) {
    bdk_error_t err = 0;
    REQUIRE(start_with_surfaces("{\"permissions\":\"custom\",\"find\":\"custom\"}", &err) ==
            BDK_OK);
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_FIND, ui_handler, NULL, NULL, &err) ==
            BDK_OK);
    CHECK(bdk_runtime_freeze_registrations(t_runtime, &err) == BDK_ERR_FAILED);
    char message[160];
    bdk_string_t m = bdk_error_message(err);
    snprintf(message, sizeof message, "%.*s", (int)m.length, m.data);
    CHECK(t_error_is(err, "config", "handlerMissing"));
    CHECK(strstr(message, "permissions") != NULL && strstr(message, "find") == NULL);
    /* The failed freeze changed nothing: the handler can still be registered. */
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_PERMISSIONS, ui_handler, NULL, NULL, &err) ==
            BDK_OK);
    CHECK(bdk_runtime_freeze_registrations(t_runtime, &err) == BDK_OK);
}

static void test_a_disabled_surface_denies_without_calling_a_handler(void) {
    bdk_contents_t c =
        setup_surfaces("{\"downloads\":\"disabled\"}", BDK_SURFACE_DOWNLOADS, BDK_UI_HANDLED);
    REQUIRE(bdk_harness_raise_ui_request(t_runtime, BDK_SURFACE_DOWNLOADS, c, "{}") == BDK_OK);
    CHECK(last_outcome().kind == BDK_HARNESS_UI_NONE); /* never synchronous */
    t_pump();
    CHECK(g_handler_calls == 0);
    CHECK(last_outcome().kind == BDK_HARNESS_UI_DISABLED);
    /* Another surface is not affected. */
    REQUIRE(bdk_harness_raise_ui_request(t_runtime, BDK_SURFACE_PRINTING, c, "{}") == BDK_OK);
    t_pump();
    CHECK(last_outcome().kind == BDK_HARNESS_UI_DEFAULTED);
    t_stop();
    CHECK(g_handler_released == 1); /* the ignored handler is still released */
}

static void test_a_disabled_surface_does_not_queue(void) {
    bdk_contents_t c =
        setup_surfaces("{\"downloads\":\"disabled\"}", BDK_SURFACE_DOWNLOADS, BDK_UI_HANDLED);
    for (int i = 0; i < 3; ++i)
        REQUIRE(bdk_harness_raise_ui_request(t_runtime, BDK_SURFACE_DOWNLOADS, c, "{}") == BDK_OK);
    t_pump();
    CHECK(g_handler_calls == 0 && last_outcome().kind == BDK_HARNESS_UI_DISABLED);
}

static void test_a_custom_surface_calls_its_handler(void) {
    bdk_contents_t c =
        setup_surfaces("{\"find\":\"custom\"}", BDK_SURFACE_FIND, BDK_UI_USE_DEFAULT);
    REQUIRE(bdk_harness_raise_ui_request(t_runtime, BDK_SURFACE_FIND, c, "{}") == BDK_OK);
    t_pump();
    CHECK(g_handler_calls == 1);
    CHECK(last_outcome().kind == BDK_HARNESS_UI_DEFAULTED); /* use-default stays available */
}

static void test_reject_denies_and_never_grants(void) {
    bdk_contents_t c = setup(BDK_SURFACE_PERMISSIONS, BDK_UI_REJECT);
    REQUIRE(bdk_harness_raise_ui_request(t_runtime, BDK_SURFACE_PERMISSIONS, c,
                                         "{\"kinds\":[\"camera\"]}") == BDK_OK);
    t_pump();
    CHECK(g_handler_calls == 1);
    bdk_harness_ui_outcome_t o = last_outcome();
    CHECK(o.kind == BDK_HARNESS_UI_REJECTED && o.completion[0] == '\0');
    g_answer = (bdk_ui_response_t)99; /* a value that is not an answer reads as a rejection */
    REQUIRE(bdk_harness_raise_ui_request(t_runtime, BDK_SURFACE_PERMISSIONS, c, "{}") == BDK_OK);
    t_pump();
    CHECK(last_outcome().kind == BDK_HARNESS_UI_REJECTED);
}

/* An interstitial blocks the page until its request ends, and ending never means proceed. */
static void test_rejecting_an_interstitial_goes_back(void) {
    bdk_contents_t c = setup(BDK_SURFACE_CERTIFICATE_ERRORS, BDK_UI_REJECT);
    REQUIRE(bdk_harness_raise_ui_request(t_runtime, BDK_SURFACE_CERTIFICATE_ERRORS, c,
                                         "{\"isOverridable\":true}") == BDK_OK);
    t_pump();
    bdk_harness_ui_outcome_t o = last_outcome();
    CHECK(o.kind == BDK_HARNESS_UI_COMPLETED && strcmp(o.completion, "go_back") == 0);
}

static void test_dropping_an_interstitial_goes_back(void) {
    bdk_contents_t c = setup(BDK_SURFACE_REPUTATION_WARNINGS, BDK_UI_HANDLED);
    REQUIRE(bdk_harness_raise_ui_request(t_runtime, BDK_SURFACE_REPUTATION_WARNINGS, c,
                                         "{\"isOverridable\":true}") == BDK_OK);
    t_pump();
    bdk_ui_request_release(g_request);
    bdk_harness_ui_outcome_t o = last_outcome();
    CHECK(o.kind == BDK_HARNESS_UI_COMPLETED && strcmp(o.completion, "go_back") == 0);
}

static void test_an_interstitial_that_is_withdrawn_stays_withdrawn(void) {
    bdk_contents_t c = setup(BDK_SURFACE_CERTIFICATE_ERRORS, BDK_UI_HANDLED);
    REQUIRE(bdk_harness_raise_ui_request(t_runtime, BDK_SURFACE_CERTIFICATE_ERRORS, c, "{}") ==
            BDK_OK);
    t_pump();
    REQUIRE(bdk_harness_withdraw_ui_requests(t_runtime) == BDK_OK);
    CHECK(last_outcome().kind == BDK_HARNESS_UI_WITHDRAWN);
}

static void test_an_interstitial_can_use_the_default_presenter(void) {
    bdk_contents_t c = setup(BDK_SURFACE_CERTIFICATE_ERRORS, BDK_UI_USE_DEFAULT);
    REQUIRE(bdk_harness_raise_ui_request(t_runtime, BDK_SURFACE_CERTIFICATE_ERRORS, c, "{}") ==
            BDK_OK);
    t_pump();
    CHECK(last_outcome().kind == BDK_HARNESS_UI_DEFAULTED); /* Chromium's own page, in the page */
}

static void test_an_interstitial_is_completed_by_go_back_or_proceed_only(void) {
    bdk_contents_t c = setup(BDK_SURFACE_REPUTATION_WARNINGS, BDK_UI_HANDLED);
    REQUIRE(bdk_harness_raise_ui_request(t_runtime, BDK_SURFACE_REPUTATION_WARNINGS, c,
                                         "{\"isOverridable\":true}") == BDK_OK);
    t_pump();
    const char* other[] = {"deny", "accept", "grant", "dismiss", "cancel", "allow"};
    for (size_t i = 0; i < sizeof other / sizeof other[0]; ++i) {
        bdk_error_t err = 0;
        CHECK(bdk_ui_complete(g_request, S(other[i]), S("{}"), gesture(), &err) == BDK_ERR_FAILED);
        CHECK(t_error_is(err, "ui", "completionNotAllowed"));
    }
    CHECK(last_outcome().kind == BDK_HARNESS_UI_NONE);
    bdk_error_t err = 0;
    CHECK(bdk_ui_complete(g_request, S("go_back"), S("{}"), no_gesture(), &err) == BDK_OK);
    CHECK(strcmp(last_outcome().completion, "go_back") == 0);
}

static void test_proceeding_needs_a_gesture_and_an_overridable_request(void) {
    bdk_contents_t c = setup(BDK_SURFACE_CERTIFICATE_ERRORS, BDK_UI_HANDLED);
    const char* payloads[] = {"{\"isOverridable\":false}", "{}", "{\"isOverridable\":\"true\"}",
                              "not json"};
    for (size_t i = 0; i < sizeof payloads / sizeof payloads[0]; ++i) {
        REQUIRE(bdk_harness_raise_ui_request(t_runtime, BDK_SURFACE_CERTIFICATE_ERRORS, c,
                                             payloads[i]) == BDK_OK);
        t_pump();
        bdk_error_t err = 0;
        CHECK(bdk_ui_complete(g_request, S("proceed"), S("{}"), no_gesture(), &err) ==
              BDK_ERR_FAILED);
        CHECK(t_error_is(err, "ui", "gestureRequired"));
        CHECK(bdk_ui_complete(g_request, S("proceed"), S("{}"), gesture(), &err) == BDK_ERR_FAILED);
        CHECK(t_error_is(err, "ui", "notOverridable")); /* a request that does not say is refused */
        CHECK(last_outcome().kind == BDK_HARNESS_UI_NONE);
        bdk_ui_request_release(g_request); /* dropped: goes back */
        CHECK(strcmp(last_outcome().completion, "go_back") == 0);
    }
    REQUIRE(bdk_harness_raise_ui_request(t_runtime, BDK_SURFACE_CERTIFICATE_ERRORS, c,
                                         "{\"isOverridable\":true}") == BDK_OK);
    t_pump();
    bdk_error_t err = 0;
    CHECK(bdk_ui_complete(g_request, S("proceed"), S("{}"), gesture(), &err) == BDK_OK);
    bdk_harness_ui_outcome_t o = last_outcome();
    CHECK(o.kind == BDK_HARNESS_UI_COMPLETED && strcmp(o.completion, "proceed") == 0);
}

/* Each surface's granting completion needs the gesture; its other completions do not. */
static const struct {
    uint32_t surface;
    const char* grants;
    const char* harmless;
} k_grant_rows[] = {
    {BDK_SURFACE_PERMISSIONS, "grant", "deny"},
    {BDK_SURFACE_DOWNLOADS, "keep", "proceed"},
    {BDK_SURFACE_PASSWORDS, "save", "never"},
    {BDK_SURFACE_AUTOFILL, "save", "dismiss"},
    {BDK_SURFACE_AUTOFILL_SUGGESTIONS, "accept", "preview"},
    {BDK_SURFACE_DEVICE_CHOOSERS, "select", "cancel"},
    {BDK_SURFACE_SCREEN_CAPTURE, "share", "cancel"},
    {BDK_SURFACE_EXTERNAL_PROTOCOLS, "open", "cancel"},
    {BDK_SURFACE_JAVASCRIPT_DIALOGS, "grant", "accept"}, /* "grant" grants on every surface */
    {BDK_SURFACE_NEW_WINDOWS, "grant", "accept"},
};

static void test_granting_completions_need_a_gesture_on_every_surface(void) {
    reset_counters(BDK_UI_HANDLED);
    t_start();
    bdk_error_t err = 0;
    for (size_t i = 0; i < sizeof k_grant_rows / sizeof k_grant_rows[0]; ++i)
        REQUIRE(bdk_ui_set_handler(t_runtime, k_grant_rows[i].surface, ui_handler, NULL, NULL,
                                   &err) == BDK_OK);
    t_freeze();
    bdk_contents_t c = t_new_contents(t_new_profile("UI"));
    for (size_t i = 0; i < sizeof k_grant_rows / sizeof k_grant_rows[0]; ++i) {
        uint32_t surface = k_grant_rows[i].surface;
        REQUIRE(bdk_harness_raise_ui_request(t_runtime, surface, c, "{}") == BDK_OK);
        t_pump();
        REQUIRE(g_request != 0);
        CHECK(bdk_ui_complete(g_request, S(k_grant_rows[i].grants), S("{}"), no_gesture(), &err) ==
              BDK_ERR_FAILED);
        CHECK(t_error_is(err, "ui", "gestureRequired"));
        /* An event of another kind is not a user gesture either. */
        CHECK(bdk_ui_complete(g_request, S(k_grant_rows[i].grants), S("{}"),
                              gesture_of_kind(BDK_NATIVE_NSVIEW), &err) == BDK_ERR_FAILED);
        CHECK(t_error_is(err, "ui", "gestureRequired"));
        CHECK(last_outcome().kind == BDK_HARNESS_UI_NONE);
        CHECK(bdk_ui_complete(g_request, S(k_grant_rows[i].harmless), S("{}"), no_gesture(),
                              &err) == BDK_OK);
        CHECK(strcmp(last_outcome().completion, k_grant_rows[i].harmless) == 0);
        /* With the gesture, a fresh request is granted. */
        REQUIRE(bdk_harness_raise_ui_request(t_runtime, surface, c, "{}") == BDK_OK);
        t_pump();
        CHECK(bdk_ui_complete(g_request, S(k_grant_rows[i].grants), S("{}"), gesture(), &err) ==
              BDK_OK);
        CHECK(strcmp(last_outcome().completion, k_grant_rows[i].grants) == 0);
    }
}

static bdk_status_t complete_grant(const char* arguments, bdk_error_t* err) {
    return bdk_ui_complete(g_request, S("grant"), S(arguments), gesture(), err);
}

static void test_a_permission_grant_cannot_widen_the_request(void) {
    bdk_contents_t c = setup(BDK_SURFACE_PERMISSIONS, BDK_UI_HANDLED);
    REQUIRE(bdk_harness_raise_ui_request(
                t_runtime, BDK_SURFACE_PERMISSIONS, c,
                "{\"origin\":\"https://a.test\",\"kinds\":[\"camera\",\"microphone\"]}") == BDK_OK);
    t_pump();
    bdk_error_t err = 0;
    CHECK(complete_grant("{\"kinds\":[\"camera\",\"geolocation\"]}", &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "ui", "widensRequest"));
    CHECK(complete_grant("{\"kinds\":[\"geolocation\"]}", &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "ui", "widensRequest"));
    CHECK(complete_grant("{\"kinds\":\"camera\"}", &err) == BDK_ERR_INVALID_ARGUMENT);
    CHECK(complete_grant("{\"kinds\":[1]}", &err) == BDK_ERR_INVALID_ARGUMENT);
    CHECK(complete_grant("[\"camera\"]", &err) == BDK_ERR_INVALID_ARGUMENT);
    CHECK(last_outcome().kind == BDK_HARNESS_UI_NONE); /* the refusals left it open */
    CHECK(complete_grant("{\"kinds\":[\"microphone\"],\"duration\":\"thisVisit\"}", &err) ==
          BDK_OK);
    CHECK(last_outcome().kind == BDK_HARNESS_UI_COMPLETED);

    /* Everything asked, with or without naming it. */
    REQUIRE(bdk_harness_raise_ui_request(t_runtime, BDK_SURFACE_PERMISSIONS, c,
                                         "{\"kinds\":[\"camera\"]}") == BDK_OK);
    t_pump();
    CHECK(complete_grant("{\"kinds\":[\"camera\"]}", &err) == BDK_OK);
    REQUIRE(bdk_harness_raise_ui_request(t_runtime, BDK_SURFACE_PERMISSIONS, c,
                                         "{\"kinds\":[\"camera\"]}") == BDK_OK);
    t_pump();
    CHECK(complete_grant("{}", &err) == BDK_OK);

    /* A request that names no kinds offers nothing to grant. */
    REQUIRE(bdk_harness_raise_ui_request(t_runtime, BDK_SURFACE_PERMISSIONS, c, "{}") == BDK_OK);
    t_pump();
    CHECK(complete_grant("{\"kinds\":[\"camera\"]}", &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "ui", "widensRequest"));
}

static void test_a_dropped_request_is_cancelled_never_granted(void) {
    bdk_contents_t c = setup(BDK_SURFACE_PERMISSIONS, BDK_UI_HANDLED);
    REQUIRE(bdk_harness_raise_ui_request(t_runtime, BDK_SURFACE_PERMISSIONS, c,
                                         "{\"kinds\":[\"camera\"]}") == BDK_OK);
    t_pump();
    bdk_ui_request_release(g_request);
    bdk_harness_ui_outcome_t o = last_outcome();
    CHECK(o.kind == BDK_HARNESS_UI_CANCELLED && o.completion[0] == '\0');
    bdk_error_t err = 0;
    CHECK(complete_grant("{}", &err) == BDK_ERR_STALE_HANDLE);
    CHECK(last_outcome().kind == BDK_HARNESS_UI_CANCELLED);
}

static void raise_numbered(uint32_t surface, bdk_contents_t c, int n) {
    char payload[32];
    snprintf(payload, sizeof payload, "{\"n\":%d}", n);
    REQUIRE(bdk_harness_raise_ui_request(t_runtime, surface, c, payload) == BDK_OK);
}

static void test_requests_of_a_kind_for_a_page_reach_the_handler_one_at_a_time(void) {
    bdk_contents_t c = setup(BDK_SURFACE_PERMISSIONS, BDK_UI_HANDLED);
    for (int n = 1; n <= 3; ++n)
        raise_numbered(BDK_SURFACE_PERMISSIONS, c, n);
    t_pump();
    CHECK(g_handler_calls == 1 && strcmp(g_payload, "{\"n\":1}") == 0);
    t_pump();
    CHECK(g_handler_calls == 1); /* the second waits for the first */
    bdk_error_t err = 0;
    CHECK(bdk_ui_complete(g_request, S("deny"), S("{}"), no_gesture(), &err) == BDK_OK);
    CHECK(g_handler_calls == 1); /* never synchronous */
    t_pump();
    CHECK(g_handler_calls == 2 && strcmp(g_payload, "{\"n\":2}") == 0);
    bdk_ui_request_release(g_request); /* dropped: the third follows */
    t_pump();
    CHECK(g_handler_calls == 3 && strcmp(g_payload, "{\"n\":3}") == 0);
    CHECK(bdk_ui_complete(g_request, S("deny"), S("{}"), no_gesture(), &err) == BDK_OK);
    t_pump();
    CHECK(g_handler_calls == 3);
}

static void test_queues_are_per_page_and_per_kind(void) {
    reset_counters(BDK_UI_HANDLED);
    t_start();
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_PERMISSIONS, ui_handler, NULL, NULL, &err) ==
            BDK_OK);
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_FIND, ui_handler, NULL, NULL, &err) ==
            BDK_OK);
    t_freeze();
    bdk_profile_t p = t_new_profile("UI");
    bdk_contents_t a = t_new_contents(p), b = t_new_contents(p);
    raise_numbered(BDK_SURFACE_PERMISSIONS, a, 1);
    raise_numbered(BDK_SURFACE_PERMISSIONS, b, 2); /* another page */
    raise_numbered(BDK_SURFACE_FIND, a, 3);        /* another kind */
    raise_numbered(BDK_SURFACE_PERMISSIONS, a, 4); /* waits */
    t_pump();
    CHECK(g_handler_calls == 3);
}

static void test_a_handler_that_answers_at_once_sees_every_queued_request(void) {
    bdk_contents_t c = setup(BDK_SURFACE_PERMISSIONS, BDK_UI_USE_DEFAULT);
    for (int n = 1; n <= 4; ++n)
        raise_numbered(BDK_SURFACE_PERMISSIONS, c, n);
    t_pump();
    t_pump();
    t_pump();
    t_pump();
    CHECK(g_handler_calls == 4);
    CHECK(strcmp(g_payload, "{\"n\":4}") == 0);
    CHECK(last_outcome().kind == BDK_HARNESS_UI_DEFAULTED);
}

static void test_a_withdrawn_request_leaves_the_queue_unseen(void) {
    bdk_contents_t c = setup(BDK_SURFACE_PERMISSIONS, BDK_UI_HANDLED);
    raise_numbered(BDK_SURFACE_PERMISSIONS, c, 1);
    raise_numbered(BDK_SURFACE_PERMISSIONS, c, 2);
    t_pump();
    REQUIRE(g_handler_calls == 1);
    REQUIRE(bdk_harness_withdraw_ui_requests(t_runtime) == BDK_OK);
    t_pump();
    t_pump();
    CHECK(g_handler_calls == 1); /* the second never reached the handler */
    CHECK(last_outcome().kind == BDK_HARNESS_UI_WITHDRAWN);
    raise_numbered(BDK_SURFACE_PERMISSIONS, c, 3); /* the queue is free again */
    t_pump();
    CHECK(g_handler_calls == 2 && strcmp(g_payload, "{\"n\":3}") == 0);
}

static void test_closing_the_page_withdraws_its_queue(void) {
    bdk_contents_t c = setup(BDK_SURFACE_PERMISSIONS, BDK_UI_HANDLED);
    raise_numbered(BDK_SURFACE_PERMISSIONS, c, 1);
    raise_numbered(BDK_SURFACE_PERMISSIONS, c, 2);
    t_pump();
    REQUIRE(bdk_contents_destroy(c) == BDK_OK);
    t_pump();
    t_pump();
    CHECK(g_handler_calls == 1);
    CHECK(last_outcome().kind == BDK_HARNESS_UI_WITHDRAWN);
}

static void on_withdrawn_fwd(void* ctx) {
    on_withdrawn(ctx);
}
static void on_withdrawn_rel_fwd(void* ctx) {
    on_withdrawn_released(ctx);
}

const t_test_t t_ui_tests[] = {
    {"ui.handler_receives_request_and_completes_once",
     test_handler_receives_the_request_and_completes_it_once},
    {"ui.granting_needs_a_gesture", test_granting_needs_a_gesture},
    {"ui.complete_validates_arguments", test_complete_validates_its_arguments},
    {"ui.use_default_and_reject_responses", test_use_default_and_reject_responses},
    {"ui.handled_request_can_use_default_later", test_handled_request_can_use_the_default_later},
    {"ui.surface_without_handler_uses_default", test_a_surface_with_no_handler_uses_the_default},
    {"ui.withdrawn_request_notifies_and_cannot_complete",
     test_withdrawn_request_notifies_and_cannot_be_completed},
    {"ui.releasing_uncompleted_request_cancels_it",
     test_releasing_an_uncompleted_request_cancels_it},
    {"ui.handler_registration_rules", test_handler_registration_rules},
    {"ui.withdrawn_release_when_handler_uses_default",
     test_withdrawn_release_when_the_handler_uses_the_default},
    {"ui.withdrawn_release_when_handler_rejects", test_withdrawn_release_when_the_handler_rejects},
    {"ui.withdrawn_release_when_completed", test_withdrawn_release_when_completed},
    {"ui.withdrawn_release_when_default_used_later",
     test_withdrawn_release_when_the_default_is_used_later},
    {"ui.withdrawn_callback_then_release_when_withdrawn",
     test_withdrawn_callback_then_release_when_withdrawn},
    {"ui.withdrawn_release_when_request_released",
     test_withdrawn_release_when_the_request_is_released},
    {"ui.withdrawn_when_contents_closed", test_withdrawn_when_the_contents_is_closed},
    {"ui.withdrawn_when_contents_destroyed", test_withdrawn_when_the_contents_is_destroyed},
    {"ui.withdrawn_release_at_shutdown", test_withdrawn_release_at_runtime_shutdown},
    {"ui.replacing_withdrawn_handler_releases_old",
     test_replacing_the_withdrawn_handler_releases_the_old_one},
    {"ui.withdrawn_handler_set_too_late_is_released",
     test_a_withdrawn_handler_set_too_late_is_released},
    {"ui.surface_mode_not_allowed_fails_start", test_a_disallowed_surface_mode_fails_the_start},
    {"ui.unknown_surface_or_mode_fails_start", test_unknown_surfaces_and_modes_fail_the_start},
    {"ui.every_surface_name_is_accepted", test_every_surface_name_is_accepted},
    {"ui.custom_surface_needs_handler_before_freeze",
     test_a_custom_surface_needs_a_handler_before_the_freeze},
    {"ui.disabled_surface_denies_without_handler",
     test_a_disabled_surface_denies_without_calling_a_handler},
    {"ui.disabled_surface_does_not_queue", test_a_disabled_surface_does_not_queue},
    {"ui.custom_surface_calls_its_handler", test_a_custom_surface_calls_its_handler},
    {"ui.reject_denies_and_never_grants", test_reject_denies_and_never_grants},
    {"ui.interstitial_reject_goes_back", test_rejecting_an_interstitial_goes_back},
    {"ui.interstitial_drop_goes_back", test_dropping_an_interstitial_goes_back},
    {"ui.interstitial_withdrawn_stays_withdrawn",
     test_an_interstitial_that_is_withdrawn_stays_withdrawn},
    {"ui.interstitial_can_use_default", test_an_interstitial_can_use_the_default_presenter},
    {"ui.interstitial_completions_are_go_back_or_proceed",
     test_an_interstitial_is_completed_by_go_back_or_proceed_only},
    {"ui.interstitial_proceed_needs_gesture_and_override",
     test_proceeding_needs_a_gesture_and_an_overridable_request},
    {"ui.granting_completions_need_gesture_per_surface",
     test_granting_completions_need_a_gesture_on_every_surface},
    {"ui.permission_grant_cannot_widen", test_a_permission_grant_cannot_widen_the_request},
    {"ui.dropped_request_is_cancelled_never_granted",
     test_a_dropped_request_is_cancelled_never_granted},
    {"ui.find_present_raises_a_find_request",
     test_present_raises_a_find_request_the_handler_completes},
    {"ui.print_raises_a_printing_request", test_print_raises_a_user_initiated_printing_request},
    {"ui.queue_one_at_a_time", test_requests_of_a_kind_for_a_page_reach_the_handler_one_at_a_time},
    {"ui.queue_per_page_and_kind", test_queues_are_per_page_and_per_kind},
    {"ui.queue_drains_when_handler_answers_at_once",
     test_a_handler_that_answers_at_once_sees_every_queued_request},
    {"ui.queue_withdrawn_request_unseen", test_a_withdrawn_request_leaves_the_queue_unseen},
    {"ui.queue_withdrawn_when_page_closed", test_closing_the_page_withdraws_its_queue},
    {NULL, NULL}};
