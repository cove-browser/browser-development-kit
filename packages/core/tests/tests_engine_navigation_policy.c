/* The navigation policy on Chromium (T-SWF-11; swift-api.md 6.3; conventions.md 16): an
   application load, a link, a script navigation, a server redirect and the first navigation of a
   window.open page are each asked; a cancel sends no request; a redirect requests only the new URL.
   The pages come from a loopback HTTP server in a child process, which the engine's test
   interceptor leaves to the network, and which writes the path of every request it receives to a
   log the scenarios read. The rules shared with the stub (allow, cancel and redirect now and later,
   stale decisions, the handler's lifetime, reloads and history) are tests_navigation_policy.c,
   which runs here too. Built only into the engine conformance app (BDK_HARNESS_ENGINE_TESTS). */
#include <stdlib.h>

#include "bdk_testing.h"
#include "engine_http_server.h"
#include "test_support.h"

/* ---- the loopback server (engine_http_server.h) ---- */

static int start_server(void) {
    return t_http_start(NULL, 0);
}

/* ---- the policy ---- */

typedef struct seen {
    int calls;
    uint32_t answer;      /* 0: keep the decision in held */
    const char* redirect; /* the URL of a BDK_NAVIGATION_REDIRECT answer */
    const char* cancel;   /* cancel a navigation whose URL contains this, whatever `answer` is */
    bdk_context_t held;
    bdk_contents_t contents;
    char navigation[1024];
    char all[4096]; /* every navigation asked about, in order */
} seen_t;

static seen_t g_seen;

static void on_policy(void* ctx, bdk_context_t decision, bdk_contents_t contents,
                      bdk_string_t navigation) {
    seen_t* s = (seen_t*)ctx;
    ++s->calls;
    s->contents = contents;
    snprintf(s->navigation, sizeof s->navigation, "%.*s", (int)navigation.length, navigation.data);
    size_t used = strlen(s->all);
    snprintf(s->all + used, sizeof s->all - used, "%s\n", s->navigation);
    uint32_t answer = s->answer;
    if (s->cancel && strstr(s->navigation, s->cancel))
        answer = BDK_NAVIGATION_CANCEL;
    if (!answer) {
        s->held = decision;
        return;
    }
    bdk_error_t err = 0;
    CHECK(bdk_navigation_policy_decide(decision, answer, S(s->redirect ? s->redirect : ""), &err) ==
          BDK_OK);
}

static int has(const char* text, const char* needle) {
    return strstr(text, needle) != NULL;
}

static void current_url(bdk_contents_t c, char* out, size_t n) {
    bdk_contents_state_t st;
    memset(&st, 0, sizeof st);
    st.struct_size = sizeof st;
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_state(c, &st, &err) == BDK_OK);
    snprintf(out, n, "%.*s", (int)st.url.length, st.url.data ? st.url.data : "");
}

static int is_loading(bdk_contents_t c) {
    bdk_contents_state_t st;
    memset(&st, 0, sizeof st);
    st.struct_size = sizeof st;
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_state(c, &st, &err) == BDK_OK);
    return st.loading;
}

/* Pumps until the page has stopped loading at `url` (with its title, the server's page). */
static int wait_at(bdk_contents_t c, const char* url) {
    char now[256] = "";
    for (int i = 0; i < 400; ++i) {
        current_url(c, now, sizeof now);
        if (strcmp(now, url) == 0 && !is_loading(c))
            return 1;
        t_pump();
    }
    printf("    the page is at %s, not %s\n", now, url);
    return 0;
}

/* Pumps until the policy has been asked `calls` times. */
static int wait_calls(int calls) {
    for (int i = 0; i < 400 && g_seen.calls < calls; ++i)
        t_pump();
    if (g_seen.calls != calls)
        printf("    asked %d times, not %d\n", g_seen.calls, calls);
    return g_seen.calls == calls;
}

/* Pumps a little more, so a request the page should not have sent would have reached the server.
 */
static void settle(void) {
    for (int i = 0; i < 40; ++i)
        t_pump();
}

static t_events_t g_events;

static int failed_with(const char* code) {
    char want[64];
    snprintf(want, sizeof want, "\"code\":\"%s\"", code);
    for (int i = 0; i < g_events.count; ++i) {
        if (g_events.kind[i] == BDK_CONTENTS_EVENT_NAVIGATION_FAILED &&
            has(g_events.detail[i], want))
            return 1;
    }
    return 0;
}

