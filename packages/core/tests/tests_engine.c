/* Scenarios only a real engine can run: Chromium raising UI requests from what a page does,
   renderer loss, visibility, and Chromium's own session state and profile storage. Built only into
   the engine conformance app (BDK_HARNESS_ENGINE_TESTS); pages are driven through the engine's test
   hooks (bdk_testing.h), and every http(s) load is served by the engine's test interceptor. */
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "bdk_testing.h"
#include "test_support.h"

/* From harness_engine.mm: shows the page's view in a window of its own, or hides that window. */
void bdk_harness_engine_show(bdk_contents_t contents, int visible);
/* From harness_engine.mm: a key press delivered to the page's window as AppKit would, with Command
   if `command`; an input method composing `marked` then committing `text` in the focused page; and
   the text of the page's accessibility tree as VoiceOver reads it (returns whether a web area was
   found). */
void bdk_harness_engine_key(bdk_contents_t contents, unsigned short key_code,
                            const char* characters, int command);
int bdk_harness_engine_compose(bdk_contents_t contents, const char* marked, const char* text);
int bdk_harness_engine_window_is_key(bdk_contents_t contents);
int bdk_harness_engine_accessibility_text(bdk_contents_t contents, char* out, size_t n);
/* From harness_engine.mm: an application window registered with the runtime (returns its index,
   or -1), its page views added to it or removed from it, and the window unregistered and closed;
   and how many NSWindows the application has. */
int bdk_harness_engine_window_open(bdk_runtime_t rt);
void bdk_harness_engine_window_attach(int window, bdk_contents_t contents);
void bdk_harness_engine_window_detach(bdk_contents_t contents);
void bdk_harness_engine_window_close(int window);
size_t bdk_harness_engine_app_window_count(void);
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

static ui_log_t g_new_windows, g_menus, g_fullscreen, g_devtools;
/* The profile start_with_page opened. */
static bdk_profile_t g_page_profile;

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
    memset(&g_devtools, 0, sizeof g_devtools);
}

/* Starts a runtime with handlers for the surfaces the engine raises here, and one page that has
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
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_DEVTOOLS, record, &g_devtools, NULL, &err) ==
            BDK_OK);
    t_freeze();
    g_page_profile = t_new_profile("Engine");
    bdk_contents_t c = t_new_contents(g_page_profile);
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

/* Left to BDK (no handler, or use-default), the new page is shown by the default presenter in a
   window of BDK's own, at the size window.open asked for, instead of being dropped (T-EXT-12). */
static void test_engine_a_defaulted_new_window_opens_in_the_default_presenter(void) {
    reset_logs();
    g_new_windows.response = BDK_UI_USE_DEFAULT;
    bdk_contents_t c = start_with_page();
    char record[512];
    REQUIRE(bdk_testing_command(t_runtime, S("present.script"),
                                S("{\"surface\":\"new_windows\",\"answer\":{\"none\":true}}"), NULL,
                                0) == BDK_OK);
    run_script(c, "window.open('https://bdk.test/defaulted', '_blank', 'width=320,height=240')", 1);
    REQUIRE(g_new_windows.calls == 1);
    bdk_contents_t popup = new_contents_of(g_new_windows.payload);
    for (int i = 0; i < 10; ++i)
        t_pump();
    REQUIRE(bdk_testing_command(t_runtime, S("present.record"), S("{\"surface\":\"new_windows\"}"),
                                record, sizeof record) == BDK_OK);
    CHECK(strstr(record, "\"shown\":1") != NULL);
    /* The payload is a JSON string inside the record, so its quotes are escaped. */
    CHECK(strstr(record, "\\\"width\\\":320") != NULL);
    CHECK(strstr(record, "\\\"height\\\":240") != NULL);
    CHECK(wait_for_title(popup, "https://bdk.test/defaulted"));
    /* Nobody owns a page in BDK's window: window.close() closes it and destroys the page. */
    run_script(popup, "window.close()", 1);
    for (int i = 0; i < 20; ++i)
        t_pump();
    CHECK(bdk_contents_destroy(popup) == BDK_ERR_STALE_HANDLE);
    bdk_testing_command(t_runtime, S("present.clear"), S("{}"), NULL, 0);
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
    REQUIRE(bdk_ui_complete(g_menus.request, S("accept"),
                            S("{\"action\":\"perform\",\"item\":\"openLinkInNewPage\"}"), none,
                            &err) == BDK_OK);
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

/* Inspect opens DevTools through a devtools request carrying the front end as a page
   (ui-requests.md 5.15; until M7 it was a new-window request of kind devTools). */
static void test_engine_inspect_opens_devtools_through_a_devtools_request(void) {
    reset_logs();
    bdk_contents_t c = start_with_page();
    bdk_harness_engine_show(c, 1);
    REQUIRE(bdk_testing_click(c, 20, 20, 2) == BDK_OK);
    for (int i = 0; i < 20 && g_menus.calls == 0; ++i)
        t_pump();
    REQUIRE(g_menus.calls == 1);
    CHECK(strstr(g_menus.payload, "\"id\":\"inspect\"") != NULL);
    g_devtools.complete = "accept";
    bdk_native_handle_t none;
    memset(&none, 0, sizeof none);
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_complete(g_menus.request, S("accept"),
                            S("{\"action\":\"perform\",\"item\":\"inspect\"}"), none,
                            &err) == BDK_OK);
    t_pump();
    CHECK(g_new_windows.calls == 0);
    REQUIRE(g_devtools.calls == 1);
    CHECK(g_devtools.contents == c);
    CHECK(strstr(g_devtools.payload, "\"url\":\"devtools://devtools/bundled/devtools_app.html") !=
          NULL);
    CHECK(strstr(g_devtools.payload, "\"preferredDock\":\"undocked\"") != NULL);
    char handle[32];
    REQUIRE(t_json_string(g_devtools.payload, "frontend", handle, sizeof handle));
    bdk_contents_t front = (bdk_contents_t)strtoull(handle, NULL, 10);
    bdk_harness_engine_show(front, 1);
    bdk_contents_state_t st;
    int loaded = 0;
    for (int i = 0; i < 40 && !loaded; ++i) {
        t_pump();
        memset(&st, 0, sizeof st);
        st.struct_size = sizeof st;
        REQUIRE(bdk_contents_state(front, &st, &err) == BDK_OK);
        loaded = str_has(st.url, "devtools://devtools/bundled/devtools_app.html") && !st.loading;
    }
    CHECK(loaded);
    bdk_harness_engine_show(front, 0);
    CHECK(bdk_contents_destroy(front) == BDK_OK);
    bdk_harness_engine_show(c, 0);
}

/* ---- fullscreen (T-MAC-05) ---- */

