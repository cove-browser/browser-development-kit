/* The navigation policy (T-SWF-10, swift-api.md 6.3): with no handler nothing changes; allow,
 * cancel and redirect, answered at once and later; server redirects and page navigations are asked;
 * the redirect target loads and is not asked again; a redirect loop ends; a decision goes stale
 * when its navigation ends first, and the handler's context is released once. */
#include "harness_broker.h"
#include "test_support.h"

/* A handler that records what it was asked and answers as the test chose. */
typedef struct policy_seen {
    int calls, releases;
    uint32_t answer;         /* 0: keep the decision in held */
    const char* redirect;    /* the URL of a BDK_NAVIGATION_REDIRECT answer */
    int redirect_redirects;  /* answer every server redirect with a redirect to `redirect` */
    int event_count_at_call; /* how many page events had arrived when it was last called */
    const t_events_t* events;
    bdk_context_t held;
    bdk_contents_t contents;
    char navigation[512];
} policy_seen_t;

static policy_seen_t g_policy;

static void on_policy(void* ctx, bdk_context_t decision, bdk_contents_t contents,
                      bdk_string_t navigation) {
    policy_seen_t* p = (policy_seen_t*)ctx;
    ++p->calls;
    p->contents = contents;
    p->event_count_at_call = p->events ? p->events->count : -1;
    snprintf(p->navigation, sizeof p->navigation, "%.*s", (int)navigation.length, navigation.data);
    uint32_t answer = p->answer;
    if (p->redirect_redirects)
        answer = str_has(navigation, "\"isRedirect\":true") ? BDK_NAVIGATION_REDIRECT
                                                            : BDK_NAVIGATION_ALLOW;
    if (!answer) {
        p->held = decision;
        return;
    }
    bdk_error_t err = 0;
    CHECK(bdk_navigation_policy_decide(decision, answer, S(p->redirect ? p->redirect : ""), &err) ==
          BDK_OK);
}

static void on_policy_release(void* ctx) {
    ++((policy_seen_t*)ctx)->releases;
}

/* Starts, sets the handler answering `answer` (none when `with_handler` is 0), freezes and opens a
   profile. */
static bdk_profile_t start_policy(int with_handler, uint32_t answer) {
    memset(&g_policy, 0, sizeof g_policy);
    g_policy.answer = answer;
    t_start();
    if (with_handler) {
        bdk_error_t err = 0;
        REQUIRE(bdk_navigation_policy_set_handler(t_runtime, on_policy, &g_policy,
                                                  on_policy_release, &err) == BDK_OK);
    }
    t_freeze();
    return t_new_profile("Policy");
}

static bdk_contents_t watched_page(bdk_profile_t profile, t_events_t* ev) {
    bdk_contents_t c = t_new_contents(profile);
    t_ev_init(ev);
    g_policy.events = ev;
    REQUIRE(bdk_contents_subscribe(c, t_on_contents_event, ev, t_on_event_release) != 0);
    return c;
}

static void load(bdk_contents_t c, const char* url) {
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_load(c, S(url), &err) == BDK_OK);
    t_pump();
}

