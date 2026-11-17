/* WebContents: lifecycle, navigation state machine, events, session state, page controls, keyboard.
 */
#include "test_support.h"

#define URL_A "https://example.com/a"
#define URL_B "https://example.com/b"
#define URL_C "http://example.com/c"

static bdk_contents_t g_c;
static t_events_t g_ev;

static bdk_contents_t new_page(void) {
    t_start_frozen();
    return t_new_contents(t_new_profile("Pages"));
}

static bdk_subscription_t watch(bdk_contents_t c, t_events_t* ev) {
    t_ev_init(ev);
    bdk_subscription_t s = bdk_contents_subscribe(c, t_on_contents_event, ev, t_on_event_release);
    REQUIRE(s != 0);
    return s;
}

static void load(bdk_contents_t c, const char* url) {
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_load(c, S(url), &err) == BDK_OK);
}

static void state_of(bdk_contents_t c, bdk_contents_state_t* st) {
    bdk_error_t err = 0;
    memset(st, 0, sizeof *st);
    st->struct_size = sizeof *st;
    REQUIRE(bdk_contents_state(c, st, &err) == BDK_OK);
}

static void test_new_contents_state_defaults(void) {
    g_c = new_page();
    bdk_contents_state_t st;
    state_of(g_c, &st);
    CHECK(str_is(st.url, "about:blank"));
    CHECK(st.loading == 0);
    CHECK(st.zoom == 1.0);
    CHECK(st.visibility == BDK_VISIBILITY_VISIBLE);
    CHECK(st.focused == 0);
    CHECK((st.audio & BDK_AUDIO_MUTED) == 0);
}

static void test_navigation_emits_ordered_events_after_the_call_returns(void) {
    g_c = new_page();
    watch(g_c, &g_ev);
    load(g_c, URL_A);
    CHECK(g_ev.count == 0); /* events never arrive synchronously */
    bdk_contents_state_t st;
    state_of(g_c, &st);
    CHECK(st.loading == 1);
    t_pump();
    REQUIRE(g_ev.count == 4);
    CHECK(g_ev.kind[0] == BDK_CONTENTS_EVENT_NAVIGATION_STARTED);
    CHECK(g_ev.kind[1] == BDK_CONTENTS_EVENT_NAVIGATION_COMMITTED);
    CHECK(g_ev.kind[2] == BDK_CONTENTS_EVENT_TITLE_CHANGED);
    CHECK(g_ev.kind[3] == BDK_CONTENTS_EVENT_LOAD_FINISHED);
    for (int i = 0; i < 4; ++i) {
        CHECK(strcmp(g_ev.url[i], URL_A) == 0);
        CHECK((g_ev.flags[i] & BDK_EVENT_FLAG_MAIN_FRAME) != 0);
    }
    state_of(g_c, &st);
    CHECK(str_is(st.url, URL_A));
    CHECK(st.loading == 0 && st.progress == 1.0);
    CHECK(st.security == BDK_SECURITY_SECURE);
}

static void test_scripted_title_and_insecure_scheme(void) {
    g_c = new_page();
    REQUIRE(bdk_harness_script_navigation(t_runtime, URL_C, 0, "Plain") == BDK_OK);
    load(g_c, URL_C);
    t_pump();
    bdk_contents_state_t st;
    state_of(g_c, &st);
    CHECK(str_is(st.title, "Plain"));
    CHECK(st.security == BDK_SECURITY_NONE);
}

static void test_failed_navigation_reports_the_net_error_and_keeps_the_page(void) {
    g_c = new_page();
    load(g_c, URL_A);
    t_pump();
    REQUIRE(bdk_harness_script_navigation(t_runtime, URL_B, -105, NULL) == BDK_OK);
    watch(g_c, &g_ev);
    load(g_c, URL_B);
    t_pump();
    REQUIRE(g_ev.count == 2);
    CHECK(g_ev.kind[0] == BDK_CONTENTS_EVENT_NAVIGATION_STARTED);
    CHECK(g_ev.kind[1] == BDK_CONTENTS_EVENT_NAVIGATION_FAILED);
    CHECK(g_ev.net_error[1] == -105);
    CHECK((g_ev.flags[1] & BDK_EVENT_FLAG_ERROR_PAGE) != 0);
    bdk_contents_state_t st;
    state_of(g_c, &st);
    CHECK(str_is(st.url, URL_A));
    CHECK(st.loading == 0);
}

