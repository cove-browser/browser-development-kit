/* UI requests. Rules from ui-requests.md section 3 that the ABI can express: gestures, completion
 * once, withdrawal. */
#include "test_support.h"

static int g_handler_calls, g_handler_released, g_withdrawn, g_withdrawn_released;
static bdk_request_t g_request;
static uint32_t g_seen_surface;
static bdk_ui_response_t g_answer;
static char g_payload[96];

static bdk_ui_response_t ui_handler(void* ctx, const bdk_ui_request_info_t* info) {
    (void)ctx;
    ++g_handler_calls;
    g_request = info->request;
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
    {NULL, NULL}};