static int find_event(const t_events_t* ev, uint32_t kind, int from) {
    for (int i = from; i < ev->count; ++i) {
        if (ev->kind[i] == kind)
            return i;
    }
    return -1;
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

static void expect_committed(const t_events_t* ev, const char* url) {
    int committed = find_event(ev, BDK_CONTENTS_EVENT_NAVIGATION_COMMITTED, 0);
    REQUIRE(committed >= 0);
    CHECK(strcmp(ev->url[committed], url) == 0);
    CHECK(find_event(ev, BDK_CONTENTS_EVENT_LOAD_FINISHED, committed) > committed);
}

/* The navigation ended with no commit, for the reason in `code`; the page stayed at `stayed`. */
static void expect_ended(bdk_contents_t c, const t_events_t* ev, const char* code,
                         const char* stayed) {
    int failed = find_event(ev, BDK_CONTENTS_EVENT_NAVIGATION_FAILED, 0);
    REQUIRE(failed >= 0);
    char want[64];
    snprintf(want, sizeof want, "\"code\":\"%s\"", code);
    CHECK(str_has(S(ev->detail[failed]), want));
    CHECK(ev->net_error[failed] == -3);
    CHECK((ev->flags[failed] & BDK_EVENT_FLAG_ERROR_PAGE) == 0);
    char url[128];
    current_url(c, url, sizeof url);
    CHECK(strcmp(url, stayed) == 0);
}

/* ---- No handler ---- */

static void test_with_no_handler_nothing_is_asked(void) {
    bdk_profile_t p = start_policy(0, 0);
    t_events_t ev;
    bdk_contents_t c = watched_page(p, &ev);
    load(c, "https://example.test/");
    CHECK(g_policy.calls == 0);
    CHECK(find_event(&ev, BDK_CONTENTS_EVENT_NAVIGATION_FAILED, 0) < 0);
    REQUIRE(ev.count >= 2);
    CHECK(ev.kind[0] == BDK_CONTENTS_EVENT_NAVIGATION_STARTED);
    CHECK(ev.kind[1] == BDK_CONTENTS_EVENT_NAVIGATION_COMMITTED);
    expect_committed(&ev, "https://example.test/");
}

/* ---- Allow ---- */

static void test_allow_at_once_loads_the_page(void) {
    bdk_profile_t p = start_policy(1, BDK_NAVIGATION_ALLOW);
    t_events_t ev;
    bdk_contents_t c = watched_page(p, &ev);
    load(c, "https://example.test/a");
    CHECK(g_policy.calls == 1);
    CHECK(g_policy.contents == c);
    /* Asked after the start is reported, before anything commits. */
    CHECK(g_policy.event_count_at_call == 1);
    CHECK(str_has(S(g_policy.navigation), "\"url\":\"https://example.test/a\""));
    CHECK(str_has(S(g_policy.navigation), "\"type\":\"load\""));
    CHECK(str_has(S(g_policy.navigation), "\"method\":\"GET\""));
    CHECK(str_has(S(g_policy.navigation), "\"isUserInitiated\":true"));
    CHECK(str_has(S(g_policy.navigation), "\"isRedirect\":false"));
    CHECK(str_has(S(g_policy.navigation), "\"initiator\":{\"kind\":\"application\"}"));
    CHECK(str_has(S(g_policy.navigation), "\"isPrerender\":false"));
    expect_committed(&ev, "https://example.test/a");
}

static void test_allow_later_waits_for_the_answer(void) {
    bdk_profile_t p = start_policy(1, 0);
    t_events_t ev;
    bdk_contents_t c = watched_page(p, &ev);
    load(c, "https://example.test/a");
    REQUIRE(g_policy.calls == 1 && g_policy.held != 0);
    CHECK(is_loading(c));
    CHECK(find_event(&ev, BDK_CONTENTS_EVENT_NAVIGATION_COMMITTED, 0) < 0);
    CHECK(find_event(&ev, BDK_CONTENTS_EVENT_NAVIGATION_FAILED, 0) < 0);
    bdk_error_t err = 0;
    CHECK(bdk_navigation_policy_decide(g_policy.held, BDK_NAVIGATION_ALLOW, S(""), &err) == BDK_OK);
    /* No callback runs from inside the call. */
    CHECK(find_event(&ev, BDK_CONTENTS_EVENT_NAVIGATION_COMMITTED, 0) < 0);
    t_pump();
    expect_committed(&ev, "https://example.test/a");
    CHECK(g_policy.calls == 1);
}

/* ---- Cancel ---- */

static void test_cancel_at_once_keeps_the_page_where_it_is(void) {
    bdk_profile_t p = start_policy(1, BDK_NAVIGATION_ALLOW);
    t_events_t ev;
    bdk_contents_t c = watched_page(p, &ev);
    load(c, "https://example.test/first");
    g_policy.answer = BDK_NAVIGATION_CANCEL;
    t_ev_init(&ev);
    load(c, "https://example.test/second");
    CHECK(g_policy.calls == 2);
    expect_ended(c, &ev, "cancelledByPolicy", "https://example.test/first");
    CHECK(find_event(&ev, BDK_CONTENTS_EVENT_NAVIGATION_COMMITTED, 0) < 0);
    CHECK(!is_loading(c));
}

static void test_cancel_later_ends_the_navigation(void) {
    bdk_profile_t p = start_policy(1, 0);
    t_events_t ev;
    bdk_contents_t c = watched_page(p, &ev);
    load(c, "https://example.test/a");
    REQUIRE(g_policy.held != 0);
    bdk_error_t err = 0;
    CHECK(bdk_navigation_policy_decide(g_policy.held, BDK_NAVIGATION_CANCEL, S(""), &err) ==
          BDK_OK);
    t_pump();
    expect_ended(c, &ev, "cancelledByPolicy", "about:blank");
    CHECK(find_event(&ev, BDK_CONTENTS_EVENT_NAVIGATION_COMMITTED, 0) < 0);
}

/* ---- Redirect ---- */

static void expect_redirected(bdk_contents_t c, const t_events_t* ev, const char* to) {
    int failed = find_event(ev, BDK_CONTENTS_EVENT_NAVIGATION_FAILED, 0);
    REQUIRE(failed >= 0);
    CHECK(str_has(S(ev->detail[failed]), "\"code\":\"redirectedByPolicy\""));
    char want[160];
    snprintf(want, sizeof want, "\"redirectUrl\":\"%s\"", to);
    CHECK(str_has(S(ev->detail[failed]), want));
    CHECK(ev->net_error[failed] == -3);
    /* Only the redirect target commits; its start follows the end of the first navigation. */
    int started = find_event(ev, BDK_CONTENTS_EVENT_NAVIGATION_STARTED, failed);
    REQUIRE(started > failed);
    CHECK(strcmp(ev->url[started], to) == 0);
    int committed = find_event(ev, BDK_CONTENTS_EVENT_NAVIGATION_COMMITTED, 0);
    REQUIRE(committed > started);
    CHECK(strcmp(ev->url[committed], to) == 0);
    char url[128];
    current_url(c, url, sizeof url);
    CHECK(strcmp(url, to) == 0);
}

static void test_redirect_at_once_loads_the_target(void) {
    bdk_profile_t p = start_policy(1, BDK_NAVIGATION_REDIRECT);
    g_policy.redirect = "https://example.test/instead";
    t_events_t ev;
    bdk_contents_t c = watched_page(p, &ev);
    load(c, "https://example.test/asked");
    /* The application chose the target: its start is not asked again. */
    CHECK(g_policy.calls == 1);
    expect_redirected(c, &ev, "https://example.test/instead");
}

static void test_redirect_later_loads_the_target(void) {
    bdk_profile_t p = start_policy(1, 0);
    t_events_t ev;
    bdk_contents_t c = watched_page(p, &ev);
    load(c, "https://example.test/asked");
    REQUIRE(g_policy.held != 0);
    bdk_error_t err = 0;
    CHECK(bdk_navigation_policy_decide(g_policy.held, BDK_NAVIGATION_REDIRECT,
                                       S("https://example.test/instead"), &err) == BDK_OK);
    t_pump();
    CHECK(g_policy.calls == 1);
    expect_redirected(c, &ev, "https://example.test/instead");
}

/* ---- Decisions ---- */

static void test_a_decision_is_checked_and_answered_once(void) {
    bdk_profile_t p = start_policy(1, 0);
    t_events_t ev;
    bdk_contents_t c = watched_page(p, &ev);
    load(c, "https://example.test/a");
    bdk_context_t d = g_policy.held;
    REQUIRE(d != 0);
    bdk_error_t err = 0;
    CHECK(bdk_navigation_policy_decide(d, 99, S(""), &err) == BDK_ERR_INVALID_ARGUMENT);
    CHECK(bdk_navigation_policy_decide(d, BDK_NAVIGATION_REDIRECT, S("not a url"), &err) ==
          BDK_ERR_INVALID_ARGUMENT);
    CHECK(bdk_navigation_policy_decide(d, BDK_NAVIGATION_REDIRECT, S(""), &err) ==
          BDK_ERR_INVALID_ARGUMENT);
    CHECK(bdk_release(d) == BDK_ERR_INVALID_ARGUMENT); /* answered, not released */
    t_pump();
    CHECK(is_loading(c)); /* still open after the refused answers */
    CHECK(bdk_navigation_policy_decide(d, BDK_NAVIGATION_ALLOW, S(""), &err) == BDK_OK);
    CHECK(bdk_navigation_policy_decide(d, BDK_NAVIGATION_CANCEL, S(""), &err) ==
          BDK_ERR_STALE_HANDLE);
    CHECK(bdk_navigation_policy_decide(0, BDK_NAVIGATION_ALLOW, S(""), &err) ==
          BDK_ERR_STALE_HANDLE);
    t_pump();
    expect_committed(&ev, "https://example.test/a");
}

typedef struct thread_call {
    bdk_context_t decision;
    bdk_status_t decide, set;
} thread_call_t;

static void decide_on_other_thread(void* ctx) {
    thread_call_t* t = (thread_call_t*)ctx;
    t->decide = bdk_navigation_policy_decide(t->decision, BDK_NAVIGATION_ALLOW, S(""), NULL);
    t->set = bdk_navigation_policy_set_handler(t_runtime, NULL, NULL, NULL, NULL);
}

static void test_the_policy_is_ui_thread_only(void) {
    bdk_profile_t p = start_policy(1, 0);
    t_events_t ev;
    bdk_contents_t c = watched_page(p, &ev);
    load(c, "https://example.test/a");
    REQUIRE(g_policy.held != 0);
    thread_call_t t = {g_policy.held, BDK_OK, BDK_OK};
    bdk_harness_run_on_other_thread(decide_on_other_thread, &t);
    CHECK(t.decide == BDK_ERR_WRONG_THREAD);
    CHECK(t.set == BDK_ERR_WRONG_THREAD);
    CHECK(bdk_navigation_policy_decide(g_policy.held, BDK_NAVIGATION_ALLOW, S(""), NULL) == BDK_OK);
}

static void test_a_stopped_or_superseded_navigation_makes_its_decision_stale(void) {
    bdk_profile_t p = start_policy(1, 0);
    t_events_t ev;
    bdk_contents_t c = watched_page(p, &ev);
    load(c, "https://example.test/a");
    bdk_context_t stopped = g_policy.held;
    REQUIRE(stopped != 0);
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_stop(c, &err) == BDK_OK);
    t_pump();
    CHECK(bdk_navigation_policy_decide(stopped, BDK_NAVIGATION_ALLOW, S(""), &err) ==
          BDK_ERR_STALE_HANDLE);

    load(c, "https://example.test/b");
    bdk_context_t superseded = g_policy.held;
    REQUIRE(superseded != 0 && superseded != stopped);
    t_ev_init(&ev);
    load(c, "https://example.test/c");
    CHECK(g_policy.calls == 3);
    CHECK(bdk_navigation_policy_decide(superseded, BDK_NAVIGATION_ALLOW, S(""), &err) ==
          BDK_ERR_STALE_HANDLE);
    int failed = find_event(&ev, BDK_CONTENTS_EVENT_NAVIGATION_FAILED, 0);
    REQUIRE(failed >= 0);
    CHECK(str_has(S(ev.detail[failed]), "\"code\":\"superseded\""));
    CHECK(bdk_navigation_policy_decide(g_policy.held, BDK_NAVIGATION_ALLOW, S(""), &err) == BDK_OK);
    t_pump();
    expect_committed(&ev, "https://example.test/c");
}

