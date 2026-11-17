/* UI requests. Rules from ui-requests.md section 3 that the ABI can express: gestures, completion
 * once, withdrawal. */
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
    {NULL, NULL}};
