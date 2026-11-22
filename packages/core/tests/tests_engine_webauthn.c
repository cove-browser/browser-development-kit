/* Engine scenarios for passkeys and security keys (ui-requests.md 5.18, T-UIR-09). Chromium's own
   WebAuthn request runs; each step its dialog would show is a `webauthn` request. A virtual USB
   security key (bdk_testing_command "webauthn.virtual") stands in for hardware, so no switch and no
   DevTools protocol is involved. The default presenter is scripted, so no sheet blocks the suite.
   Built only into the engine conformance app (BDK_HARNESS_ENGINE_TESTS). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bdk_testing.h"
#include "test_support.h"

void bdk_harness_engine_show(bdk_contents_t contents, int visible);
bdk_native_handle_t bdk_harness_engine_gesture(bdk_contents_t contents, int kind);
extern const char* bdk_harness_engine_surfaces;

/* http://localhost is a secure context and a valid relying party, and the engine's interceptor
   serves it. */
#define PAGE "http://localhost/webauthn"

/* What the handler does with each step. A step it has no answer for stays open. */
typedef struct {
    int calls;
    int withdrawn;
    char steps[256];          /* the steps seen, comma separated */
    char payload[4096];       /* the last step's payload */
    const char* pin;          /* entered at an enterPIN step */
    const char* account_name; /* chosen at a chooseAccount step, with the gesture below */
    int gesture_kind;
    int deny;           /* cancels at the first step */
    bdk_request_t last; /* the last step delivered */
} webauthn_log_t;

static webauthn_log_t g_log;

static void on_withdrawn(void* ctx) {
    ++((webauthn_log_t*)ctx)->withdrawn;
}

/* The id of the account named `name` in a chooseAccount payload. Keys are written in order, so
   an entry reads {"displayName":..,"id":"N","name":..}. */
static int account_id(const char* payload, const char* name, char* id, size_t n) {
    char pattern[128];
    snprintf(pattern, sizeof pattern, "\"name\":\"%s\"", name);
    const char* at = strstr(payload, pattern);
    if (!at)
        return 0;
    const char* key = NULL;
    for (const char* p = strstr(payload, "\"id\":\""); p && p < at; p = strstr(p + 1, "\"id\":\""))
        key = p;
    if (!key)
        return 0;
    key += strlen("\"id\":\"");
    const char* end = strchr(key, '"');
    snprintf(id, n, "%.*s", (int)(end - key), key);
    return 1;
}

static int step_is(const char* payload, const char* step) {
    char pattern[64];
    snprintf(pattern, sizeof pattern, "\"step\":\"%s\"", step);
    return strstr(payload, pattern) != NULL;
}

static bdk_ui_response_t handle(void* ctx, const bdk_ui_request_info_t* info) {
    webauthn_log_t* log = (webauthn_log_t*)ctx;
    ++log->calls;
    snprintf(log->payload, sizeof log->payload, "%.*s", (int)info->payload_json.length,
             info->payload_json.data ? info->payload_json.data : "");
    const char* step = strstr(log->payload, "\"step\":\"");
    if (step) {
        step += strlen("\"step\":\"");
        size_t used = strlen(log->steps);
        snprintf(log->steps + used, sizeof log->steps - used, "%s%.*s", used ? "," : "",
                 (int)(strchr(step, '"') - step), step);
    }
    log->last = info->request;
    bdk_ui_set_withdrawn_handler(info->request, on_withdrawn, log, NULL);
    bdk_error_t err = 0;
    bdk_native_handle_t none;
    memset(&none, 0, sizeof none);
    if (log->deny) {
        CHECK(bdk_ui_complete(info->request, S("deny"), S("{}"), none, &err) == BDK_OK);
    } else if (log->pin && step_is(log->payload, "enterPIN")) {
        char args[64];
        snprintf(args, sizeof args, "{\"pin\":\"%s\"}", log->pin);
        CHECK(bdk_ui_complete(info->request, S("accept"), S(args), none, &err) == BDK_OK);
    } else if (log->account_name && step_is(log->payload, "chooseAccount")) {
        char id[16], args[64];
        CHECK(account_id(log->payload, log->account_name, id, sizeof id));
        snprintf(args, sizeof args, "{\"account\":\"%s\"}", id);
        bdk_native_handle_t gesture = bdk_harness_engine_gesture(info->contents, log->gesture_kind);
        CHECK(bdk_ui_complete(info->request, S("grant"), S(args), gesture, &err) == BDK_OK);
    }
    return BDK_UI_HANDLED;
}