static void test_a_page_closed_while_a_decision_is_pending(void) {
    bdk_profile_t p = start_policy(1, 0);
    t_events_t ev;
    bdk_contents_t c = watched_page(p, &ev);
    load(c, "https://example.test/a");
    bdk_context_t closed = g_policy.held;
    REQUIRE(closed != 0);
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_close(c, &err) == BDK_OK);
    t_pump();
    CHECK(find_event(&ev, BDK_CONTENTS_EVENT_CLOSED, 0) >= 0);
    CHECK(bdk_navigation_policy_decide(closed, BDK_NAVIGATION_ALLOW, S(""), &err) ==
          BDK_ERR_STALE_HANDLE);
    CHECK(find_event(&ev, BDK_CONTENTS_EVENT_NAVIGATION_COMMITTED, 0) < 0);
    CHECK(bdk_contents_destroy(c) == BDK_OK);

    /* Destroyed with no close, and the answer arriving after. */
    t_events_t ev2;
    bdk_contents_t gone = watched_page(p, &ev2);
    load(gone, "https://example.test/b");
    bdk_context_t destroyed = g_policy.held;
    REQUIRE(destroyed != 0 && destroyed != closed);
    CHECK(bdk_contents_destroy(gone) == BDK_OK);
    CHECK(bdk_navigation_policy_decide(destroyed, BDK_NAVIGATION_REDIRECT,
                                       S("https://example.test/c"), &err) == BDK_ERR_STALE_HANDLE);
    t_pump();
    CHECK(g_policy.calls == 2);
}