static void test_invalid_urls_are_rejected(void) {
    g_c = new_page();
    bdk_error_t err = 0;
    CHECK(bdk_contents_load(g_c, S(""), &err) == BDK_ERR_INVALID_ARGUMENT);
    CHECK(bdk_contents_load(g_c, S("no scheme"), &err) == BDK_ERR_INVALID_ARGUMENT);
}

static void test_history_navigation(void) {
    g_c = new_page();
    load(g_c, URL_A);
    t_pump();
    load(g_c, URL_B);
    t_pump();
    load(g_c, URL_C);
    t_pump();
    bdk_buffer_t b = 0;
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_entries(g_c, &b, &err) == BDK_OK);
    char text[512];
    t_buffer_text(b, text, sizeof text);
    bdk_release(b);
    CHECK(strstr(text, "\"current\":2") != NULL);
    CHECK(strstr(text, URL_A) != NULL && strstr(text, URL_C) != NULL);

    CHECK(bdk_contents_go_to_offset(g_c, -1, &err) == BDK_OK);
    t_pump();
    bdk_contents_state_t st;
    state_of(g_c, &st);
    CHECK(str_is(st.url, URL_B));
    CHECK(bdk_contents_go_to_index(g_c, 0, &err) == BDK_OK);
    t_pump();
    state_of(g_c, &st);
    CHECK(str_is(st.url, URL_A));
    CHECK(bdk_contents_go_to_offset(g_c, -1, &err) == BDK_ERR_INVALID_ARGUMENT);
    CHECK(bdk_contents_go_to_offset(g_c, 0, &err) == BDK_ERR_INVALID_ARGUMENT);
    CHECK(bdk_contents_go_to_index(g_c, 3, &err) == BDK_ERR_INVALID_ARGUMENT);
    CHECK(bdk_contents_go_to_index(g_c, -1, &err) == BDK_ERR_INVALID_ARGUMENT);

    /* A new navigation from the middle drops the forward entries. */
    CHECK(bdk_contents_go_to_index(g_c, 1, &err) == BDK_OK);
    t_pump();
    load(g_c, URL_C);
    t_pump();
    REQUIRE(bdk_contents_entries(g_c, &b, &err) == BDK_OK);
    t_buffer_text(b, text, sizeof text);
    bdk_release(b);
    CHECK(strstr(text, "\"current\":2") != NULL);
}

static void test_reload(void) {
    g_c = new_page();
    bdk_error_t err = 0;
    CHECK(bdk_contents_reload(g_c, 0, &err) == BDK_ERR_FAILED); /* nothing to reload */
    bdk_error_release(err);
    load(g_c, URL_A);
    t_pump();
    watch(g_c, &g_ev);
    CHECK(bdk_contents_reload(g_c, 1, &err) == BDK_OK);
    t_pump();
    CHECK(g_ev.count == 4 && g_ev.kind[0] == BDK_CONTENTS_EVENT_NAVIGATION_STARTED);
    bdk_buffer_t b = 0;
    REQUIRE(bdk_contents_entries(g_c, &b, &err) == BDK_OK);
    char text[256];
    t_buffer_text(b, text, sizeof text);
    bdk_release(b);
    CHECK(strstr(text, "\"current\":0") != NULL);
}

static void test_stop_aborts_a_navigation_in_flight(void) {
    g_c = new_page();
    watch(g_c, &g_ev);
    load(g_c, URL_A);
    bdk_error_t err = 0;
    CHECK(bdk_contents_stop(g_c, &err) == BDK_OK);
    t_pump();
    int committed = 0, failed = 0;
    for (int i = 0; i < g_ev.count; ++i) {
        if (g_ev.kind[i] == BDK_CONTENTS_EVENT_NAVIGATION_COMMITTED)
            ++committed;
        if (g_ev.kind[i] == BDK_CONTENTS_EVENT_NAVIGATION_FAILED)
            ++failed;
    }
    CHECK(committed == 0 && failed == 1);
    bdk_contents_state_t st;
    state_of(g_c, &st);
    CHECK(st.loading == 0);
    CHECK(str_is(st.url, "about:blank"));
    CHECK(bdk_contents_stop(g_c, &err) == BDK_OK); /* stopping an idle page is harmless */
}

