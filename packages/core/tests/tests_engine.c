/* Scenarios only a real engine can run: Chromium raising UI requests from what a page does,
   renderer loss, visibility, and Chromium's own session state and profile storage. Built only into
   the engine conformance app (BDK_HARNESS_ENGINE_TESTS); pages are driven through the engine's test
   hooks (bdk_testing.h), and every http(s) load is served by the engine's test interceptor. */
#include <stdlib.h>

#include "bdk_testing.h"
#include "test_support.h"

/* From harness_engine.mm: shows the page's view in a window of its own, or hides that window. */
void bdk_harness_engine_show(bdk_contents_t contents, int visible);
/* From harness_engine.mm: while set, a new runtime keeps the persistent profiles of the last one.
 */
extern int bdk_harness_engine_keep_profiles;

#define OPENER "https://bdk.test/opener"

/* ---- a UI handler that records what it was given ---- */

typedef struct {
    int calls;
    uint32_t surface;
    bdk_request_t request;
    bdk_contents_t contents;
    char payload[2048];
    /* What to do with the request: 0 leave it open, otherwise this response; with complete set,
       complete it with that completion and arguments instead. */
    bdk_ui_response_t response;
    const char* complete;
    const char* complete_args;
} ui_log_t;

static ui_log_t g_new_windows, g_menus, g_fullscreen;

static bdk_ui_response_t record(void* ctx, const bdk_ui_request_info_t* info) {
    ui_log_t* log = (ui_log_t*)ctx;
    ++log->calls;
    log->surface = info->surface;
    log->request = info->request;
    log->contents = info->contents;
    snprintf(log->payload, sizeof log->payload, "%.*s", (int)info->payload_json.length,
             info->payload_json.data ? info->payload_json.data : "");
    if (log->complete) {
        bdk_native_handle_t none;
        memset(&none, 0, sizeof none);
        bdk_error_t err = 0;
        CHECK(bdk_ui_complete(info->request, S(log->complete),
                              S(log->complete_args ? log->complete_args : ""), none,
                              &err) == BDK_OK);
        return BDK_UI_HANDLED;
    }
    return log->response ? log->response : BDK_UI_HANDLED;
}

static void reset_logs(void) {
    memset(&g_new_windows, 0, sizeof g_new_windows);
    memset(&g_menus, 0, sizeof g_menus);
    memset(&g_fullscreen, 0, sizeof g_fullscreen);
}

/* Starts a runtime with handlers for the three surfaces the engine raises, and one page that has
   loaded the opener page. */
static bdk_contents_t start_with_page(void) {
    t_start();
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_NEW_WINDOWS, record, &g_new_windows, NULL,
                               &err) == BDK_OK);
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_CONTEXT_MENUS, record, &g_menus, NULL,
                               &err) == BDK_OK);
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_FULLSCREEN, record, &g_fullscreen, NULL,
                               &err) == BDK_OK);
    t_freeze();
    bdk_contents_t c = t_new_contents(t_new_profile("Engine"));
    REQUIRE(bdk_contents_load(c, S(OPENER), &err) == BDK_OK);
    t_pump();
    return c;
}

static void run_script(bdk_contents_t c, const char* script, int gesture) {
    REQUIRE(bdk_testing_execute_script(c, S(script), (uint8_t)gesture) == BDK_OK);
    t_pump();
}

static void title_of(bdk_contents_t c, char* out, size_t n) {
    bdk_contents_state_t st;
    memset(&st, 0, sizeof st);
    st.struct_size = sizeof st;
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_state(c, &st, &err) == BDK_OK);
    snprintf(out, n, "%.*s", (int)st.title.length, st.title.data ? st.title.data : "");
}

/* Waits (pumping) until the page's title is `want`, for script results that arrive late. */
static int wait_for_title(bdk_contents_t c, const char* want) {
    char title[256];
    for (int i = 0; i < 40; ++i) {
        title_of(c, title, sizeof title);
        if (strcmp(title, want) == 0)
            return 1;
        t_pump();
    }
    printf("    title is \"%s\", not \"%s\"\n", title, want);
    return 0;
}

static bdk_contents_t new_contents_of(const char* payload) {
    char handle[32];
    REQUIRE(t_json_string(payload, "newContents", handle, sizeof handle));
    return (bdk_contents_t)strtoull(handle, NULL, 10);
}

/* ---- new windows (T-ADP-05) ---- */