static void control(const char* verb, const char* args, char* out, size_t n) {
    REQUIRE(bdk_testing_command(t_runtime, S(verb), S(args), out, n) == BDK_OK);
}

static void script(const char* answer) {
    char args[1024];
    snprintf(args, sizeof args, "{\"surface\":\"webauthn\",\"answer\":%s}", answer);
    control("present.script", args, NULL, 0);
}

static int json_int(const char* json, const char* key) {
    char pattern[64];
    snprintf(pattern, sizeof pattern, "\"%s\":", key);
    const char* at = strstr(json, pattern);
    return at ? atoi(at + strlen(pattern)) : -1;
}

static int presented(void) {
    char out[8192];
    control("present.record", "{\"surface\":\"webauthn\"}", out, sizeof out);
    return json_int(out, "shown");
}

static void title_of(bdk_contents_t c, char* out, size_t n) {
    bdk_contents_state_t st;
    memset(&st, 0, sizeof st);
    st.struct_size = sizeof st;
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_state(c, &st, &err) == BDK_OK);
    snprintf(out, n, "%.*s", (int)st.title.length, st.title.data ? st.title.data : "");
}

/* Waits for the page's title to be `want`; the ceremony's outcome is written there. */
static int wait_for_title(bdk_contents_t c, const char* want) {
    char title[256];
    for (int i = 0; i < 400; ++i) {
        title_of(c, title, sizeof title);
        if (strcmp(title, want) == 0)
            return 1;
        t_pump();
    }
    printf("    title is \"%s\", not \"%s\" (steps: %s; last: %.300s)\n", title, want, g_log.steps,
           g_log.payload);
    return 0;
}

/* A runtime with the surfaces given (NULL: all default), a handler on webauthn when asked, and a
   page at PAGE that is shown and focused. */
static bdk_contents_t start_page(int with_handler, const char* surfaces) {
    memset(&g_log, 0, sizeof g_log);
    bdk_harness_engine_surfaces = surfaces;
    t_start();
    control("present.clear", "{}", NULL, 0);
    bdk_error_t err = 0;
    if (with_handler)
        REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_WEBAUTHN, handle, &g_log, NULL, &err) ==
                BDK_OK);
    t_freeze();
    bdk_contents_t c = t_new_contents(t_new_profile("WebAuthn"));
    REQUIRE(bdk_contents_load(c, S(PAGE), &err) == BDK_OK);
    t_pump();
    bdk_harness_engine_show(c, 1);
    REQUIRE(bdk_contents_focus(c) == BDK_OK);
    t_pump();
    return c;
}

static void run(bdk_contents_t c, const char* js) {
    REQUIRE(bdk_testing_execute_script(c, S(js), 1) == BDK_OK);
    t_pump();
}

/* The ceremonies the scenarios start; each writes its outcome, or the error's name, to the title.
 */
static const char* const kGet =
    "(async () => { try { const c = await navigator.credentials.get({publicKey: {"
    "challenge: new Uint8Array(16), rpId: 'localhost', userVerification: 'discouraged',"
    "timeout: 20000}});"
    "document.title = 'got:' + new TextDecoder().decode(c.response.userHandle);"
    "} catch (e) { document.title = e.name; } })()";

static const char* const kCreate =
    "(async () => { try { await navigator.credentials.create({publicKey: {"
    "rp: {name: 'BDK'}, user: {id: new Uint8Array([1, 2, 3]), name: 'ada', displayName: 'Ada'},"
    "challenge: new Uint8Array(16), pubKeyCredParams: [{type: 'public-key', alg: -7}],"
    "authenticatorSelection: {userVerification: 'required'}, timeout: 20000}});"
    "document.title = 'created'; } catch (e) { document.title = e.name; } })()";