static void test_engine_html_fullscreen_enters_and_exits_through_requests(void) {
    reset_logs();
    g_fullscreen.complete = "accept";
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
    CHECK(strstr(g_fullscreen.payload, "\"isEntering\":true") != NULL);
    CHECK(strstr(g_fullscreen.payload, "\"origin\":\"https://bdk.test\"") != NULL);
    run_script(c, "document.exitFullscreen()", 1);
    REQUIRE(wait_for_title(c, "out"));
    CHECK(g_fullscreen.calls == 2);
    CHECK(strstr(g_fullscreen.payload, "\"isEntering\":false") != NULL);
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
    CHECK(strstr(g_fullscreen.payload, "\"isEntering\"") != NULL);
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

/* Chromium's own discard (memory pressure) swaps a WebContents with no renderer into a hidden
   page's tab. The page follows its tab and loads again when it is shown, instead of keeping the
   destroyed WebContents (found in M8: a page left hidden for hours came back blank). */
static void test_engine_a_page_chromium_discards_loads_again_when_shown(void) {
    reset_logs();
    bdk_contents_t c = start_with_page();
    char out[128];
    char args[96];
    snprintf(args, sizeof args, "{\"contents\":\"%llu\"}", (unsigned long long)c);
    REQUIRE(bdk_testing_command(t_runtime, S("tab.chromiumDiscard"), S(args), out, sizeof out) ==
            BDK_OK);
    CHECK(strstr(out, "\"discarded\":true") != NULL);
    t_pump();
    bdk_harness_engine_show(c, 1);
    for (int i = 0; i < 40; ++i)
        t_pump();
    run_script(c, "document.title = 'back'", 0);
    CHECK(wait_for_title(c, "back"));
    bdk_harness_engine_show(c, 0);
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

/* ---- the host view: keys, input methods, accessibility (T-MAC-03) ---- */

static int g_reserved_calls, g_unhandled_calls;
static uint32_t g_unhandled_kind;

static uint8_t reserve_command_k(void* ctx, bdk_native_handle_t key) {
    (void)ctx;
    ++g_reserved_calls;
    return key.kind == BDK_NATIVE_NSEVENT ? 1 : 0;
}
static void note_unhandled(void* ctx, bdk_native_handle_t key) {
    (void)ctx;
    ++g_unhandled_calls;
    g_unhandled_kind = key.kind;
}

/* A shortcut the page does not handle reaches the application's handler with the native event, and
   a key the application reserves never reaches the page. */
static void test_engine_keys_the_page_leaves_reach_the_application(void) {
    reset_logs();
    bdk_contents_t c = start_with_page();
    bdk_harness_engine_show(c, 1);
    REQUIRE(bdk_contents_focus(c) == BDK_OK);
    run_script(c, "document.title='keys:';onkeydown=e=>{document.title+=e.key}", 0);
    REQUIRE(wait_for_title(c, "keys:"));

    g_unhandled_calls = 0;
    bdk_contents_set_keyboard_handler(c, NULL, note_unhandled, NULL, NULL);
    bdk_harness_engine_key(c, 38 /* kVK_ANSI_J */, "j", 1);
    for (int i = 0; i < 20 && g_unhandled_calls == 0; ++i)
        t_pump();
    CHECK(g_unhandled_calls >= 1);
    CHECK(g_unhandled_kind == BDK_NATIVE_NSEVENT);

    g_reserved_calls = 0;
    bdk_contents_set_keyboard_handler(c, reserve_command_k, note_unhandled, NULL, NULL);
    bdk_harness_engine_key(c, 0 /* kVK_ANSI_A */, "a", 0);
    CHECK(g_reserved_calls >= 1);
    t_pump();
    /* The page saw Command-J (it did not handle it) and never the reserved key. */
    CHECK(wait_for_title(c, "keys:j"));
    bdk_harness_engine_show(c, 0);
}

static void test_engine_an_input_method_composes_text_in_the_page(void) {
    reset_logs();
    bdk_contents_t c = start_with_page();
    bdk_harness_engine_show(c, 1);
    run_script(c,
               "document.body.innerHTML='<input id=f>';document.getElementById('f').focus();"
               "document.title='ready'",
               0);
    REQUIRE(wait_for_title(c, "ready"));
    REQUIRE(bdk_contents_focus(c) == BDK_OK);
    for (int i = 0; i < 5; ++i)
        t_pump();
    /* Typed keys reach the focused field in any window. */
    bdk_harness_engine_key(c, 0 /* kVK_ANSI_A */, "a", 0);
    run_script(c, "document.title='typed:'+document.getElementById('f').value", 0);
    CHECK(wait_for_title(c, "typed:a"));
    run_script(c, "document.getElementById('f').value=''", 0);
    /* macOS runs input methods only in the key window, and Chromium engages one only there. A
       process started from a terminal cannot make itself frontmost (cooperative activation), so
       the composition is checked when the harness's window is key, and skipped otherwise. */
    if (!bdk_harness_engine_window_is_key(c)) {
        printf("    (input method skipped: the harness window is not key; run the harness as the "
               "frontmost application)\n");
        bdk_harness_engine_show(c, 0);
        return;
    }
    REQUIRE(bdk_harness_engine_compose(c, "\xe3\x81\xab", "\xe6\x97\xa5\xe6\x9c\xac"));
    run_script(c, "document.title='value:'+document.getElementById('f').value", 0);
    CHECK(wait_for_title(c, "value:\xe6\x97\xa5\xe6\x9c\xac"));
    bdk_harness_engine_show(c, 0);
}

static void test_engine_the_accessibility_tree_exposes_page_content(void) {
    reset_logs();
    bdk_contents_t c = start_with_page();
    bdk_harness_engine_show(c, 1);
    run_script(c, "document.body.innerHTML='<h1>Readable heading</h1><p>Spoken text</p>'", 0);
    char text[2048];
    int web_area = 0, found = 0;
    for (int i = 0; i < 40 && !found; ++i) {
        web_area = bdk_harness_engine_accessibility_text(c, text, sizeof text);
        found = web_area && strstr(text, "Readable heading") && strstr(text, "Spoken text");
        t_pump();
    }
    CHECK(web_area);
    CHECK(found);
    bdk_harness_engine_show(c, 0);
}

/* ---- host contexts (T-ADP-01 to T-ADP-03) ---- */

static bdk_testing_tab_info_t tab_info_of(bdk_contents_t c) {
    bdk_testing_tab_info_t info;
    memset(&info, 0, sizeof info);
    info.struct_size = sizeof info;
    REQUIRE(bdk_testing_tab_info(c, &info) == BDK_OK);
    return info;
}

/* A runtime with the engine's UI handlers, and two pages of one profile that have loaded the
   opener page. */
static void start_with_two_pages(bdk_contents_t* a, bdk_contents_t* b) {
    reset_logs();
    *a = start_with_page();
    bdk_error_t err = 0;
    *b = t_new_contents(g_page_profile);
    REQUIRE(bdk_contents_load(*b, S(OPENER), &err) == BDK_OK);
    t_pump();
}

static void test_engine_a_new_page_is_a_tab_of_a_parking_context(void) {
    reset_logs();
    t_start_frozen();
    size_t windows = bdk_harness_engine_app_window_count();
    bdk_profile_t profile = t_new_profile("Parked");
    bdk_contents_t first = t_new_contents(profile);
    t_pump();
    bdk_testing_tab_info_t info = tab_info_of(first);
    CHECK(info.is_tab);
    CHECK(info.parking);
    CHECK(!info.has_native_window);
    CHECK(info.tab_id > 0);
    CHECK(info.context_id > 0);
    CHECK(info.tab_count == 1);
    CHECK(bdk_testing_host_context_count() == 1);
    /* No Chrome window of its own is shown, and the application gained no window. */
    CHECK(bdk_testing_chrome_window_count() == 0);
    CHECK(bdk_harness_engine_app_window_count() == windows);
    /* A second page of the profile is a second tab of the same context; a page of another profile
       has a parking context of its own. */
    bdk_contents_t second = t_new_contents(profile);
    bdk_contents_t elsewhere = t_new_contents(t_new_profile("Elsewhere"));
    bdk_testing_tab_info_t a = tab_info_of(first), b = tab_info_of(second),
                           e = tab_info_of(elsewhere);
    CHECK(a.parking && b.parking && e.parking);
    CHECK(a.context_id == b.context_id);
    CHECK(a.tab_id != b.tab_id);
    CHECK(a.tab_count == 2);
    CHECK(e.context_id != a.context_id);
    CHECK(bdk_testing_host_context_count() == 2);
    CHECK(bdk_testing_chrome_window_count() == 0);
}

static void test_engine_attaching_a_page_to_a_registered_window_moves_it_into_its_context(void) {
    reset_logs();
    bdk_contents_t c = start_with_page();
    bdk_testing_tab_info_t parked = tab_info_of(c);
    REQUIRE(parked.parking);
    int window = bdk_harness_engine_window_open(t_runtime);
    REQUIRE(window >= 0);
    bdk_harness_engine_window_attach(window, c);
    bdk_testing_tab_info_t placed = tab_info_of(c);
    CHECK(placed.is_tab);
    CHECK(!placed.parking);
    CHECK(placed.has_native_window);
    CHECK(placed.context_id != parked.context_id);
    CHECK(placed.tab_id == parked.tab_id);
    CHECK(placed.tab_count == 1);
    /* The move did not reload the page. */
    char title[256];
    title_of(c, title, sizeof title);
    CHECK(strcmp(title, OPENER) == 0);
    CHECK(bdk_testing_chrome_window_count() == 0);
    bdk_harness_engine_window_close(window);
}

static void test_engine_moving_a_page_between_windows_keeps_its_tab_history_and_renderer(void) {
    reset_logs();
    bdk_contents_t c = start_with_page();
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_load(c, S("https://bdk.test/second"), &err) == BDK_OK);
    t_pump();
    run_script(c, "window.bdkMarker = 42", 0);
    bdk_testing_tab_info_t parked = tab_info_of(c);
    int first = bdk_harness_engine_window_open(t_runtime);
    int second = bdk_harness_engine_window_open(t_runtime);
    REQUIRE(first >= 0 && second >= 0);
    bdk_harness_engine_window_attach(first, c);
    bdk_testing_tab_info_t in_first = tab_info_of(c);
    bdk_harness_engine_window_attach(second, c);
    bdk_testing_tab_info_t in_second = tab_info_of(c);
    CHECK(in_first.is_tab && !in_first.parking);
    CHECK(in_second.is_tab && !in_second.parking);
    CHECK(in_first.context_id != in_second.context_id);
    CHECK(in_first.context_id != parked.context_id);
    CHECK(in_first.tab_id == parked.tab_id);
    CHECK(in_second.tab_id == parked.tab_id);
    CHECK(in_second.tab_count == 1);
    /* History and the page's own state came along: the renderer was never replaced. */
    bdk_buffer_t entries = 0;
    REQUIRE(bdk_contents_entries(c, &entries, &err) == BDK_OK);
    char text[512];
    t_buffer_text(entries, text, sizeof text);
    CHECK(strstr(text, "\"current\":1") != NULL);
    CHECK(strstr(text, OPENER) != NULL);
    bdk_release(entries);
    run_script(c, "document.title = 'marker:' + window.bdkMarker", 0);
    CHECK(wait_for_title(c, "marker:42"));
    /* And back again. */
    bdk_harness_engine_window_attach(first, c);
    bdk_testing_tab_info_t back = tab_info_of(c);
    CHECK(back.context_id == in_first.context_id);
    CHECK(back.tab_id == parked.tab_id);
    CHECK(bdk_testing_chrome_window_count() == 0);
    bdk_harness_engine_window_close(first);
    bdk_harness_engine_window_close(second);
}

static void test_engine_a_detached_page_stays_a_background_tab_of_its_last_window(void) {
    bdk_contents_t a, b;
    start_with_two_pages(&a, &b);
    int window = bdk_harness_engine_window_open(t_runtime);
    REQUIRE(window >= 0);
    bdk_harness_engine_window_attach(window, a);
    bdk_testing_tab_info_t first = tab_info_of(a);
    bdk_harness_engine_window_detach(a);
    bdk_testing_tab_info_t detached = tab_info_of(a);
    /* No view in any window: the page is still a tab of the window it was last in. */
    CHECK(detached.is_tab);
    CHECK(!detached.parking);
    CHECK(detached.context_id == first.context_id);
    CHECK(detached.tab_id == first.tab_id);
    CHECK(bdk_testing_chrome_window_count() == 0);
    bdk_harness_engine_window_close(window);
}

static void test_engine_closing_a_window_parks_its_pages(void) {
    bdk_contents_t a, b;
    start_with_two_pages(&a, &b);
    int window = bdk_harness_engine_window_open(t_runtime);
    REQUIRE(window >= 0);
    bdk_harness_engine_window_attach(window, a);
    bdk_testing_tab_info_t placed = tab_info_of(a);
    REQUIRE(!placed.parking);
    size_t contexts = bdk_testing_host_context_count();
    bdk_harness_engine_window_close(window);
    bdk_testing_tab_info_t parked = tab_info_of(a);
    CHECK(parked.is_tab);
    CHECK(parked.parking);
    CHECK(parked.tab_id == placed.tab_id);
    /* The window's context closed; the page's own state is intact. */
    CHECK(bdk_testing_host_context_count() == contexts - 1);
    char title[256];
    title_of(a, title, sizeof title);
    CHECK(strcmp(title, OPENER) == 0);
    CHECK(bdk_testing_chrome_window_count() == 0);
}

static void test_engine_focusing_a_page_activates_its_tab_and_destroying_it_closes_the_tab(void) {
    bdk_contents_t a, b;
    start_with_two_pages(&a, &b);
    int window = bdk_harness_engine_window_open(t_runtime);
    REQUIRE(window >= 0);
    bdk_harness_engine_window_attach(window, a);
    bdk_harness_engine_window_attach(window, b);
    CHECK(tab_info_of(a).context_id == tab_info_of(b).context_id);
    CHECK(tab_info_of(a).tab_count == 2);
    REQUIRE(bdk_contents_focus(b) == BDK_OK);
    t_pump();
    CHECK(tab_info_of(b).active);
    CHECK(!tab_info_of(a).active);
    REQUIRE(bdk_contents_focus(a) == BDK_OK);
    t_pump();
    CHECK(tab_info_of(a).active);
    CHECK(!tab_info_of(b).active);
    /* Destroying a page closes its tab. */
    REQUIRE(bdk_contents_destroy(a) == BDK_OK);
    t_pump();
    CHECK(tab_info_of(b).tab_count == 1);
    CHECK(bdk_testing_chrome_window_count() == 0);
    bdk_harness_engine_window_close(window);
}

/* ---- codecs (decision D7, T-CHR-12) ---- */

/* The runtime is built with Chromium's open codecs only: no bundled H.264, AAC or HEVC decoder
   (D7 allows those only through the system's decoders, which T-CHR-12 records as not yet wired).
   This pins what pages are told, so a build that starts bundling a proprietary decoder fails. */
static void test_engine_only_open_codecs_are_offered(void) {
    reset_logs();
    bdk_contents_t c = start_with_page();
    run_script(
        c,
        "const v=document.createElement('video'),a=document.createElement('audio');"
        "const p=t=>v.canPlayType(t)||'no';"
        "document.title=['vp9:'+p('video/webm; codecs=\"vp9\"'),"
        "'av1:'+p('video/mp4; codecs=\"av01.0.05M.08\"'),"
        "'opus:'+(a.canPlayType('audio/webm; codecs=\"opus\"')||'no'),"
        "'h264:'+p('video/mp4; codecs=\"avc1.42E01E\"'),"
        "'aac:'+(a.canPlayType('audio/mp4; codecs=\"mp4a.40.2\"')||'no'),"
        "'hevc:'+p('video/mp4; codecs=\"hvc1.1.6.L93.B0\"'),"
        "'mse-h264:'+MediaSource.isTypeSupported('video/mp4; codecs=\"avc1.42E01E\"')].join(' ')",
        0);
    char title[256];
    for (int i = 0; i < 20; ++i) {
        title_of(c, title, sizeof title);
        if (strstr(title, "vp9:"))
            break;
        t_pump();
    }
    printf("    codecs: %s\n", title);
    CHECK(strstr(title, "vp9:probably") != NULL);
    CHECK(strstr(title, "opus:probably") != NULL);
    CHECK(strstr(title, "h264:no") != NULL);
    CHECK(strstr(title, "aac:no") != NULL);
    CHECK(strstr(title, "hevc:no") != NULL);
    CHECK(strstr(title, "mse-h264:false") != NULL);
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

/* A profile the application closes is unloaded by Chromium, with its databases and the descriptors
   they hold; one that is opened again afterwards loads from the same files. */
int bdk_harness_engine_open_descriptors(void);

static void test_engine_a_closed_profile_releases_its_descriptors(void) {
    t_start_frozen();
    char id[64];
    t_create_profile("Reopened", id, sizeof id);
    enum { kWarmup = 2, kRounds = 6 };
    int before = 0;
    for (int i = 0; i < kWarmup + kRounds; ++i) {
        char fresh[64], name[32];
        snprintf(name, sizeof name, "Opened once %d", i);
        t_create_profile(name, fresh, sizeof fresh);
        bdk_profile_t p = t_open_profile(fresh);
        bdk_contents_t c = t_new_contents(p);
        t_pump();
        REQUIRE(bdk_contents_destroy(c) == BDK_OK);
        REQUIRE(bdk_profile_close(p, NULL) == BDK_OK);
        t_pump();
        if (i == kWarmup - 1)
            before = bdk_harness_engine_open_descriptors();
    }
    /* A leaked profile held about eighty descriptors. */
    CHECK(bdk_harness_engine_open_descriptors() - before < 40);
    /* The same profile, closed and opened at once, is one instance again. */
    for (int i = 0; i < 3; ++i) {
        bdk_profile_t p = t_open_profile(id);
        bdk_contents_t c = t_new_contents(p);
        t_pump();
        REQUIRE(bdk_contents_destroy(c) == BDK_OK);
        REQUIRE(bdk_profile_close(p, NULL) == BDK_OK);
    }
}

/* ---- built-in services (T-SVC-02, T-SVC-03, T-SVC-06) ---- */

int bdk_harness_engine_download_server_start(void);
void bdk_harness_engine_download_server_release(void);
void bdk_harness_engine_download_server_stop(void);

/* One built-in command by the host, run to its end. Chromium's services answer from other threads,
   so this pumps until the result arrives. */
static void svc(const char* command, const char* args, bdk_profile_t profile, t_capture_t* cap) {
    bdk_invoke_options_t o;
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.profile = profile;
    t_cap_init(cap);
    REQUIRE(bdk_invoke_named(t_runtime, S(command), S(args), &o, t_on_result, t_on_progress, cap,
                             t_on_release) != 0);
    for (int i = 0; i < 1000 && cap->results == 0; ++i) {
        t_pump();
        bdk_testing_run_for(0.01);
    }
    CHECK(cap->results == 1);
}

static int failed_with(const t_capture_t* cap, const char* domain, const char* code) {
    return cap->has_error && strcmp(cap->domain, domain) == 0 && strcmp(cap->code, code) == 0;
}

static void command(const char* name, const char* args, char* out, size_t n) {
    REQUIRE(bdk_testing_command(t_runtime, S(name), S(args), out, n) == BDK_OK);
}

/* The member `key` of a one-member JSON object the test hook wrote, without the braces and, for a
   string, without its quotes. */
static void value_of_member(const char* json, const char* key, char* out, size_t n) {
    char pattern[32];
    snprintf(pattern, sizeof pattern, "\"%s\":", key);
    const char* at = strstr(json, pattern);
    REQUIRE(at != NULL);
    at += strlen(pattern);
    size_t len = strlen(at);
    while (len > 0 && at[len - 1] == '}')
        --len;
    if (len >= 2 && at[0] == '"' && at[len - 1] == '"') {
        ++at;
        len -= 2;
    }
    REQUIRE(len < n);
    memcpy(out, at, len);
    out[len] = '\0';
}

static void value_of(const char* json, char* out, size_t n) {
    value_of_member(json, "value", out, n);
}

/* Chromium's heavy-ad intervention keeps an opt-out store per profile. BDK keeps it in memory, as
   Chrome does for an off-the-record profile: on disk, its database is loaded by a best-effort task,
   and a load for a profile deleted before that task ran fails a fatal check in the SQL layer. */
static void test_engine_the_heavy_ad_opt_out_store_stays_in_memory(void) {
    t_start_frozen();
    char json[64];
    command("heavy_ad.store", "{}", json, sizeof json);
    CHECK(strcmp(json, "{\"store\":\"memory\"}") == 0);
}

/* T-ADP-10: Chromium's after-start-up work (AfterStartupTaskUtils, best-effort tasks) runs once
   the runtime has started, not after Chromium's three-minute failsafe: a BDK application opens no
   Chromium window whose tab would end start-up. Run alone, this is the process's first start. */
static void test_engine_after_startup_work_runs_once_the_runtime_has_started(void) {
    t_start_frozen();
    char json[64];
    command("startup.post", "{}", NULL, 0);
    const time_t until = time(NULL) + 10;
    do {
        t_pump();
        command("startup.state", "{}", json, sizeof json);
    } while (!strstr(json, "\"ran\":true") && time(NULL) < until);
    CHECK(strcmp(json, "{\"complete\":true,\"ran\":true}") == 0);
}

/* Reads a preference of the page's profile as Chromium holds it. */
static void pref_of(bdk_contents_t page, const char* name, char* out, size_t n) {
    char args[256], json[512];
    snprintf(args, sizeof args, "{\"contents\":\"%llu\",\"name\":\"%s\"}", (unsigned long long)page,
             name);
    command("pref.json", args, json, sizeof json);
    value_of(json, out, n);
}

static void content_setting_of(bdk_contents_t page, const char* url, const char* kind, char* out,
                               size_t n) {
    char args[256], json[128];
    snprintf(args, sizeof args, "{\"contents\":\"%llu\",\"origin\":\"%s\",\"kind\":\"%s\"}",
             (unsigned long long)page, url, kind);
    command("permission.get", args, json, sizeof json);
    value_of(json, out, n);
}

static void test_engine_history_is_searched_paginated_and_cleared(void) {
    t_start_frozen();
    bdk_profile_t p = t_new_profile("History");
    t_capture_t cap;
    const char* urls[] = {"https://a.example/", "https://b.example/", "https://c.example/"};
    for (int i = 0; i < 3; ++i) {
        char args[128];
        snprintf(args, sizeof args, "{\"url\":\"%s\",\"title\":\"page %d\"}", urls[i], i);
        svc("history.add", args, p, &cap);
        CHECK(!cap.has_error);
    }
    svc("history.search", "{\"limit\":2}", p, &cap);
    CHECK(!cap.has_error && strstr(cap.json, "c.example") && strstr(cap.json, "b.example"));
    CHECK(strstr(cap.json, "a.example") == NULL);
    char cursor[64], next[128];
    REQUIRE(t_json_string(cap.json, "nextCursor", cursor, sizeof cursor));
    snprintf(next, sizeof next, "{\"limit\":2,\"cursor\":\"%s\"}", cursor);
    svc("history.search", next, p, &cap);
    CHECK(strstr(cap.json, "a.example") && strstr(cap.json, "\"nextCursor\":null"));
    svc("history.search", "{\"cursor\":\"nonsense\"}", p, &cap);
    CHECK(failed_with(&cap, "broker", "invalidArguments"));
    svc("history.search", "{\"text\":\"page 1\"}", p, &cap);
    CHECK(strstr(cap.json, "b.example") && !strstr(cap.json, "c.example"));
    svc("history.visits", "{\"url\":\"https://a.example/\"}", p, &cap);
    CHECK(strstr(cap.json, "\"transition\""));
    svc("history.deleteUrls", "{\"urls\":[\"https://b.example/\"]}", p, &cap);
    CHECK(!cap.has_error);
    svc("history.search", "{}", p, &cap);
    CHECK(!strstr(cap.json, "b.example") && strstr(cap.json, "a.example"));
    svc("privacy.clear", "{\"types\":[\"history\"]}", p, &cap);
    CHECK(!cap.has_error);
    svc("history.search", "{}", p, &cap);
    CHECK(strstr(cap.json, "\"items\":[]") != NULL);
}

static void test_engine_bookmarks_nest_move_and_remove(void) {
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Bookmarks");
    t_capture_t cap;
    svc("bookmarks.roots", "{}", p, &cap);
    CHECK(!cap.has_error);
    char bar[32], other[32];
    REQUIRE(t_json_string(cap.json, "id", bar, sizeof bar));
    const char* second = strstr(cap.json, "},{");
    REQUIRE(second != NULL);
    REQUIRE(t_json_string(second, "id", other, sizeof other));
    char args[256], a[32], b[32], c[32], mark[32];
    snprintf(args, sizeof args, "{\"parentId\":\"%s\",\"title\":\"A\"}", bar);
    svc("bookmarks.createFolder", args, p, &cap);
    REQUIRE(t_json_string(cap.json, "id", a, sizeof a));
    snprintf(args, sizeof args, "{\"parentId\":\"%s\",\"title\":\"B\"}", a);
    svc("bookmarks.createFolder", args, p, &cap);
    REQUIRE(t_json_string(cap.json, "id", b, sizeof b));
    snprintf(args, sizeof args,
             "{\"parentId\":\"%s\",\"title\":\"Example\",\"url\":\"https://bm.example/\"}", b);
    svc("bookmarks.create", args, p, &cap);
    CHECK(!cap.has_error && strstr(cap.json, "\"isFolder\":false"));
    REQUIRE(t_json_string(cap.json, "id", mark, sizeof mark));
    snprintf(args, sizeof args, "{\"id\":\"%s\"}", a);
    svc("bookmarks.subtree", args, p, &cap);
    CHECK(strstr(cap.json, "\"title\":\"B\"") != NULL);
    svc("bookmarks.forUrl", "{\"url\":\"https://bm.example/\"}", p, &cap);
    CHECK(strstr(cap.json, "Example") != NULL);
    svc("bookmarks.search", "{\"query\":\"Example\"}", p, &cap);
    CHECK(strstr(cap.json, "bm.example") != NULL);
    /* Move B, with its bookmark, to the other bookmarks. */
    snprintf(args, sizeof args, "{\"id\":\"%s\",\"parentId\":\"%s\"}", b, other);
    svc("bookmarks.move", args, p, &cap);
    CHECK(!cap.has_error && strstr(cap.json, other) != NULL);
    snprintf(args, sizeof args, "{\"id\":\"%s\"}", other);
    svc("bookmarks.children", args, p, &cap);
    CHECK(strstr(cap.json, "\"title\":\"B\"") != NULL);
    /* A folder cannot move into its own descendant. */
    snprintf(args, sizeof args, "{\"parentId\":\"%s\",\"title\":\"C\"}", a);
    svc("bookmarks.createFolder", args, p, &cap);
    REQUIRE(t_json_string(cap.json, "id", c, sizeof c));
    snprintf(args, sizeof args, "{\"id\":\"%s\",\"parentId\":\"%s\"}", a, c);
    svc("bookmarks.move", args, p, &cap);
    CHECK(failed_with(&cap, "broker", "invalidArguments"));
    snprintf(args, sizeof args, "{\"id\":\"%s\",\"title\":\"Renamed\"}", mark);
    svc("bookmarks.update", args, p, &cap);
    CHECK(strstr(cap.json, "Renamed") != NULL);
    /* Removing a folder removes what is in it. */
    snprintf(args, sizeof args, "{\"id\":\"%s\"}", b);
    svc("bookmarks.remove", args, p, &cap);
    CHECK(!cap.has_error);
    svc("bookmarks.forUrl", "{\"url\":\"https://bm.example/\"}", p, &cap);
    CHECK(strstr(cap.json, "\"items\":[]") != NULL);
    snprintf(args, sizeof args, "{\"id\":\"%s\"}", other);
    svc("bookmarks.remove", args, p, &cap);
    CHECK(failed_with(&cap, "broker", "invalidArguments"));
}

#define BOOKMARKS_FIXTURE                                                                          \
    "<!DOCTYPE NETSCAPE-Bookmark-file-1>\n"                                                        \
    "<META HTTP-EQUIV=\"Content-Type\" CONTENT=\"text/html; charset=UTF-8\">\n"                    \
    "<TITLE>Bookmarks</TITLE>\n<H1>Bookmarks</H1>\n<DL><p>\n"                                      \
    "<DT><H3>Fixtures</H3>\n<DL><p>\n"                                                             \
    "<DT><A HREF=\"https://import-one.example/\">Import One</A>\n"                                 \
    "<DT><A HREF=\"https://import-two.example/page\">Import Two</A>\n"                             \
    "</DL><p>\n<DT><A HREF=\"https://import-top.example/\">Import Top</A>\n</DL><p>\n"

static void write_text_file(const char* path, const char* text) {
    FILE* f = fopen(path, "w");
    REQUIRE(f != NULL);
    fputs(text, f);
    fclose(f);
}

static void test_engine_bookmarks_are_imported_and_exported_as_html(void) {
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Bookmark files");
    bdk_profile_t other = t_new_profile("Bookmark files other");
    char dir[] = "/tmp/bdk-engine-bookmarks-XXXXXX";
    REQUIRE(mkdtemp(dir) != NULL);
    char in[96], out[96], args[256];
    snprintf(in, sizeof in, "%s/in.html", dir);
    snprintf(out, sizeof out, "%s/out.html", dir);
    write_text_file(in, BOOKMARKS_FIXTURE);

    t_capture_t cap;
    snprintf(args, sizeof args, "{\"path\":\"%s\"}", in);
    svc("bookmarks.import", args, p, &cap);
    CHECK(!cap.has_error);
    CHECK(strstr(cap.json, "\"imported\":3") != NULL);
    CHECK(cap.progress >= 1);

    /* The bookmarks landed in a folder of the other bookmarks, keeping the file's folder. */
    char parent[32], folder[32];
    svc("bookmarks.search", "{\"query\":\"Import One\"}", p, &cap);
    REQUIRE(t_json_string(cap.json, "parentId", parent, sizeof parent));
    snprintf(args, sizeof args, "{\"id\":\"%s\"}", parent);
    svc("bookmarks.get", args, p, &cap);
    CHECK(strstr(cap.json, "\"title\":\"Fixtures\"") != NULL);
    REQUIRE(t_json_string(cap.json, "parentId", folder, sizeof folder));
    snprintf(args, sizeof args, "{\"id\":\"%s\"}", folder);
    svc("bookmarks.get", args, p, &cap);
    CHECK(strstr(cap.json, "\"title\":\"Imported bookmarks\"") != NULL);
    svc("bookmarks.forUrl", "{\"url\":\"https://import-top.example/\"}", p, &cap);
    CHECK(strstr(cap.json, "Import Top") != NULL);

    /* Another profile has none of them. */
    svc("bookmarks.search", "{\"query\":\"Import\"}", other, &cap);
    CHECK(strstr(cap.json, "\"items\":[]") != NULL);

    /* What cannot be read as bookmarks is refused. */
    snprintf(args, sizeof args, "{\"path\":\"%s/missing.html\"}", dir);
    svc("bookmarks.import", args, p, &cap);
    CHECK(failed_with(&cap, "broker", "invalidArguments"));

    /* An export reads back as the same bookmarks. */
    snprintf(args, sizeof args, "{\"path\":\"%s\"}", out);
    svc("bookmarks.export", args, p, &cap);
    CHECK(!cap.has_error);
    char text[4096] = "";
    FILE* f = fopen(out, "r");
    REQUIRE(f != NULL);
    size_t n = fread(text, 1, sizeof text - 1, f);
    text[n] = 0;
    fclose(f);
    CHECK(strstr(text, "NETSCAPE-Bookmark-file-1") != NULL);
    CHECK(strstr(text, "https://import-one.example/") != NULL);
    CHECK(strstr(text, "https://import-two.example/page") != NULL);
    CHECK(strstr(text, "Import Top") != NULL);
    CHECK(strstr(text, "Fixtures") != NULL);

    /* Round trip: the export imports into the other profile with the same count. */
    snprintf(args, sizeof args, "{\"path\":\"%s\"}", out);
    svc("bookmarks.import", args, other, &cap);
    CHECK(!cap.has_error && strstr(cap.json, "\"imported\":3") != NULL);
    unlink(in);
    unlink(out);
    rmdir(dir);
}

/* T-SVC-12: closing a profile while its bookmarks are being exported. Chromium writes the file
   from a best-effort task that answers later; the export keeps the profile loaded until then, so
   nothing still points at a profile that is gone (Chromium's dangling-pointer check aborts the
   engine otherwise). The call ends once, and the profile is unloaded after it. */
static void test_engine_closing_a_profile_during_a_bookmarks_export(void) {
    t_start_frozen();
    char id[64];
    t_create_profile("Export while closing", id, sizeof id);
    bdk_profile_t p = t_open_profile(id);
    char dir[] = "/tmp/bdk-engine-bookmarks-XXXXXX";
    REQUIRE(mkdtemp(dir) != NULL);
    char in[96], out[96], args[256];
    snprintf(in, sizeof in, "%s/in.html", dir);
    snprintf(out, sizeof out, "%s/out.html", dir);
    write_text_file(in, BOOKMARKS_FIXTURE);
    t_capture_t cap;
    snprintf(args, sizeof args, "{\"path\":\"%s\"}", in);
    svc("bookmarks.import", args, p, &cap);
    CHECK(!cap.has_error);

    bdk_invoke_options_t o;
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.profile = p;
    t_cap_init(&cap);
    snprintf(args, sizeof args, "{\"path\":\"%s\"}", out);
    REQUIRE(bdk_invoke_named(t_runtime, S("bookmarks.export"), S(args), &o, t_on_result,
                             t_on_progress, &cap, t_on_release) != 0);
    REQUIRE(bdk_profile_close(p, NULL) == BDK_OK);
    for (int i = 0; i < 1000 && cap.results == 0; ++i) {
        t_pump();
        bdk_testing_run_for(0.01);
    }
    CHECK(cap.results == 1);
    /* The writer's answer and the profile's unload come after: still alive here. */
    for (int i = 0; i < 300; ++i) {
        t_pump();
        bdk_testing_run_for(0.01);
    }
    CHECK(cap.results == 1);
    /* The profile opens again, as one instance. */
    bdk_profile_t again = t_open_profile(id);
    REQUIRE(bdk_profile_close(again, NULL) == BDK_OK);
    unlink(in);
    unlink(out);
    rmdir(dir);
}

/* The first time the item's state is `state`, within a time limit. */
static int wait_for_download_state(bdk_profile_t p, const char* id, const char* state) {
    char args[64], want[48];
    snprintf(args, sizeof args, "{\"id\":\"%s\"}", id);
    snprintf(want, sizeof want, "\"state\":\"%s\"", state);
    for (int i = 0; i < 300; ++i) {
        t_capture_t cap;
        svc("downloads.get", args, p, &cap);
        if (strstr(cap.json, want))
            return 1;
        bdk_testing_run_for(0.05);
    }
    return 0;
}

static void test_engine_a_download_completes_and_is_controllable(void) {
    int port = bdk_harness_engine_download_server_start();
    REQUIRE(port != 0);
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Downloads");
    char dir[] = "/tmp/bdk-engine-downloads-XXXXXX";
    REQUIRE(mkdtemp(dir) != NULL);
    t_capture_t cap;
    char args[256], url[96], first[32], second[32];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/big.bin", port);
    snprintf(args, sizeof args, "{\"url\":\"%s\",\"path\":\"%s/one.bin\"}", url, dir);
    svc("downloads.start", args, p, &cap);
    REQUIRE(!cap.has_error);
    REQUIRE(t_json_string(cap.json, "id", first, sizeof first));
    snprintf(args, sizeof args, "{\"url\":\"%s\",\"path\":\"%s/two.bin\"}", url, dir);
    svc("downloads.start", args, p, &cap);
    REQUIRE(!cap.has_error);
    REQUIRE(t_json_string(cap.json, "id", second, sizeof second));
    /* The first is paused and resumed. */
    snprintf(args, sizeof args, "{\"id\":\"%s\"}", first);
    svc("downloads.pause", args, p, &cap);
    CHECK(!cap.has_error);
    CHECK(wait_for_download_state(p, first, "paused"));
    svc("downloads.resume", args, p, &cap);
    CHECK(!cap.has_error);
    CHECK(wait_for_download_state(p, first, "inProgress"));
    /* The second is cancelled. */
    char second_args[64];
    snprintf(second_args, sizeof second_args, "{\"id\":\"%s\"}", second);
    svc("downloads.cancel", second_args, p, &cap);
    CHECK(!cap.has_error);
    CHECK(wait_for_download_state(p, second, "cancelled"));
    svc("downloads.list", "{\"state\":\"inProgress\"}", p, &cap);
    CHECK(strstr(cap.json, url) != NULL);
    /* The server lets the first finish. */
    bdk_harness_engine_download_server_release();
    CHECK(wait_for_download_state(p, first, "complete"));
    svc("downloads.get", args, p, &cap);
    CHECK(strstr(cap.json, "one.bin") != NULL && strstr(cap.json, "\"receivedBytes\":200000"));
    svc("downloads.remove", args, p, &cap);
    CHECK(!cap.has_error);
    svc("downloads.get", args, p, &cap);
    CHECK(failed_with(&cap, "broker", "invalidArguments"));
    svc("downloads.clear", "{}", p, &cap);
    CHECK(!cap.has_error);
    svc("downloads.list", "{}", p, &cap);
    CHECK(strstr(cap.json, "\"items\":[]") != NULL);
    bdk_harness_engine_download_server_stop();
}

/* The profile Chromium loads itself at startup (the last used one) has its download manager before
   BDK puts its delegate in, and Chromium gave the first free download id to the delegate it
   replaced: every download in that profile waited forever, a store install among them (found in
   M8). The hook puts BDK's delegate in again once the manager exists, as at startup. */
static void test_engine_a_download_completes_in_a_profile_whose_manager_came_first(void) {
    int port = bdk_harness_engine_download_server_start();
    REQUIRE(port != 0);
    bdk_harness_engine_download_server_release();
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Manager first");
    bdk_contents_t c = t_new_contents(p);
    char args[256], url[96], id[32];
    snprintf(args, sizeof args, "{\"contents\":\"%llu\"}", (unsigned long long)c);
    command("downloads.replaceDelegate", args, NULL, 0);
    char dir[] = "/tmp/bdk-engine-downloads-XXXXXX";
    REQUIRE(mkdtemp(dir) != NULL);
    snprintf(url, sizeof url, "http://127.0.0.1:%d/big.bin", port);
    snprintf(args, sizeof args, "{\"url\":\"%s\",\"path\":\"%s/first.bin\"}", url, dir);
    t_capture_t cap;
    svc("downloads.start", args, p, &cap);
    REQUIRE(!cap.has_error);
    REQUIRE(t_json_string(cap.json, "id", id, sizeof id));
    CHECK(wait_for_download_state(p, id, "complete"));
    snprintf(args, sizeof args, "%s/first.bin", dir);
    CHECK(unlink(args) == 0);
    rmdir(dir);
    bdk_harness_engine_download_server_stop();
}

/* Waits for Chromium to record a visit to the page's URL, as the helper of each page does after a
   commit. */
static int history_has(bdk_profile_t p, const char* text) {
    for (int i = 0; i < 100; ++i) {
        t_capture_t cap;
        svc("history.search", "{}", p, &cap);
        if (strstr(cap.json, text))
            return 1;
        bdk_testing_run_for(0.05);
    }
    return 0;
}

static void test_engine_profiles_keep_their_data_apart_and_private_ones_record_no_history(void) {
    t_start_frozen();
    bdk_profile_t a = t_new_profile("Apart A");
    bdk_profile_t b = t_new_profile("Apart B");
    bdk_profile_t priv = 0;
    bdk_error_t err = 0;
    REQUIRE(bdk_profile_open_private(t_runtime, &priv, &err) == BDK_OK);
    t_capture_t cap;
    bdk_contents_t in_a = t_new_contents(a);
    bdk_contents_t in_priv = t_new_contents(priv);
    REQUIRE(bdk_contents_load(in_a, S("https://visited.test/in-a"), &err) == BDK_OK);
    REQUIRE(bdk_contents_load(in_priv, S("https://visited.test/in-private"), &err) == BDK_OK);
    CHECK(history_has(a, "visited.test/in-a"));
    svc("history.search", "{}", b, &cap);
    CHECK(strstr(cap.json, "visited.test") == NULL);
    svc("history.search", "{}", priv, &cap);
    CHECK(strstr(cap.json, "in-private") == NULL);
    svc("history.add", "{\"url\":\"https://added.test/private\"}", priv, &cap);
    CHECK(!cap.has_error);
    svc("history.search", "{}", priv, &cap);
    CHECK(strstr(cap.json, "added.test") == NULL);
    /* Settings and site permissions are per profile. */
    svc("settings.set", "{\"key\":\"appearance.fontSize\",\"value\":21}", a, &cap);
    CHECK(!cap.has_error);
    svc("settings.get", "{\"key\":\"appearance.fontSize\"}", b, &cap);
    CHECK(strstr(cap.json, "\"value\":21") == NULL);
    svc("permissions.set",
        "{\"origin\":\"https://x.example\",\"kind\":\"camera\","
        "\"value\":\"allow\"}",
        a, &cap);
    CHECK(!cap.has_error);
    svc("permissions.get", "{\"origin\":\"https://x.example\",\"kind\":\"camera\"}", priv, &cap);
    CHECK(strstr(cap.json, "\"source\":\"default\"") != NULL);
    svc("profiles.current", "{}", priv, &cap);
    CHECK(strstr(cap.json, "\"isPrivate\":true") != NULL);
}

static void test_engine_settings_and_permissions_round_trip_to_chromium(void) {
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Settings");
    bdk_contents_t page = t_new_contents(p);
    t_capture_t cap;
    char value[128];
    svc("settings.set", "{\"key\":\"appearance.fontSize\",\"value\":23}", p, &cap);
    CHECK(!cap.has_error);
    svc("settings.get", "{\"key\":\"appearance.fontSize\"}", p, &cap);
    CHECK(strstr(cap.json, "\"value\":23") != NULL);
    pref_of(page, "webkit.webprefs.default_font_size", value, sizeof value);
    CHECK(strcmp(value, "23") == 0);
    svc("settings.describe", "{\"key\":\"appearance.fontSize\"}", p, &cap);
    CHECK(strstr(cap.json, "\"source\":\"user\"") != NULL);
    svc("settings.reset", "{\"key\":\"appearance.fontSize\"}", p, &cap);
    pref_of(page, "webkit.webprefs.default_font_size", value, sizeof value);
    CHECK(strcmp(value, "23") != 0);
    svc("settings.set", "{\"key\":\"privacy.cookies.thirdParty\",\"value\":\"block\"}", p, &cap);
    pref_of(page, "profile.cookie_controls_mode", value, sizeof value);
    CHECK(strcmp(value, "1") == 0);
    svc("settings.set", "{\"key\":\"privacy.cookies.thirdParty\",\"value\":\"sideways\"}", p, &cap);
    CHECK(failed_with(&cap, "broker", "invalidArguments"));
    svc("settings.set", "{\"key\":\"no.such.key\",\"value\":1}", p, &cap);
    CHECK(failed_with(&cap, "broker", "invalidArguments"));
    svc("settings.get", "{\"key\":\"no.such.key\"}", p, &cap);
    CHECK(strstr(cap.json, "{\"value\":null}") != NULL);
    /* Site permissions. */
    char setting[16];
    svc("permissions.set",
        "{\"origin\":\"https://perm.example\",\"kind\":\"geolocation\","
        "\"value\":\"allow\"}",
        p, &cap);
    CHECK(!cap.has_error);
    content_setting_of(page, "https://perm.example/", "geolocation", setting, sizeof setting);
    CHECK(strcmp(setting, "allow") == 0);
    svc("permissions.get", "{\"origin\":\"https://perm.example\",\"kind\":\"geolocation\"}", p,
        &cap);
    CHECK(strstr(cap.json, "\"value\":\"allow\"") && strstr(cap.json, "\"source\":\"user\""));
    svc("permissions.exceptions", "{\"kind\":\"geolocation\"}", p, &cap);
    CHECK(strstr(cap.json, "perm.example") != NULL);
    svc("permissions.set",
        "{\"origin\":\"https://perm.example\",\"kind\":\"javascript\","
        "\"value\":\"ask\"}",
        p, &cap);
    CHECK(failed_with(&cap, "broker", "invalidArguments"));
    svc("permissions.setDefault", "{\"kind\":\"notifications\",\"value\":\"block\"}", p, &cap);
    content_setting_of(page, "https://other.example/", "notifications", setting, sizeof setting);
    CHECK(strcmp(setting, "block") == 0);
    svc("permissions.getDefault", "{\"kind\":\"notifications\"}", p, &cap);
    CHECK(strstr(cap.json, "\"value\":\"block\"") != NULL);
    svc("permissions.reset", "{\"origin\":\"https://perm.example\"}", p, &cap);
    content_setting_of(page, "https://perm.example/", "geolocation", setting, sizeof setting);
    CHECK(strcmp(setting, "allow") != 0);
}

static void test_engine_the_default_browser_status_is_answered(void) {
    t_start_frozen();
    t_capture_t cap;
    svc("system.defaultBrowser.status", "{}", 0, &cap);
    CHECK(!cap.has_error && strstr(cap.json, "\"isDefault\":") &&
          strstr(cap.json, "\"canBeDefault\":"));
    svc("runtime.info", "{}", 0, &cap);
    CHECK(strstr(cap.json, "\"chromiumVersion\":\"") != NULL);
}

static void start_capture(bdk_contents_t c, const char* path, const char* format) {
    char args[256];
    snprintf(args, sizeof args, "{\"contents\":\"%llu\",\"path\":\"%s\",\"format\":\"%s\"}",
             (unsigned long long)c, path, format);
    command("capture.start", args, NULL, 0);
}

static int wait_for_capture(void) {
    for (int i = 0; i < 400; ++i) {
        char json[32], state[8];
        command("capture.state", "{}", json, sizeof json);
        value_of_member(json, "state", state, sizeof state);
        if (state[0] != '0')
            return state[0] == '1';
        bdk_testing_run_for(0.05);
    }
    return 0;
}

static size_t file_prefix(const char* path, char* out, size_t n) {
    FILE* f = fopen(path, "rb");
    if (!f)
        return 0;
    size_t got = fread(out, 1, n, f);
    fclose(f);
    return got;
}

static void test_engine_a_page_is_captured_as_an_image_and_as_a_pdf(void) {
    bdk_contents_t c = start_with_page();
    bdk_harness_engine_show(c, 1);
    for (int i = 0; i < 20; ++i)
        bdk_testing_run_for(0.05);
    char dir[] = "/tmp/bdk-engine-capture-XXXXXX";
    REQUIRE(mkdtemp(dir) != NULL);
    char png[96], pdf[96], head[8];
    snprintf(png, sizeof png, "%s/page.png", dir);
    snprintf(pdf, sizeof pdf, "%s/page.pdf", dir);
    start_capture(c, png, "png");
    CHECK(wait_for_capture());
    CHECK(file_prefix(png, head, 4) == 4 && memcmp(head, "\x89PNG", 4) == 0);
    start_capture(c, pdf, "pdf");
    CHECK(wait_for_capture());
    CHECK(file_prefix(pdf, head, 4) == 4 && memcmp(head, "%PDF", 4) == 0);
}

const t_test_t t_engine_tests[] = {
    {"engine_window_open_with_features_raises_a_popup_request",
     test_engine_window_open_with_features_raises_a_popup_request},
    {"engine_window_open_without_features_raises_a_new_page_request",
     test_engine_window_open_without_features_raises_a_new_page_request},
    {"engine_a_rejected_new_window_is_destroyed", test_engine_a_rejected_new_window_is_destroyed},
    {"engine_a_defaulted_new_window_opens_in_the_default_presenter",
     test_engine_a_defaulted_new_window_opens_in_the_default_presenter},
    {"engine_a_right_click_on_a_link_raises_a_context_menu_with_link_items",
     test_engine_a_right_click_on_a_link_raises_a_context_menu_with_link_items},
    {"engine_a_right_click_on_the_page_offers_navigation_items",
     test_engine_a_right_click_on_the_page_offers_navigation_items},
    {"engine_inspect_opens_devtools_through_a_devtools_request",
     test_engine_inspect_opens_devtools_through_a_devtools_request},
    {"engine_html_fullscreen_enters_and_exits_through_requests",
     test_engine_html_fullscreen_enters_and_exits_through_requests},
    {"engine_a_denied_page_ends_up_not_fullscreen",
     test_engine_a_denied_page_ends_up_not_fullscreen},
    {"engine_a_crashed_renderer_reports_renderer_gone",
     test_engine_a_crashed_renderer_reports_renderer_gone},
    {"engine_a_page_chromium_discards_loads_again_when_shown",
     test_engine_a_page_chromium_discards_loads_again_when_shown},
    {"engine_showing_and_hiding_the_view_changes_visibility",
     test_engine_showing_and_hiding_the_view_changes_visibility},
    {"engine_keys_the_page_leaves_reach_the_application",
     test_engine_keys_the_page_leaves_reach_the_application},
    {"engine_an_input_method_composes_text_in_the_page",
     test_engine_an_input_method_composes_text_in_the_page},
    {"engine_the_accessibility_tree_exposes_page_content",
     test_engine_the_accessibility_tree_exposes_page_content},
    {"engine_a_new_page_is_a_tab_of_a_parking_context",
     test_engine_a_new_page_is_a_tab_of_a_parking_context},
    {"engine_attaching_a_page_to_a_registered_window_moves_it_into_its_context",
     test_engine_attaching_a_page_to_a_registered_window_moves_it_into_its_context},
    {"engine_moving_a_page_between_windows_keeps_its_tab_history_and_renderer",
     test_engine_moving_a_page_between_windows_keeps_its_tab_history_and_renderer},
    {"engine_a_detached_page_stays_a_background_tab_of_its_last_window",
     test_engine_a_detached_page_stays_a_background_tab_of_its_last_window},
    {"engine_closing_a_window_parks_its_pages", test_engine_closing_a_window_parks_its_pages},
    {"engine_focusing_a_page_activates_its_tab_and_destroying_it_closes_the_tab",
     test_engine_focusing_a_page_activates_its_tab_and_destroying_it_closes_the_tab},
    {"engine_only_open_codecs_are_offered", test_engine_only_open_codecs_are_offered},
    {"engine_session_state_restores_chromium_history_into_a_new_page",
     test_engine_session_state_restores_chromium_history_into_a_new_page},
    {"engine_persistent_profiles_outlive_the_runtime",
     test_engine_persistent_profiles_outlive_the_runtime},
    {"engine_a_closed_profile_releases_its_descriptors",
     test_engine_a_closed_profile_releases_its_descriptors},
    {"engine_the_heavy_ad_opt_out_store_stays_in_memory",
     test_engine_the_heavy_ad_opt_out_store_stays_in_memory},
    {"engine_after_startup_work_runs_once_the_runtime_has_started",
     test_engine_after_startup_work_runs_once_the_runtime_has_started},
    {"engine_history_is_searched_paginated_and_cleared",
     test_engine_history_is_searched_paginated_and_cleared},
    {"engine_bookmarks_nest_move_and_remove", test_engine_bookmarks_nest_move_and_remove},
    {"engine_closing_a_profile_during_a_bookmarks_export",
     test_engine_closing_a_profile_during_a_bookmarks_export},
    {"engine_bookmarks_are_imported_and_exported_as_html",
     test_engine_bookmarks_are_imported_and_exported_as_html},
    {"engine_a_download_completes_and_is_controllable",
     test_engine_a_download_completes_and_is_controllable},
    {"engine_a_download_completes_in_a_profile_whose_manager_came_first",
     test_engine_a_download_completes_in_a_profile_whose_manager_came_first},
    {"engine_profiles_keep_their_data_apart_and_private_ones_record_no_history",
     test_engine_profiles_keep_their_data_apart_and_private_ones_record_no_history},
    {"engine_settings_and_permissions_round_trip_to_chromium",
     test_engine_settings_and_permissions_round_trip_to_chromium},
    {"engine_the_default_browser_status_is_answered",
     test_engine_the_default_browser_status_is_answered},
    {"engine_a_page_is_captured_as_an_image_and_as_a_pdf",
     test_engine_a_page_is_captured_as_an_image_and_as_a_pdf},
    {NULL, NULL},
};