static void test_engine_window_open_with_features_raises_a_popup_request(void) {
    reset_logs();
    g_new_windows.complete = "accept";
    bdk_contents_t c = start_with_page();
    run_script(c, "window.open('https://bdk.test/popup', '_blank', 'width=300,height=200')", 1);
    REQUIRE(g_new_windows.calls == 1);
    CHECK(g_new_windows.contents == c);
    CHECK(strstr(g_new_windows.payload, "\"kind\":\"popup\"") != NULL);
    CHECK(strstr(g_new_windows.payload, "\"disposition\":\"popup\"") != NULL);
    CHECK(strstr(g_new_windows.payload, "\"targetURL\":\"https://bdk.test/popup\"") != NULL);
    CHECK(strstr(g_new_windows.payload, "\"width\":300") != NULL);
    CHECK(strstr(g_new_windows.payload, "\"userGesture\":true") != NULL);
    /* Accepted: the new page is the application's, and it loads. */
    bdk_contents_t popup = new_contents_of(g_new_windows.payload);
    CHECK(wait_for_title(popup, "https://bdk.test/popup"));
    CHECK(bdk_contents_destroy(popup) == BDK_OK);
}

static void test_engine_window_open_without_features_raises_a_new_page_request(void) {
    reset_logs();
    g_new_windows.complete = "accept";
    bdk_contents_t c = start_with_page();
    run_script(c, "window.open('https://bdk.test/page')", 1);
    REQUIRE(g_new_windows.calls == 1);
    CHECK(strstr(g_new_windows.payload, "\"kind\":\"newPage\"") != NULL);
    CHECK(strstr(g_new_windows.payload, "\"disposition\":\"foreground\"") != NULL);
}

static void test_engine_a_rejected_new_window_is_destroyed(void) {
    reset_logs();
    g_new_windows.response = BDK_UI_REJECT;
    bdk_contents_t c = start_with_page();
    run_script(c, "window.open('https://bdk.test/rejected', '_blank', 'width=300')", 1);
    REQUIRE(g_new_windows.calls == 1);
    bdk_contents_t popup = new_contents_of(g_new_windows.payload);
    CHECK(bdk_contents_destroy(popup) == BDK_ERR_STALE_HANDLE);
}

/* ---- context menus (T-MAC-05) ---- */

static void test_engine_a_right_click_on_a_link_raises_a_context_menu_with_link_items(void) {
    reset_logs();
    bdk_contents_t c = start_with_page();
    bdk_harness_engine_show(c, 1);
    run_script(c,
               "document.body.style.margin='0';document.body.innerHTML="
               "'<a href=\"https://bdk.test/target\" style=\"display:block;width:400px;"
               "height:300px\">link</a>';document.title='ready'",
               0);
    REQUIRE(wait_for_title(c, "ready"));
    REQUIRE(bdk_testing_click(c, 50, 50, 2) == BDK_OK);
    for (int i = 0; i < 20 && g_menus.calls == 0; ++i)
        t_pump();
    REQUIRE(g_menus.calls == 1);
    CHECK(strstr(g_menus.payload, "\"linkURL\":\"https://bdk.test/target\"") != NULL);
    CHECK(strstr(g_menus.payload, "\"id\":\"openLinkInNewPage\"") != NULL);
    CHECK(strstr(g_menus.payload, "\"id\":\"copyLink\"") != NULL);
    CHECK(strstr(g_menus.payload, "\"location\":{\"x\":50,\"y\":50}") != NULL);
    /* Performing "open link in new page" goes through a new-window request. */
    bdk_native_handle_t none;
    memset(&none, 0, sizeof none);
    bdk_error_t err = 0;
    g_new_windows.complete = "accept";
    REQUIRE(bdk_ui_complete(g_menus.request, S("perform"), S("{\"id\":\"openLinkInNewPage\"}"),
                            none, &err) == BDK_OK);
    t_pump();
    REQUIRE(g_new_windows.calls == 1);
    CHECK(strstr(g_new_windows.payload, "\"kind\":\"newPage\"") != NULL);
    CHECK(strstr(g_new_windows.payload, "\"targetURL\":\"https://bdk.test/target\"") != NULL);
    bdk_harness_engine_show(c, 0);
}

static void test_engine_a_right_click_on_the_page_offers_navigation_items(void) {
    reset_logs();
    bdk_contents_t c = start_with_page();
    bdk_harness_engine_show(c, 1);
    REQUIRE(bdk_testing_click(c, 20, 20, 2) == BDK_OK);
    for (int i = 0; i < 20 && g_menus.calls == 0; ++i)
        t_pump();
    REQUIRE(g_menus.calls == 1);
    CHECK(strstr(g_menus.payload, "\"id\":\"back\"") != NULL);
    CHECK(strstr(g_menus.payload, "\"id\":\"reload\"") != NULL);
    CHECK(strstr(g_menus.payload, "\"linkURL\":\"\"") != NULL);
    bdk_harness_engine_show(c, 0);
}