static void test_a_newer_navigation_supersedes_the_older(void) {
    g_c = new_page();
    watch(g_c, &g_ev);
    load(g_c, URL_A);
    load(g_c, URL_B);
    t_pump();
    int committed_a = 0, committed_b = 0, failed = 0;
    for (int i = 0; i < g_ev.count; ++i) {
        if (g_ev.kind[i] == BDK_CONTENTS_EVENT_NAVIGATION_COMMITTED) {
            if (strcmp(g_ev.url[i], URL_A) == 0)
                ++committed_a;
            if (strcmp(g_ev.url[i], URL_B) == 0)
                ++committed_b;
        }
        if (g_ev.kind[i] == BDK_CONTENTS_EVENT_NAVIGATION_FAILED)
            ++failed;
    }
    CHECK(committed_a == 0 && committed_b == 1 && failed == 1);
    bdk_buffer_t b = 0;
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_entries(g_c, &b, &err) == BDK_OK);
    char text[256];
    t_buffer_text(b, text, sizeof text);
    bdk_release(b);
    CHECK(strstr(text, URL_A) == NULL);
}

static void test_initial_url_navigates_after_creation(void) {
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Initial");
    bdk_contents_options_t o;
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.profile = p;
    o.initial_url = S(URL_A);
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_create(t_runtime, &o, &g_c, &err) == BDK_OK);
    t_pump();
    bdk_contents_state_t st;
    state_of(g_c, &st);
    CHECK(str_is(st.url, URL_A));
}

static void test_session_state_round_trip(void) {
    g_c = new_page();
    load(g_c, URL_A);
    t_pump();
    load(g_c, URL_B);
    t_pump();
    bdk_error_t err = 0;
    bdk_buffer_t saved = 0;
    REQUIRE(bdk_contents_session_state(g_c, &saved, &err) == BDK_OK);
    bdk_bytes_t bytes = bdk_buffer_bytes(saved);
    CHECK(bytes.length > 0);
    bdk_contents_t other = t_new_contents(t_new_profile("Restored"));
    CHECK(bdk_contents_restore(other, bytes, &err) == BDK_OK);
    bdk_release(saved);
    bdk_contents_state_t st;
    state_of(other, &st);
    CHECK(str_is(st.url, URL_B));
    CHECK(bdk_contents_go_to_offset(other, -1, &err) == BDK_OK);
    t_pump();
    state_of(other, &st);
    CHECK(str_is(st.url, URL_A));
}

static void test_restore_rejects_garbage_and_newer_formats(void) {
    g_c = new_page();
    bdk_error_t err = 0;
    bdk_bytes_t junk = {(const uint8_t*)"nonsense", 8};
    CHECK(bdk_contents_restore(g_c, junk, &err) == BDK_ERR_INVALID_ARGUMENT);
    bdk_bytes_t empty = {NULL, 0};
    CHECK(bdk_contents_restore(g_c, empty, &err) == BDK_ERR_INVALID_ARGUMENT);
    bdk_bytes_t newer = {(const uint8_t*)"BDKSTUB9 0 0\n", 13};
    CHECK(bdk_contents_restore(g_c, newer, &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "contents", "stateTooNew"));
}

static void test_page_controls_show_in_state(void) {
    g_c = new_page();
    bdk_error_t err = 0;
    CHECK(bdk_contents_set_muted(g_c, 1, &err) == BDK_OK);
    CHECK(bdk_contents_set_zoom(g_c, 1.5, &err) == BDK_OK);
    CHECK(bdk_contents_set_user_agent(g_c, S("Test/1.0"), &err) == BDK_OK);
    CHECK(bdk_contents_set_zoom(g_c, 0.0, &err) == BDK_ERR_INVALID_ARGUMENT);
    CHECK(bdk_contents_set_zoom(g_c, -2.0, &err) == BDK_ERR_INVALID_ARGUMENT);
    bdk_contents_state_t st;
    state_of(g_c, &st);
    CHECK((st.audio & BDK_AUDIO_MUTED) != 0);
    CHECK(st.zoom == 1.5);
    CHECK(bdk_contents_set_muted(g_c, 0, &err) == BDK_OK);
    state_of(g_c, &st);
    CHECK((st.audio & BDK_AUDIO_MUTED) == 0);
}