/* A runtime with the policy handler (none when `with_handler` is 0) and a watched page. */
static bdk_contents_t start(int with_handler, uint32_t answer) {
    memset(&g_seen, 0, sizeof g_seen);
    g_seen.answer = answer;
    t_start();
    if (with_handler) {
        bdk_error_t err = 0;
        REQUIRE(bdk_navigation_policy_set_handler(t_runtime, on_policy, &g_seen, NULL, &err) ==
                BDK_OK);
    }
    t_freeze();
    bdk_contents_t c = t_new_contents(t_new_profile("Policy"));
    t_ev_init(&g_events);
    REQUIRE(bdk_contents_subscribe(c, t_on_contents_event, &g_events, t_on_event_release) != 0);
    return c;
}

static void load(bdk_contents_t c, const char* url) {
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_load(c, S(url), &err) == BDK_OK);
    t_pump();
}

static void run(bdk_contents_t c, const char* js, int gesture) {
    REQUIRE(bdk_testing_execute_script(c, S(js), (uint8_t)gesture) == BDK_OK);
    t_pump();
}

#define NEEDS_SERVER()                                                                             \
    do {                                                                                           \
        if (!start_server()) {                                                                     \
            printf("    skipped: no loopback socket\n");                                           \
            return;                                                                                \
        }                                                                                          \
    } while (0)

/* ---- scenarios ---- */

static void test_with_no_handler_the_page_loads_unasked(void) {
    NEEDS_SERVER();
    bdk_contents_t c = start(0, 0);
    load(c, t_http_url("/plain"));
    CHECK(wait_at(c, t_http_url("/plain")));
    CHECK(t_http_requested("/plain"));
    CHECK(g_seen.calls == 0);
    t_http_stop();
}

static void test_an_application_load_is_asked_and_a_cancel_sends_no_request(void) {
    NEEDS_SERVER();
    bdk_contents_t c = start(1, BDK_NAVIGATION_ALLOW);
    load(c, t_http_url("/allowed"));
    CHECK(wait_at(c, t_http_url("/allowed")));
    CHECK(g_seen.calls == 1);
    CHECK(g_seen.contents == c);
    char want[160];
    snprintf(want, sizeof want, "\"url\":\"%s\"", t_http_url("/allowed"));
    CHECK(has(g_seen.navigation, want));
    CHECK(has(g_seen.navigation, "\"type\":\"load\""));
    CHECK(has(g_seen.navigation, "\"initiator\":{\"kind\":\"application\"}"));
    CHECK(has(g_seen.navigation, "\"isUserInitiated\":true"));
    CHECK(t_http_requested("/allowed"));

    g_seen.answer = BDK_NAVIGATION_CANCEL;
    t_ev_init(&g_events);
    load(c, t_http_url("/cancelled"));
    CHECK(wait_calls(2));
    settle();
    CHECK(!t_http_requested("/cancelled"));
    CHECK(failed_with("cancelledByPolicy"));
    CHECK(wait_at(c, t_http_url("/allowed")));
    t_http_stop();
}

static void test_a_redirect_requests_only_the_new_url(void) {
    NEEDS_SERVER();
    bdk_contents_t c = start(1, BDK_NAVIGATION_REDIRECT);
    char instead[128];
    snprintf(instead, sizeof instead, "%s", t_http_url("/instead"));
    g_seen.redirect = instead;
    load(c, t_http_url("/asked"));
    CHECK(wait_at(c, instead));
    /* The application chose the target: its start is not asked again. */
    CHECK(g_seen.calls == 1);
    CHECK(failed_with("redirectedByPolicy"));
    CHECK(t_http_requested("/instead"));
    CHECK(!t_http_requested("/asked"));
    t_http_stop();
}

static void test_links_and_script_navigations_are_asked(void) {
    NEEDS_SERVER();
    bdk_contents_t c = start(1, BDK_NAVIGATION_ALLOW);
    load(c, t_http_url("/start"));
    REQUIRE(wait_at(c, t_http_url("/start")));
    char origin[64];
    snprintf(origin, sizeof origin, "\"origin\":\"%s\"", t_http_origin());

    /* A link the user follows. */
    run(c,
        "const a = document.createElement('a'); a.href = '/linked';"
        "document.body.appendChild(a); a.click();",
        1);
    CHECK(wait_at(c, t_http_url("/linked")));
    CHECK(g_seen.calls == 2);
    CHECK(has(g_seen.navigation, "\"initiator\":{\"kind\":\"page\""));
    CHECK(has(g_seen.navigation, origin));
    CHECK(has(g_seen.navigation, "\"isUserInitiated\":true"));

    /* A script navigation with no gesture, cancelled. */
    g_seen.answer = BDK_NAVIGATION_CANCEL;
    t_ev_init(&g_events);
    run(c, "location.assign('/scripted')", 0);
    CHECK(wait_calls(3));
    CHECK(has(g_seen.navigation, "/scripted\""));
    CHECK(has(g_seen.navigation, "\"initiator\":{\"kind\":\"page\""));
    CHECK(has(g_seen.navigation, "\"isUserInitiated\":false"));
    settle();
    CHECK(!t_http_requested("/scripted"));
    CHECK(failed_with("cancelledByPolicy"));
    CHECK(wait_at(c, t_http_url("/linked")));
    t_http_stop();
}