static void finish(bdk_contents_t c) {
    control("webauthn.clear", "{}", NULL, 0);
    bdk_harness_engine_show(c, 0);
    for (int i = 0; i < 10; ++i)
        t_pump();
}

/* ---- Scenarios ---- */

/* The crash this surface fixes: with nothing registered, Chromium's tab-modal views dialog ended
   the process. The default presenter shows the step instead, and closing it rejects the promise. */
static void test_a_request_with_no_handler_is_shown_by_the_default_presenter(void) {
    bdk_contents_t c = start_page(0, NULL);
    script("{\"completion\":\"deny\"}");
    run(c, kGet);
    CHECK(wait_for_title(c, "NotAllowedError"));
    CHECK(presented() >= 1);
    char out[8192];
    control("present.record", "{\"surface\":\"webauthn\"}", out, sizeof out);
    CHECK(strstr(out, "relyingParty\\\":\\\"localhost") != NULL);
    CHECK(strstr(out, "operation\\\":\\\"get") != NULL);
    finish(c);
}

static void test_a_handler_gets_the_step_and_cancels_it(void) {
    bdk_contents_t c = start_page(1, NULL);
    g_log.deny = 1;
    run(c, kGet);
    CHECK(wait_for_title(c, "NotAllowedError"));
    CHECK(g_log.calls >= 1);
    CHECK(strstr(g_log.payload, "\"origin\":\"http://localhost\"") != NULL);
    CHECK(strstr(g_log.payload, "\"relyingParty\":\"localhost\"") != NULL);
    CHECK(strstr(g_log.payload, "\"text\":{") != NULL);
    CHECK(strstr(g_log.payload, "\"cancelLabel\":") != NULL);
    CHECK(presented() == 0);
    finish(c);
}

static void test_a_disabled_surface_rejects_without_asking(void) {
    bdk_contents_t c = start_page(1, "{\"webauthn\":\"disabled\"}");
    control("webauthn.virtual", "{}", NULL, 0);
    run(c, kGet);
    CHECK(wait_for_title(c, "NotAllowedError"));
    CHECK(g_log.calls == 0);
    CHECK(presented() == 0);
    finish(c);
}

static void test_a_handler_enters_the_security_keys_pin(void) {
    bdk_contents_t c = start_page(1, NULL);
    control("webauthn.virtual", "{\"pin\":\"1234\"}", NULL, 0);
    g_log.pin = "1234";
    run(c, kCreate);
    CHECK(wait_for_title(c, "created"));
    CHECK(strstr(g_log.steps, "enterPIN") != NULL);
    char out[256];
    control("webauthn.state", "{}", out, sizeof out);
    CHECK(json_int(out, "credentials") == 1);
    finish(c);
}

static void test_a_wrong_pin_comes_back_with_chromiums_error(void) {
    bdk_contents_t c = start_page(1, NULL);
    control("webauthn.virtual", "{\"pin\":\"1234\"}", NULL, 0);
    g_log.pin = "9999";
    run(c, kCreate);
    /* Each wrong PIN is a new enterPIN step that says why, until the key stops asking. */
    for (int i = 0; i < 400 && !step_is(g_log.payload, "error"); ++i)
        t_pump();
    CHECK(strstr(g_log.steps, "enterPIN,enterPIN") != NULL);
    CHECK(strstr(g_log.payload, "\"error\":\"pinSoftBlocked\"") != NULL);
    bdk_error_t err = 0;
    bdk_native_handle_t none;
    memset(&none, 0, sizeof none);
    CHECK(bdk_ui_complete(g_log.last, S("deny"), S("{}"), none, &err) == BDK_OK);
    CHECK(wait_for_title(c, "NotAllowedError"));
    finish(c);
}