static void test_a_decision_pending_at_shutdown_goes_stale(void) {
    bdk_profile_t p = start_policy(1, 0);
    t_events_t ev;
    bdk_contents_t c = watched_page(p, &ev);
    load(c, "https://example.test/a");
    bdk_context_t pending = g_policy.held;
    REQUIRE(pending != 0);
    t_stop();
    CHECK(g_policy.releases == 1);
    CHECK(bdk_navigation_policy_decide(pending, BDK_NAVIGATION_ALLOW, S(""), NULL) ==
          BDK_ERR_STALE_HANDLE);
}

/* ---- The handler ---- */

static void test_the_handler_is_replaced_removed_and_released_once(void) {
    memset(&g_policy, 0, sizeof g_policy);
    g_policy.answer = BDK_NAVIGATION_CANCEL;
    policy_seen_t second;
    memset(&second, 0, sizeof second);
    second.answer = BDK_NAVIGATION_ALLOW;
    t_start_frozen();
    bdk_error_t err = 0;
    /* Set after the freeze: it is not a registration. */
    REQUIRE(bdk_navigation_policy_set_handler(t_runtime, on_policy, &g_policy, on_policy_release,
                                              &err) == BDK_OK);
    bdk_profile_t p = t_new_profile("Policy");
    t_events_t ev;
    bdk_contents_t c = watched_page(p, &ev);
    load(c, "https://example.test/a");
    CHECK(g_policy.calls == 1);
    REQUIRE(bdk_navigation_policy_set_handler(t_runtime, on_policy, &second, on_policy_release,
                                              &err) == BDK_OK);
    t_pump();
    CHECK(g_policy.releases == 1);
    t_ev_init(&ev);
    load(c, "https://example.test/b");
    CHECK(second.calls == 1 && g_policy.calls == 1);
    expect_committed(&ev, "https://example.test/b");

    /* Removed after a navigation was asked but before the handler saw it: it goes on. */
    t_ev_init(&ev);
    REQUIRE(bdk_contents_load(c, S("https://example.test/c"), &err) == BDK_OK);
    REQUIRE(bdk_navigation_policy_set_handler(t_runtime, NULL, NULL, NULL, &err) == BDK_OK);
    t_pump();
    CHECK(second.calls == 1);
    CHECK(second.releases == 1);
    expect_committed(&ev, "https://example.test/c");
    CHECK(bdk_navigation_policy_set_handler(0, NULL, NULL, NULL, &err) == BDK_ERR_STALE_HANDLE);
    t_stop();
    CHECK(g_policy.releases == 1 && second.releases == 1);
}