static void test_a_server_redirect_is_asked_and_can_be_cancelled(void) {
    NEEDS_SERVER();
    bdk_contents_t c = start(1, BDK_NAVIGATION_ALLOW);
    load(c, t_http_url("/redirect/target"));
    CHECK(wait_at(c, t_http_url("/target")));
    CHECK(g_seen.calls == 2);
    char want[160];
    snprintf(want, sizeof want, "\"redirectedFrom\":\"%s\"", t_http_url("/redirect/target"));
    CHECK(has(g_seen.navigation, "\"isRedirect\":true"));
    CHECK(has(g_seen.navigation, want));
    CHECK(t_http_requested("/redirect/target") && t_http_requested("/target"));

    /* Cancelled at the redirect: the redirect's target is never requested. */
    g_seen.cancel = "\"isRedirect\":true";
    t_ev_init(&g_events);
    load(c, t_http_url("/redirect/elsewhere"));
    CHECK(wait_calls(4));
    settle();
    CHECK(t_http_requested("/redirect/elsewhere"));
    CHECK(!t_http_requested("/elsewhere"));
    CHECK(failed_with("cancelledByPolicy"));
    CHECK(wait_at(c, t_http_url("/target")));
    t_http_stop();
}

/* Places every new page the engine asks about where the request says (accepts it). */
static bdk_ui_response_t accept_new_window(void* ctx, const bdk_ui_request_info_t* info) {
    (void)ctx;
    bdk_native_handle_t none;
    memset(&none, 0, sizeof none);
    bdk_error_t err = 0;
    CHECK(bdk_ui_complete(info->request, S("accept"), S(""), none, &err) == BDK_OK);
    return BDK_UI_HANDLED;
}

static void test_the_first_navigation_of_a_window_open_page_is_asked(void) {
    NEEDS_SERVER();
    memset(&g_seen, 0, sizeof g_seen);
    g_seen.answer = BDK_NAVIGATION_ALLOW;
    t_start();
    bdk_error_t err = 0;
    REQUIRE(bdk_navigation_policy_set_handler(t_runtime, on_policy, &g_seen, NULL, &err) == BDK_OK);
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_NEW_WINDOWS, accept_new_window, NULL, NULL,
                               &err) == BDK_OK);
    t_freeze();
    bdk_contents_t c = t_new_contents(t_new_profile("Policy"));
    load(c, t_http_url("/opener"));
    REQUIRE(wait_at(c, t_http_url("/opener")));
    g_seen.cancel = "/opened\"";
    run(c, "window.open('/opened')", 1);
    CHECK(wait_calls(2));
    CHECK(g_seen.contents != 0 && g_seen.contents != c);
    CHECK(has(g_seen.navigation, "/opened\""));
    CHECK(has(g_seen.navigation, "\"initiator\":{\"kind\":\"page\""));
    settle();
    CHECK(!t_http_requested("/opened"));
    t_http_stop();
}

const t_test_t t_engine_navigation_policy_tests[] = {
    {"engine_policy_with_no_handler_the_page_loads_unasked",
     test_with_no_handler_the_page_loads_unasked},
    {"engine_policy_an_application_load_is_asked_and_a_cancel_sends_no_request",
     test_an_application_load_is_asked_and_a_cancel_sends_no_request},
    {"engine_policy_a_redirect_requests_only_the_new_url",
     test_a_redirect_requests_only_the_new_url},
    {"engine_policy_links_and_script_navigations_are_asked",
     test_links_and_script_navigations_are_asked},
    {"engine_policy_a_server_redirect_is_asked_and_can_be_cancelled",
     test_a_server_redirect_is_asked_and_can_be_cancelled},
    {"engine_policy_the_first_navigation_of_a_window_open_page_is_asked",
     test_the_first_navigation_of_a_window_open_page_is_asked},
    {NULL, NULL},
};