static void test_a_handler_chooses_an_account_with_a_gesture(void) {
    bdk_contents_t c = start_page(1, NULL);
    control("webauthn.virtual", "{\"residentKeys\":true,\"uv\":true}", NULL, 0);
    char out[128];
    control("webauthn.inject",
            "{\"rpId\":\"localhost\",\"userId\":\"ada\",\"name\":\"ada\",\"displayName\":\"Ada\"}",
            out, sizeof out);
    CHECK(strstr(out, "\"injected\":true") != NULL);
    control("webauthn.inject",
            "{\"rpId\":\"localhost\",\"userId\":\"grace\",\"name\":\"grace\","
            "\"displayName\":\"Grace\"}",
            out, sizeof out);
    g_log.account_name = "grace";
    g_log.gesture_kind = 0; /* a recent mouse-up in the page's window */
    run(c, kGet);
    CHECK(wait_for_title(c, "got:grace"));
    CHECK(strstr(g_log.steps, "chooseAccount") != NULL);
    finish(c);
}

static void test_a_stale_event_chooses_no_account(void) {
    bdk_contents_t c = start_page(1, NULL);
    control("webauthn.virtual", "{\"residentKeys\":true,\"uv\":true}", NULL, 0);
    control("webauthn.inject", "{\"rpId\":\"localhost\",\"userId\":\"ada\",\"name\":\"ada\"}", NULL,
            0);
    control("webauthn.inject", "{\"rpId\":\"localhost\",\"userId\":\"grace\",\"name\":\"grace\"}",
            NULL, 0);
    g_log.account_name = "ada";
    g_log.gesture_kind = 2; /* a minute old: the engine grants nothing, so the ceremony ends */
    run(c, kGet);
    CHECK(wait_for_title(c, "NotAllowedError"));
    CHECK(strstr(g_log.steps, "chooseAccount") != NULL);
    finish(c);
}

static void test_the_default_presenter_takes_the_pin(void) {
    bdk_contents_t c = start_page(0, NULL);
    control("webauthn.virtual", "{\"pin\":\"1234\"}", NULL, 0);
    script("{\"enterPIN\":{\"completion\":\"accept\",\"pin\":\"1234\"},"
           "\"touchSecurityKey\":{\"none\":true},\"chooseMechanism\":{\"none\":true}}");
    run(c, kCreate);
    CHECK(wait_for_title(c, "created"));
    CHECK(presented() >= 1);
    finish(c);
}

static void test_navigating_away_withdraws_the_open_step(void) {
    bdk_contents_t c = start_page(1, NULL);
    run(c, kGet); /* the real authenticators: the first step waits for a key that never comes */
    for (int i = 0; i < 200 && g_log.calls == 0; ++i)
        t_pump();
    REQUIRE(g_log.calls >= 1);
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_load(c, S("http://localhost/elsewhere"), &err) == BDK_OK);
    for (int i = 0; i < 200 && g_log.withdrawn == 0; ++i)
        t_pump();
    CHECK(g_log.withdrawn >= 1);
    finish(c);
}

const t_test_t t_engine_webauthn_tests[] = {
    {"engine_webauthn_a_request_with_no_handler_is_shown_by_the_default_presenter",
     test_a_request_with_no_handler_is_shown_by_the_default_presenter},
    {"engine_webauthn_a_handler_gets_the_step_and_cancels_it",
     test_a_handler_gets_the_step_and_cancels_it},
    {"engine_webauthn_a_disabled_surface_rejects_without_asking",
     test_a_disabled_surface_rejects_without_asking},
    {"engine_webauthn_a_handler_enters_the_security_keys_pin",
     test_a_handler_enters_the_security_keys_pin},
    {"engine_webauthn_a_wrong_pin_comes_back_with_chromiums_error",
     test_a_wrong_pin_comes_back_with_chromiums_error},
    {"engine_webauthn_a_handler_chooses_an_account_with_a_gesture",
     test_a_handler_chooses_an_account_with_a_gesture},
    {"engine_webauthn_a_stale_event_chooses_no_account", test_a_stale_event_chooses_no_account},
    {"engine_webauthn_the_default_presenter_takes_the_pin",
     test_the_default_presenter_takes_the_pin},
    {"engine_webauthn_navigating_away_withdraws_the_open_step",
     test_navigating_away_withdraws_the_open_step},
    {NULL, NULL},
};