static void test_reloads_and_history_navigations_are_asked(void) {
    bdk_profile_t p = start_policy(1, BDK_NAVIGATION_ALLOW);
    t_events_t ev;
    bdk_contents_t c = watched_page(p, &ev);
    load(c, "https://example.test/a");
    load(c, "https://example.test/b");
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_reload(c, 0, &err) == BDK_OK);
    t_pump();
    CHECK(str_has(S(g_policy.navigation), "\"type\":\"reload\""));
    CHECK(str_has(S(g_policy.navigation), "\"url\":\"https://example.test/b\""));
    g_policy.answer = BDK_NAVIGATION_CANCEL;
    REQUIRE(bdk_contents_go_to_offset(c, -1, &err) == BDK_OK);
    t_pump();
    CHECK(g_policy.calls == 4);
    CHECK(str_has(S(g_policy.navigation), "\"type\":\"history\""));
    CHECK(str_has(S(g_policy.navigation), "\"url\":\"https://example.test/a\""));
    char url[128];
    current_url(c, url, sizeof url);
    CHECK(strcmp(url, "https://example.test/b") == 0);
}

/* ---- Page navigations and server redirects (the runtime's page hooks) ---- */

static void test_a_page_navigation_is_asked(void) {
    if (!bdk_harness_page_hooks()) {
        printf("    skipped: the runtime has no page hooks\n");
        return;
    }
    bdk_profile_t p = start_policy(1, BDK_NAVIGATION_ALLOW);
    t_events_t ev;
    bdk_contents_t c = watched_page(p, &ev);
    load(c, "https://example.test/a");
    g_policy.answer = BDK_NAVIGATION_CANCEL;
    REQUIRE(bdk_harness_page_navigate(c, "https://example.test/link") == BDK_OK);
    t_pump();
    CHECK(g_policy.calls == 2);
    CHECK(str_has(S(g_policy.navigation), "\"url\":\"https://example.test/link\""));
    CHECK(str_has(S(g_policy.navigation), "\"initiator\":{\"kind\":\"page\""));
    CHECK(str_has(S(g_policy.navigation), "\"isUserInitiated\":false"));
    char url[128];
    current_url(c, url, sizeof url);
    CHECK(strcmp(url, "https://example.test/a") == 0);
}