static void test_discard_keeps_the_handle_valid(void) {
    g_c = new_page();
    load(g_c, URL_A);
    t_pump();
    bdk_error_t err = 0;
    CHECK(bdk_contents_set_auto_discardable(g_c, 0, &err) == BDK_OK);
    CHECK(bdk_contents_discard(g_c, &err) == BDK_OK);
    bdk_contents_state_t st;
    state_of(g_c, &st);
    CHECK(str_is(st.url, URL_A));
}

static void test_close_then_destroy(void) {
    g_c = new_page();
    watch(g_c, &g_ev);
    bdk_error_t err = 0;
    CHECK(bdk_contents_close(g_c, &err) == BDK_OK);
    t_pump();
    REQUIRE(g_ev.count == 1);
    CHECK(g_ev.kind[0] == BDK_CONTENTS_EVENT_CLOSED);
    CHECK(bdk_contents_load(g_c, S(URL_A), &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "contents", "stale"));
    CHECK(bdk_contents_destroy(g_c) == BDK_OK);
    CHECK(bdk_contents_destroy(g_c) == BDK_ERR_STALE_HANDLE);
    t_pump();
    CHECK(g_ev.releases == 1); /* the subscription's context is released with the contents */
}

static void test_destroy_mid_navigation_delivers_nothing_more(void) {
    g_c = new_page();
    watch(g_c, &g_ev);
    load(g_c, URL_A);
    CHECK(bdk_contents_destroy(g_c) == BDK_OK);
    t_pump();
    CHECK(g_ev.count == 0);
    CHECK(g_ev.releases == 1);
}

static void test_events_go_only_to_their_own_contents_and_unsubscribe_stops_them(void) {
    g_c = new_page();
    bdk_contents_t other = t_new_contents(t_new_profile("Other"));
    t_events_t mine, theirs;
    bdk_subscription_t s1 = watch(g_c, &mine);
    watch(other, &theirs);
    load(g_c, URL_A);
    t_pump();
    CHECK(mine.count == 4 && theirs.count == 0);
    bdk_unsubscribe(s1);
    CHECK(mine.releases == 0);
    t_pump();
    CHECK(mine.releases == 1);
    load(g_c, URL_B);
    t_pump();
    CHECK(mine.count == 4);
    CHECK(bdk_release(s1) == BDK_ERR_STALE_HANDLE);
}

static void test_two_subscribers_see_the_same_order(void) {
    g_c = new_page();
    t_events_t a, b;
    watch(g_c, &a);
    watch(g_c, &b);
    load(g_c, URL_A);
    t_pump();
    REQUIRE(a.count == b.count && a.count == 4);
    for (int i = 0; i < a.count; ++i)
        CHECK(a.kind[i] == b.kind[i]);
}

static void test_focus_moves_between_contents(void) {
    g_c = new_page();
    bdk_contents_t other = t_new_contents(t_new_profile("Second"));
    t_events_t ea, eb;
    watch(g_c, &ea);
    watch(other, &eb);
    CHECK(bdk_contents_focus(g_c) == BDK_OK);
    CHECK(bdk_contents_focus(other) == BDK_OK);
    t_pump();
    bdk_contents_state_t st;
    state_of(g_c, &st);
    CHECK(st.focused == 0);
    state_of(other, &st);
    CHECK(st.focused == 1);
    CHECK(ea.count == 2 && eb.count == 1);
    CHECK(ea.kind[0] == BDK_CONTENTS_EVENT_FOCUS_CHANGED);
}

static int g_reserve_calls, g_unhandled_calls, g_key_released;
static uint8_t on_reserve(void* ctx, bdk_native_handle_t ev) {
    (void)ev;
    ++g_reserve_calls;
    return (uint8_t)(*(int*)ctx);
}
static void on_unhandled(void* ctx, bdk_native_handle_t ev) {
    (void)ctx;
    (void)ev;
    ++g_unhandled_calls;
}
static void on_key_release(void* ctx) {
    (void)ctx;
    ++g_key_released;
}