/* ---- fullscreen (T-MAC-05) ---- */

static void test_engine_html_fullscreen_enters_and_exits_through_requests(void) {
    reset_logs();
    g_fullscreen.complete = "allow";
    bdk_contents_t c = start_with_page();
    bdk_harness_engine_show(c, 1);
    /* Chromium refuses fullscreen to a page that does not have focus. */
    REQUIRE(bdk_contents_focus(c) == BDK_OK);
    run_script(c,
               "document.onfullscreenchange=()=>{document.title="
               "document.fullscreenElement?'in':'out'};"
               "document.documentElement.requestFullscreen()",
               1);
    REQUIRE(wait_for_title(c, "in"));
    REQUIRE(g_fullscreen.calls == 1);
    CHECK(strstr(g_fullscreen.payload, "\"entering\":true") != NULL);
    CHECK(strstr(g_fullscreen.payload, "\"origin\":\"https://bdk.test\"") != NULL);
    run_script(c, "document.exitFullscreen()", 1);
    REQUIRE(wait_for_title(c, "out"));
    CHECK(g_fullscreen.calls == 2);
    CHECK(strstr(g_fullscreen.payload, "\"entering\":false") != NULL);
    bdk_harness_engine_show(c, 0);
}

/* Content has no asynchronous refusal, so a denied page enters fullscreen while the application
   decides and leaves it when denied. What a denial promises is the end state. */
static void test_engine_a_denied_page_ends_up_not_fullscreen(void) {
    reset_logs();
    g_fullscreen.complete = "deny";
    bdk_contents_t c = start_with_page();
    bdk_harness_engine_show(c, 1);
    REQUIRE(bdk_contents_focus(c) == BDK_OK);
    run_script(c, "document.documentElement.requestFullscreen().catch(()=>{})", 1);
    REQUIRE(g_fullscreen.calls >= 1);
    CHECK(strstr(g_fullscreen.payload, "\"entering\"") != NULL);
    for (int i = 0; i < 10; ++i)
        t_pump();
    run_script(c, "document.title=document.fullscreenElement?'fullscreen':'not-fullscreen'", 0);
    CHECK(wait_for_title(c, "not-fullscreen"));
    bdk_harness_engine_show(c, 0);
}

/* ---- renderer loss and visibility ---- */

static void test_engine_a_crashed_renderer_reports_renderer_gone(void) {
    reset_logs();
    bdk_contents_t c = start_with_page();
    t_events_t ev;
    t_ev_init(&ev);
    REQUIRE(bdk_contents_subscribe(c, t_on_contents_event, &ev, t_on_event_release) != 0);
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_load(c, S("chrome://crash"), &err) == BDK_OK);
    int gone = -1;
    for (int i = 0; i < 40 && gone < 0; ++i) {
        t_pump();
        for (int k = 0; k < ev.count; ++k) {
            if (ev.kind[k] == BDK_CONTENTS_EVENT_RENDERER_GONE)
                gone = k;
        }
    }
    REQUIRE(gone >= 0);
    CHECK(strstr(ev.detail[gone], "\"reason\":\"crashed\"") != NULL);
    /* The page recovers when the application reloads it. */
    REQUIRE(bdk_contents_reload(c, 0, &err) == BDK_OK);
    CHECK(wait_for_title(c, OPENER));
}

static void test_engine_showing_and_hiding_the_view_changes_visibility(void) {
    reset_logs();
    bdk_contents_t c = start_with_page();
    t_events_t ev;
    t_ev_init(&ev);
    REQUIRE(bdk_contents_subscribe(c, t_on_contents_event, &ev, t_on_event_release) != 0);
    int shown = 0, hidden = 0;
    bdk_harness_engine_show(c, 1);
    /* Window visibility reaches Chromium asynchronously (occlusion notifications). */
    for (int i = 0; i < 40 && !hidden; ++i) {
        t_pump();
        for (int k = 0; k < ev.count; ++k) {
            if (ev.kind[k] != BDK_CONTENTS_EVENT_VISIBILITY_CHANGED)
                continue;
            if (strstr(ev.detail[k], "\"visible\":true"))
                shown = 1;
            else if (shown && strstr(ev.detail[k], "\"visible\":false"))
                hidden = 1;
        }
        if (shown && i == 0)
            bdk_harness_engine_show(c, 0);
        else if (shown && !hidden && i % 10 == 0)
            bdk_harness_engine_show(c, 0);
    }
    CHECK(shown);
    CHECK(hidden);
    bdk_contents_state_t st;
    memset(&st, 0, sizeof st);
    st.struct_size = sizeof st;
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_state(c, &st, &err) == BDK_OK);
    CHECK(st.visibility == BDK_VISIBILITY_HIDDEN);
}