static void test_a_server_redirect_is_asked_again(void) {
    if (!bdk_harness_page_hooks()) {
        printf("    skipped: the runtime has no page hooks\n");
        return;
    }
    bdk_profile_t p = start_policy(1, BDK_NAVIGATION_ALLOW);
    REQUIRE(bdk_harness_script_redirect(t_runtime, "https://example.test/old",
                                        "https://example.test/new") == BDK_OK);
    t_events_t ev;
    bdk_contents_t c = watched_page(p, &ev);
    load(c, "https://example.test/old");
    CHECK(g_policy.calls == 2);
    CHECK(str_has(S(g_policy.navigation), "\"url\":\"https://example.test/new\""));
    CHECK(str_has(S(g_policy.navigation), "\"isRedirect\":true"));
    CHECK(str_has(S(g_policy.navigation), "\"redirectedFrom\":\"https://example.test/old\""));
    expect_committed(&ev, "https://example.test/new");

    /* Cancelled at the redirect: nothing commits. */
    g_policy.answer = 0;
    t_ev_init(&ev);
    load(c, "https://example.test/old");
    REQUIRE(g_policy.held != 0);
    bdk_error_t err = 0;
    CHECK(bdk_navigation_policy_decide(g_policy.held, BDK_NAVIGATION_ALLOW, S(""), &err) == BDK_OK);
    t_pump();
    REQUIRE(g_policy.calls == 4);
    CHECK(bdk_navigation_policy_decide(g_policy.held, BDK_NAVIGATION_CANCEL, S(""), &err) ==
          BDK_OK);
    t_pump();
    expect_ended(c, &ev, "cancelledByPolicy", "https://example.test/new");
}