static void test_keyboard_handler_reserves_or_passes_on(void) {
    g_c = new_page();
    g_reserve_calls = g_unhandled_calls = g_key_released = 0;
    int reserve = 1;
    bdk_contents_set_keyboard_handler(g_c, on_reserve, on_unhandled, &reserve, on_key_release);
    uint8_t reserved = 9;
    REQUIRE(bdk_harness_simulate_key(g_c, &reserved) == BDK_OK);
    CHECK(reserved == 1 && g_reserve_calls == 1 && g_unhandled_calls == 0);
    reserve = 0;
    REQUIRE(bdk_harness_simulate_key(g_c, &reserved) == BDK_OK);
    CHECK(reserved == 0 && g_unhandled_calls == 1);
    bdk_contents_set_keyboard_handler(g_c, NULL, NULL, NULL,
                                      NULL); /* replaces: the old context is released */
    t_pump();
    CHECK(g_key_released == 1);
}

static void test_native_view_is_an_error_or_a_tagged_handle(void) {
    g_c = new_page();
    bdk_native_handle_t h;
    memset(&h, 0xFF, sizeof h);
    bdk_error_t err = 0;
    bdk_status_t s = bdk_contents_native_view(g_c, &h, &err);
    CHECK(s == BDK_OK || s == BDK_ERR_FAILED);
    if (s == BDK_OK)
        CHECK(h.kind != 0);
    if (err)
        bdk_error_release(err);
}

static int g_runtime_kinds[8], g_runtime_n, g_runtime_released;
static void on_runtime_event(void* ctx, uint32_t kind, bdk_string_t detail) {
    (void)ctx;
    (void)detail;
    if (g_runtime_n < 8)
        g_runtime_kinds[g_runtime_n++] = (int)kind;
}
static void on_runtime_release(void* ctx) {
    (void)ctx;
    ++g_runtime_released;
}

static void test_runtime_events_and_subscription_release(void) {
    t_start();
    g_runtime_n = g_runtime_released = 0;
    bdk_subscription_t s =
        bdk_runtime_subscribe(t_runtime, on_runtime_event, NULL, on_runtime_release);
    REQUIRE(s != 0);
    t_freeze();
    CHECK(g_runtime_n == 1 && g_runtime_kinds[0] == BDK_RUNTIME_EVENT_REGISTRATIONS_FROZEN);
    t_stop();
    CHECK(g_runtime_released == 1); /* released exactly once at shutdown */
    CHECK(g_runtime_kinds[g_runtime_n - 1] == BDK_RUNTIME_EVENT_SHUTDOWN_STARTED);
}

const t_test_t t_contents_tests[] = {
    {"contents.new_state_defaults", test_new_contents_state_defaults},
    {"contents.navigation_events_are_ordered_and_async",
     test_navigation_emits_ordered_events_after_the_call_returns},
    {"contents.scripted_title_and_insecure_scheme", test_scripted_title_and_insecure_scheme},
    {"contents.failed_navigation_reports_net_error",
     test_failed_navigation_reports_the_net_error_and_keeps_the_page},
    {"contents.invalid_urls_are_rejected", test_invalid_urls_are_rejected},
    {"contents.history_navigation", test_history_navigation},
    {"contents.reload", test_reload},
    {"contents.stop_aborts_in_flight_navigation", test_stop_aborts_a_navigation_in_flight},
    {"contents.newer_navigation_supersedes_older", test_a_newer_navigation_supersedes_the_older},
    {"contents.initial_url_navigates", test_initial_url_navigates_after_creation},
    {"contents.session_state_round_trip", test_session_state_round_trip},
    {"contents.restore_rejects_garbage_and_newer", test_restore_rejects_garbage_and_newer_formats},
    {"contents.page_controls_show_in_state", test_page_controls_show_in_state},
    {"contents.discard_keeps_handle_valid", test_discard_keeps_the_handle_valid},
    {"contents.close_then_destroy", test_close_then_destroy},
    {"contents.destroy_mid_navigation_delivers_nothing",
     test_destroy_mid_navigation_delivers_nothing_more},
    {"contents.events_are_per_contents_and_unsubscribe_stops",
     test_events_go_only_to_their_own_contents_and_unsubscribe_stops_them},
    {"contents.subscribers_see_the_same_order", test_two_subscribers_see_the_same_order},
    {"contents.focus_moves_between_contents", test_focus_moves_between_contents},
    {"views.keyboard_handler_reserves_or_passes_on", test_keyboard_handler_reserves_or_passes_on},
    {"views.native_view_is_error_or_tagged_handle",
     test_native_view_is_an_error_or_a_tagged_handle},
    {"runtime.events_and_subscription_release", test_runtime_events_and_subscription_release},
    {NULL, NULL}};