/* ---- Chromium's session state (T-ADP-07) and profile storage ---- */

static void test_engine_session_state_restores_chromium_history_into_a_new_page(void) {
    reset_logs();
    bdk_contents_t c = start_with_page();
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_load(c, S("https://bdk.test/second"), &err) == BDK_OK);
    t_pump();
    bdk_buffer_t state = 0;
    REQUIRE(bdk_contents_session_state(c, &state, &err) == BDK_OK);
    bdk_bytes_t bytes = bdk_buffer_bytes(state);
    REQUIRE(bytes.length > 8);
    /* After the 4-byte pickle header, the magic "BDKC" as a little-endian word. */
    CHECK(memcmp(bytes.data + 4, "BDKC", 4) == 0);
    if (memcmp(bytes.data + 4, "BDKC", 4) != 0)
        printf("    state begins %02x %02x %02x %02x %02x %02x %02x %02x\n", bytes.data[0],
               bytes.data[1], bytes.data[2], bytes.data[3], bytes.data[4], bytes.data[5],
               bytes.data[6], bytes.data[7]);

    bdk_contents_t copy = t_new_contents(t_new_profile("Copy"));
    REQUIRE(bdk_contents_restore(copy, bytes, &err) == BDK_OK);
    t_pump();
    bdk_buffer_t entries = 0;
    REQUIRE(bdk_contents_entries(copy, &entries, &err) == BDK_OK);
    char text[512];
    t_buffer_text(entries, text, sizeof text);
    CHECK(strstr(text, "\"current\":1") != NULL);
    CHECK(strstr(text, OPENER) != NULL);
    CHECK(strstr(text, "https://bdk.test/second") != NULL);
    CHECK(wait_for_title(copy, "https://bdk.test/second"));
    /* Back works on the restored history. */
    REQUIRE(bdk_contents_go_to_offset(copy, -1, &err) == BDK_OK);
    CHECK(wait_for_title(copy, OPENER));
    bdk_release(entries);
    bdk_release(state);

    /* A stub state is not an engine state. */
    const char* stub = "BDKSTUB1 -1 0\n";
    bdk_bytes_t foreign = {(const uint8_t*)stub, strlen(stub)};
    CHECK(bdk_contents_restore(copy, foreign, &err) == BDK_ERR_INVALID_ARGUMENT);
}

static void test_engine_persistent_profiles_outlive_the_runtime(void) {
    bdk_harness_engine_keep_profiles = 1;
    t_start_frozen();
    char id[64];
    t_create_profile("Kept across runtimes", id, sizeof id);
    t_stop();
    t_start_frozen();
    t_capture_t cap;
    t_cap_init(&cap);
    REQUIRE(bdk_profiles_list(t_runtime, t_on_result, &cap, t_on_release) != 0);
    t_pump();
    REQUIRE(cap.results == 1 && !cap.has_error);
    bdk_harness_engine_keep_profiles = 0;
    CHECK(strstr(cap.json, "Kept across runtimes") != NULL);
    CHECK(strstr(cap.json, id) != NULL);
}

const t_test_t t_engine_tests[] = {
    {"engine_window_open_with_features_raises_a_popup_request",
     test_engine_window_open_with_features_raises_a_popup_request},
    {"engine_window_open_without_features_raises_a_new_page_request",
     test_engine_window_open_without_features_raises_a_new_page_request},
    {"engine_a_rejected_new_window_is_destroyed", test_engine_a_rejected_new_window_is_destroyed},
    {"engine_a_right_click_on_a_link_raises_a_context_menu_with_link_items",
     test_engine_a_right_click_on_a_link_raises_a_context_menu_with_link_items},
    {"engine_a_right_click_on_the_page_offers_navigation_items",
     test_engine_a_right_click_on_the_page_offers_navigation_items},
    {"engine_html_fullscreen_enters_and_exits_through_requests",
     test_engine_html_fullscreen_enters_and_exits_through_requests},
    {"engine_a_denied_page_ends_up_not_fullscreen",
     test_engine_a_denied_page_ends_up_not_fullscreen},
    {"engine_a_crashed_renderer_reports_renderer_gone",
     test_engine_a_crashed_renderer_reports_renderer_gone},
    {"engine_showing_and_hiding_the_view_changes_visibility",
     test_engine_showing_and_hiding_the_view_changes_visibility},
    {"engine_session_state_restores_chromium_history_into_a_new_page",
     test_engine_session_state_restores_chromium_history_into_a_new_page},
    {"engine_persistent_profiles_outlive_the_runtime",
     test_engine_persistent_profiles_outlive_the_runtime},
    {NULL, NULL},
};