/* Counts the page's events beyond what t_events_t keeps. */
typedef struct tally {
    int failed;
    int32_t last_net_error;
    uint32_t last_flags;
} tally_t;

static void on_tally(void* ctx, const bdk_contents_event_t* ev) {
    tally_t* t = (tally_t*)ctx;
    if (ev->kind == BDK_CONTENTS_EVENT_NAVIGATION_FAILED) {
        ++t->failed;
        t->last_net_error = ev->net_error;
        t->last_flags = ev->flags;
    }
}

static void test_a_redirect_loop_ends_with_an_error_page(void) {
    if (!bdk_harness_page_hooks()) {
        printf("    skipped: the runtime has no page hooks\n");
        return;
    }
    bdk_profile_t p = start_policy(1, 0);
    g_policy.redirect_redirects = 1;
    g_policy.redirect = "https://example.test/start";
    REQUIRE(bdk_harness_script_redirect(t_runtime, "https://example.test/start",
                                        "https://example.test/bounce") == BDK_OK);
    bdk_contents_t c = t_new_contents(p);
    tally_t t;
    memset(&t, 0, sizeof t);
    REQUIRE(bdk_contents_subscribe(c, on_tally, &t, NULL) != 0);
    load(c, "https://example.test/start");
    /* The start, then the server redirect of each of the 21 loads: twenty redirect decisions are
       carried out, the twenty-first is refused. */
    CHECK(g_policy.calls == 1 + 21);
    CHECK(t.failed == 21);
    CHECK(t.last_net_error == -310);
    CHECK((t.last_flags & BDK_EVENT_FLAG_ERROR_PAGE) != 0);
    CHECK(!is_loading(c));
}

/* The engine asks no policy until T-SWF-11; the table runs against the stub. */
const t_test_t t_navigation_policy_tests[] = {
    {"policy.no_handler_asks_nothing", test_with_no_handler_nothing_is_asked},
    {"policy.allow_at_once", test_allow_at_once_loads_the_page},
    {"policy.allow_later", test_allow_later_waits_for_the_answer},
    {"policy.cancel_at_once", test_cancel_at_once_keeps_the_page_where_it_is},
    {"policy.cancel_later", test_cancel_later_ends_the_navigation},
    {"policy.redirect_at_once", test_redirect_at_once_loads_the_target},
    {"policy.redirect_later", test_redirect_later_loads_the_target},
    {"policy.decision_checked_and_answered_once", test_a_decision_is_checked_and_answered_once},
    {"policy.ui_thread_only", test_the_policy_is_ui_thread_only},
    {"policy.stopped_or_superseded_goes_stale",
     test_a_stopped_or_superseded_navigation_makes_its_decision_stale},
    {"policy.page_closed_while_pending", test_a_page_closed_while_a_decision_is_pending},
    {"policy.pending_at_shutdown_goes_stale", test_a_decision_pending_at_shutdown_goes_stale},
    {"policy.handler_replaced_and_released_once",
     test_the_handler_is_replaced_removed_and_released_once},
    {"policy.reload_and_history_asked", test_reloads_and_history_navigations_are_asked},
    {"policy.page_navigation_asked", test_a_page_navigation_is_asked},
    {"policy.server_redirect_asked", test_a_server_redirect_is_asked_again},
    {"policy.redirect_loop_ends", test_a_redirect_loop_ends_with_an_error_page},
    {NULL, NULL},
};
