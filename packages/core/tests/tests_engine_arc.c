/* The layouts of the Arc-style reference application on the engine (M8: T-ARC-03, T-ADP-06). BDK
   has no space, split or tab type: each layout is the application's own views in windows it
   registered, so these scenarios do what apps/examples/arc-style-browser does with its views and
   check what Chromium makes of it. Several pages side by side in one window (a split), a page
   whose view leaves its window (its space is hidden) and comes back, pages moved between panes and
   windows, a relaunch in which Chromium restores nothing, and a script that drives pages in two
   windows, one of them signed in to a test site. Built only into the engine conformance app
   (BDK_HARNESS_ENGINE_TESTS); pages are served by the engine's test interceptor. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bdk_testing.h"
#include "engine_ext_support.h"
#include "test_support.h"

#ifndef BDK_TEST_SPACES_DIGEST
#define BDK_TEST_SPACES_DIGEST "sha256:unset"
#endif
#ifndef BDK_EXTENSION_FIXTURES
#define BDK_EXTENSION_FIXTURES "tests/extensions/fixtures"
#endif

/* From harness_engine.mm. */
int bdk_harness_engine_window_open(bdk_runtime_t rt);
void bdk_harness_engine_window_place(int window, bdk_contents_t contents, int slot, int slots);
void bdk_harness_engine_window_detach(bdk_contents_t contents);
void bdk_harness_engine_window_close(int window);
bdk_native_handle_t bdk_harness_engine_gesture(bdk_contents_t contents, int kind);
extern int bdk_harness_engine_keep_profiles;

#define PANE_A "https://bdk.test/arc-a"
#define PANE_B "https://bdk.test/arc-b"
#define PANE_C "https://bdk.test/arc-c"

/* ---- helpers ---- */

static void settle(void) {
    t_pump();
    bdk_testing_run_for(0.05);
}

static void control(const char* verb, const char* args, char* out, size_t n) {
    REQUIRE(bdk_testing_command(t_runtime, S(verb), S(args), out, n) == BDK_OK);
}

static void page_control(bdk_contents_t c, const char* verb, const char* extra, char* out,
                         size_t n) {
    char args[512];
    snprintf(args, sizeof args, "{\"contents\":\"%llu\"%s%s}", (unsigned long long)c,
             extra[0] ? "," : "", extra);
    control(verb, args, out, n);
}

static void script_page(const char* url, const char* html) {
    static char args[4096];
    size_t n = (size_t)snprintf(args, sizeof args,
                                "{\"url\":\"%s\",\"contentType\":\"text/html\",\"headers\":\"\","
                                "\"body\":\"",
                                url);
    for (const char* c = html; *c && n + 8 < sizeof args; ++c) {
        if (*c == '"' || *c == '\\')
            args[n++] = '\\';
        args[n++] = *c;
    }
    snprintf(args + n, sizeof args - n, "\"}");
    control("script_page", args, NULL, 0);
}

static void run_script(bdk_contents_t c, const char* js, int gesture) {
    REQUIRE(bdk_testing_execute_script(c, S(js), (uint8_t)gesture) == BDK_OK);
    t_pump();
}

static bdk_contents_state_t state_of(bdk_contents_t c) {
    bdk_contents_state_t st;
    memset(&st, 0, sizeof st);
    st.struct_size = sizeof st;
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_state(c, &st, &err) == BDK_OK);
    return st;
}

static void title_of(bdk_contents_t c, char* out, size_t n) {
    bdk_contents_state_t st = state_of(c);
    snprintf(out, n, "%.*s", (int)st.title.length, st.title.data ? st.title.data : "");
}

static int wait_for_title(bdk_contents_t c, const char* want) {
    char title[256] = "";
    for (int i = 0; i < 100; ++i) {
        title_of(c, title, sizeof title);
        if (strcmp(title, want) == 0)
            return 1;
        settle();
    }
    printf("    title is \"%s\", not \"%s\"\n", title, want);
    return 0;
}

static int wait_for_visibility(bdk_contents_t c, uint32_t want) {
    for (int i = 0; i < 100; ++i) {
        if (state_of(c).visibility == want)
            return 1;
        settle();
    }
    printf("    the page's visibility is not %u\n", want);
    return 0;
}

static void load(bdk_contents_t c, const char* url) {
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_load(c, S(url), &err) == BDK_OK);
    t_pump();
}

static bdk_testing_tab_info_t tab_info_of(bdk_contents_t c) {
    bdk_testing_tab_info_t info;
    memset(&info, 0, sizeof info);
    info.struct_size = sizeof info;
    REQUIRE(bdk_testing_tab_info(c, &info) == BDK_OK);
    return info;
}

/* Starts with the harness's resolved configuration plus `extra` members. */
static void start_with(const char* extra) {
    bdk_start_config_t c;
    t_fill_start_config(&c);
    const bdk_string_t base = c.resolved_config_json;
    REQUIRE(base.length > 1 && base.data[base.length - 1] == '}');
    static char resolved[4096];
    snprintf(resolved, sizeof resolved, "%.*s%s%s}", (int)(base.length - 1), base.data,
             extra[0] ? "," : "", extra);
    c.resolved_config_json = S(resolved);
    bdk_error_t err = 0;
    REQUIRE(bdk_runtime_start(&c, &t_runtime, &err) == BDK_OK);
}

/* ---- what the handlers were given ---- */

typedef struct {
    int calls;
    bdk_request_t request;
    bdk_contents_t contents;
    char payload[2048];
    /* With complete set, the handler completes the request with it at once, with a gesture in
       the page's window when gesture is set. */
    const char* complete;
    const char* complete_args;
    int gesture;
} seen_t;

static seen_t g_dialogs, g_permissions, g_offers;

static bdk_ui_response_t record(void* ctx, const bdk_ui_request_info_t* info) {
    seen_t* seen = (seen_t*)ctx;
    ++seen->calls;
    seen->request = info->request;
    seen->contents = info->contents;
    snprintf(seen->payload, sizeof seen->payload, "%.*s", (int)info->payload_json.length,
             info->payload_json.data ? info->payload_json.data : "");
    if (seen->complete) {
        bdk_native_handle_t gesture;
        memset(&gesture, 0, sizeof gesture);
        if (seen->gesture)
            gesture = bdk_harness_engine_gesture(info->contents, 0);
        bdk_error_t err = 0;
        CHECK(bdk_ui_complete(info->request, S(seen->complete),
                              S(seen->complete_args ? seen->complete_args : ""), gesture,
                              &err) == BDK_OK);
    }
    return BDK_UI_HANDLED;
}

static int wait_for_calls(const seen_t* seen, int calls) {
    for (int i = 0; i < 100 && seen->calls < calls; ++i)
        settle();
    return seen->calls >= calls;
}

/* ---- a translation provider that turns French into upper case ---- */

static void fake_pairs(void* ctx, bdk_context_t call, bdk_string_t input) {
    (void)ctx, (void)input;
    bdk_provider_resolve(call, S("{\"pairs\":[{\"from\":\"fr\",\"to\":\"en\"}]}"));
}

static void fake_translate(void* ctx, bdk_context_t call, bdk_string_t input) {
    (void)ctx, (void)input;
    bdk_provider_resolve(call, S("{\"texts\":[\"BONJOUR\"]}"));
}

static void register_translation(void) {
    bdk_provider_vtable_t v;
    memset(&v, 0, sizeof v);
    v.struct_size = sizeof v;
    v.descriptor_json = S("{\"id\":\"app.fake\",\"name\":\"Fake\",\"privacy\":\"onDevice\"}");
    v.supported_pairs = fake_pairs;
    v.translate = fake_translate;
    bdk_error_t err = 0;
    REQUIRE(bdk_provider_register(t_runtime, S("translation"), &v, NULL, NULL, &err) == BDK_OK);
}

/* ---- split view (T-ARC-02, T-ADP-06) ---- */

static int find_count(bdk_contents_t c) {
    char out[256];
    page_control(c, "find.result", "", out, sizeof out);
    const char* at = strstr(out, "\"count\":");
    return at ? atoi(at + 8) : -1;
}

/* Three pages side by side in one window, the first focused. Every one of them is on screen, so
   each is foreground for the services that ask a person something: a JavaScript dialog, a
   permission prompt, find and a translation offer reach the application for a pane that is not
   the window's active tab, and none of them activates it. */
static void
test_arc_every_visible_pane_is_foreground_for_dialogs_permissions_find_and_offers(void) {
    memset(&g_dialogs, 0, sizeof g_dialogs);
    memset(&g_permissions, 0, sizeof g_permissions);
    memset(&g_offers, 0, sizeof g_offers);
    start_with("\"services\":{\"translation\":{\"provider\":\"app\"}}");
    register_translation();
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_JAVASCRIPT_DIALOGS, record, &g_dialogs, NULL,
                               &err) == BDK_OK);
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_PERMISSIONS, record, &g_permissions, NULL,
                               &err) == BDK_OK);
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_TRANSLATION, record, &g_offers, NULL, &err) ==
            BDK_OK);
    t_freeze();
    control("present.clear", "{}", NULL, 0);
    bdk_profile_t profile = t_new_profile("Split");
    int window = bdk_harness_engine_window_open(t_runtime);
    REQUIRE(window >= 0);
    bdk_contents_t panes[3];
    const char* urls[3] = {PANE_A, PANE_B, PANE_C};
    for (int i = 0; i < 3; ++i) {
        panes[i] = t_new_contents(profile);
        bdk_harness_engine_window_place(window, panes[i], i, 3);
        load(panes[i], urls[i]);
    }
    REQUIRE(bdk_contents_focus(panes[0]) == BDK_OK);
    t_pump();
    for (int i = 0; i < 3; ++i)
        CHECK(wait_for_visibility(panes[i], BDK_VISIBILITY_VISIBLE));
    CHECK(tab_info_of(panes[0]).active);
    CHECK(!tab_info_of(panes[1]).active && !tab_info_of(panes[2]).active);
    CHECK(tab_info_of(panes[0]).context_id == tab_info_of(panes[2]).context_id);

    /* A JavaScript dialog from the middle pane. */
    g_dialogs.complete = "accept";
    g_dialogs.complete_args = "{\"suppressFurther\":false}";
    run_script(panes[1], "document.title = confirm('stay?') ? 'yes' : 'no'", 0);
    CHECK(wait_for_title(panes[1], "yes"));
    CHECK(g_dialogs.calls == 1 && g_dialogs.contents == panes[1]);

    /* A permission prompt from the last pane, granted with a gesture in its window. */
    g_permissions.complete = "grant";
    g_permissions.complete_args = "{\"kinds\":[\"notifications\"],\"duration\":\"persistent\"}";
    g_permissions.gesture = 1;
    run_script(panes[2], "Notification.requestPermission().then(p => document.title = p)", 1);
    CHECK(wait_for_calls(&g_permissions, 1));
    CHECK(g_permissions.contents == panes[2]);
    CHECK(wait_for_title(panes[2], "granted"));

    /* Find in the last pane, through its default find bar. */
    run_script(panes[2], "document.body.innerText = 'alpha beta alpha'", 0);
    control("present.script", "{\"surface\":\"find\",\"answer\":{}}", NULL, 0);
    page_control(panes[2], "find.present", "", NULL, 0);
    page_control(panes[2], "find.start", "\"text\":\"alpha\"", NULL, 0);
    int count = -1;
    for (int i = 0; i < 60 && count != 2; ++i) {
        count = find_count(panes[2]);
        settle();
    }
    CHECK(count == 2);
    char record_out[2048];
    control("present.record", "{\"surface\":\"find\"}", record_out, sizeof record_out);
    CHECK(strstr(record_out, "\"shown\":1") != NULL);
    page_control(panes[2], "find.stop", "", NULL, 0);

    /* A translation offer for the middle pane. */
    script_page("https://fr.example/arc", "<html lang=\"fr\"><body><p>bonjour</p></body></html>");
    load(panes[1], "https://fr.example/arc");
    CHECK(wait_for_calls(&g_offers, 1));
    CHECK(g_offers.contents == panes[1]);

    /* None of it moved the window's active tab. */
    CHECK(tab_info_of(panes[0]).active);
    CHECK(!tab_info_of(panes[1]).active && !tab_info_of(panes[2]).active);
    control("present.clear", "{}", NULL, 0);
    bdk_harness_engine_window_close(window);
}

/* What extensions see of a split (risk R2): the focused pane is the window's active tab; the
   others are background tabs of the same window, as every unselected tab is in Chrome. */
static void test_arc_extensions_see_the_focused_pane_as_the_active_tab(void) {
    t_start_frozen();
    bdk_profile_t profile = t_new_profile("Split extensions");
    int window = bdk_harness_engine_window_open(t_runtime);
    REQUIRE(window >= 0);
    bdk_contents_t panes[3];
    const char* urls[3] = {PANE_A, PANE_B, PANE_C};
    for (int i = 0; i < 3; ++i) {
        panes[i] = t_new_contents(profile);
        bdk_harness_engine_window_place(window, panes[i], i, 3);
        load(panes[i], urls[i]);
    }
    REQUIRE(bdk_contents_focus(panes[1]) == BDK_OK);
    t_pump();
    char id[64], out[2048], expr[320];
    ext_load(panes[1], "ui-action-click", id, sizeof id);
    /* The answer is an object: ext_ask reads a resolved string as no answer yet. */
    snprintf(expr, sizeof expr,
             "chrome.tabs.get(%d).then(t => chrome.tabs.query({windowId: t.windowId}))"
             ".then(ts => ({tabs: ts.map(t => (t.active ? 'A' : '-') + t.id).join(' ')}),"
             " e => ({error: e.message}))",
             tab_info_of(panes[0]).tab_id);
    ext_ask(panes[1], id, expr, out, sizeof out);
    char want[128];
    snprintf(want, sizeof want, "\"tabs\":\"-%d A%d -%d\"", tab_info_of(panes[0]).tab_id,
             tab_info_of(panes[1]).tab_id, tab_info_of(panes[2]).tab_id);
    CHECK(ext_has(out, want));
    if (!ext_has(out, want))
        printf("    tabs.query reported %s, expected %s\n", out, want);
    bdk_harness_engine_window_close(window);
}

/* ---- a page's visibility follows its view (T-ADP-11) ---- */

/* A page whose view has never been in a window reports hidden, though a WebContents starts
   visible; once shown it reports visible. */
static void test_arc_a_page_never_shown_reports_hidden(void) {
    t_start_frozen();
    bdk_profile_t profile = t_new_profile("Unshown");
    bdk_contents_t page = t_new_contents(profile);
    load(page, PANE_A);
    CHECK(wait_for_visibility(page, BDK_VISIBILITY_HIDDEN));
    int window = bdk_harness_engine_window_open(t_runtime);
    REQUIRE(window >= 0);
    bdk_harness_engine_window_place(window, page, 0, 1);
    CHECK(wait_for_visibility(page, BDK_VISIBILITY_VISIBLE));
    bdk_harness_engine_window_close(window);
}

/* The pane that had focus reports hidden when the window stops showing it, and visible when it
   comes back. */
static void test_arc_the_focused_pane_reports_hidden_when_its_view_leaves(void) {
    t_start_frozen();
    bdk_profile_t profile = t_new_profile("Focused");
    int window = bdk_harness_engine_window_open(t_runtime);
    REQUIRE(window >= 0);
    bdk_contents_t page = t_new_contents(profile);
    bdk_contents_t other = t_new_contents(profile);
    bdk_harness_engine_window_place(window, page, 0, 1);
    load(page, PANE_A);
    REQUIRE(bdk_contents_focus(page) == BDK_OK);
    t_pump();
    REQUIRE(wait_for_visibility(page, BDK_VISIBILITY_VISIBLE));
    /* Focus stays with the page while its view leaves. */
    bdk_harness_engine_window_detach(page);
    bdk_harness_engine_window_place(window, other, 0, 1);
    load(other, PANE_B);
    CHECK(wait_for_visibility(page, BDK_VISIBILITY_HIDDEN));
    bdk_harness_engine_window_detach(other);
    bdk_harness_engine_window_place(window, page, 0, 1);
    CHECK(wait_for_visibility(page, BDK_VISIBILITY_VISIBLE));
    bdk_harness_engine_window_close(window);
}

/* ---- hide and restore (T-ARC-03) ---- */

#define TICKING                                                                                    \
    "<title>ticking</title><script>window.ticks = 0;"                                              \
    "setInterval(() => { window.ticks++; }, 100);</script>"

/* A page in a space the window stops showing: its view leaves the window. It reports hidden, stays
   a background tab of that window (not parked), keeps running, and comes back as it was. */
static void test_arc_a_page_in_a_hidden_space_keeps_running_and_comes_back(void) {
    t_start_frozen();
    bdk_profile_t profile = t_new_profile("Spaces");
    int window = bdk_harness_engine_window_open(t_runtime);
    REQUIRE(window >= 0);
    script_page("https://bdk.test/ticking", TICKING);
    bdk_contents_t page = t_new_contents(profile);
    bdk_contents_t other = t_new_contents(profile);
    bdk_harness_engine_window_place(window, page, 0, 1);
    load(page, "https://bdk.test/ticking");
    run_script(page, "window.bdkMarker = 7", 0);
    REQUIRE(wait_for_visibility(page, BDK_VISIBILITY_VISIBLE));
    bdk_testing_tab_info_t shown = tab_info_of(page);

    /* The window switches to another space: the other space's page takes the window's content. */
    bdk_harness_engine_window_detach(page);
    bdk_harness_engine_window_place(window, other, 0, 1);
    load(other, PANE_B);
    REQUIRE(bdk_contents_focus(other) == BDK_OK);
    t_pump();
    CHECK(wait_for_visibility(page, BDK_VISIBILITY_HIDDEN));
    bdk_testing_tab_info_t hidden = tab_info_of(page);
    CHECK(hidden.is_tab && !hidden.parking);
    CHECK(hidden.context_id == shown.context_id);
    CHECK(hidden.tab_id == shown.tab_id);
    CHECK(!hidden.active);
    run_script(page, "window.mark = window.ticks", 0);
    bdk_testing_run_for(2.5);
    run_script(page, "document.title = window.ticks > window.mark ? 'running' : 'stopped'", 0);
    CHECK(wait_for_title(page, "running"));

    /* Back to the first space. */
    bdk_harness_engine_window_detach(other);
    bdk_harness_engine_window_place(window, page, 0, 1);
    REQUIRE(bdk_contents_focus(page) == BDK_OK);
    t_pump();
    CHECK(wait_for_visibility(page, BDK_VISIBILITY_VISIBLE));
    bdk_testing_tab_info_t back = tab_info_of(page);
    CHECK(back.tab_id == shown.tab_id && back.context_id == shown.context_id && back.active);
    /* Not reloaded: the page's own state is still there. */
    run_script(page, "document.title = 'marker:' + window.bdkMarker", 0);
    CHECK(wait_for_title(page, "marker:7"));
    bdk_harness_engine_window_close(window);
}

/* ---- move and reparent (T-ARC-03) ---- */

static int has_active_tab(bdk_contents_t c, const char* id) {
    char extra[128], out[256];
    snprintf(extra, sizeof extra, "\"id\":\"%s\"", id);
    page_control(c, "extui.has_active_tab", extra, out, sizeof out);
    return strstr(out, "\"granted\":true") != NULL;
}

/* A page moved from one pane to another of its window, then to another window (another space's
   window, or a page dragged out): its tab identifier, history, renderer and the activeTab grant an
   extension holds for it all survive, and the extension can still use the grant. */
static void test_arc_a_page_moved_between_panes_and_windows_keeps_its_tab_grant_and_renderer(void) {
    t_start_frozen();
    bdk_profile_t profile = t_new_profile("Moves");
    int first = bdk_harness_engine_window_open(t_runtime);
    int second = bdk_harness_engine_window_open(t_runtime);
    REQUIRE(first >= 0 && second >= 0);
    bdk_contents_t page = t_new_contents(profile);
    bdk_contents_t neighbour = t_new_contents(profile);
    bdk_harness_engine_window_place(first, page, 0, 2);
    bdk_harness_engine_window_place(first, neighbour, 1, 2);
    load(page, PANE_A);
    load(page, PANE_B);
    load(neighbour, PANE_C);
    run_script(page, "window.bdkMarker = 42", 0);
    REQUIRE(bdk_contents_focus(page) == BDK_OK);
    t_pump();
    char id[64];
    ext_load(page, "ui-action-click", id, sizeof id);
    bdk_buffer_t out = 0;
    bdk_error_t err = 0;
    REQUIRE(bdk_extension_action_activate(page, S(id), bdk_harness_engine_gesture(page, 0), &out,
                                          &err) == BDK_OK);
    bdk_release(out);
    char injected[64];
    bdk_testing_tab_info_t before = tab_info_of(page);
    snprintf(injected, sizeof injected, "injected:%d", before.tab_id);
    REQUIRE(wait_for_title(page, injected));
    REQUIRE(has_active_tab(page, id));

    /* To the other pane of the same window: the same tab, nothing moves in Chromium. */
    bdk_harness_engine_window_place(first, neighbour, 0, 2);
    bdk_harness_engine_window_place(first, page, 1, 2);
    bdk_testing_tab_info_t swapped = tab_info_of(page);
    CHECK(swapped.context_id == before.context_id && swapped.tab_id == before.tab_id);

    /* To the other window. */
    bdk_harness_engine_window_place(second, page, 0, 1);
    REQUIRE(bdk_contents_focus(page) == BDK_OK);
    t_pump();
    bdk_testing_tab_info_t moved = tab_info_of(page);
    CHECK(moved.context_id != before.context_id);
    CHECK(moved.tab_id == before.tab_id);
    CHECK(has_active_tab(page, id));
    /* The renderer and history came along. */
    run_script(page, "document.title = 'marker:' + window.bdkMarker", 0);
    CHECK(wait_for_title(page, "marker:42"));
    bdk_buffer_t entries = 0;
    REQUIRE(bdk_contents_entries(page, &entries, &err) == BDK_OK);
    char text[1024];
    t_buffer_text(entries, text, sizeof text);
    bdk_release(entries);
    CHECK(strstr(text, PANE_A) != NULL && strstr(text, "\"current\":1") != NULL);
    /* The extension still holds the grant for the same tab, now in the other window. */
    char expr[256], answer[256];
    snprintf(expr, sizeof expr,
             "chrome.scripting.executeScript({target: {tabId: %d}, func: () => document.title = "
             "'again'}).then(() => ({ok: true}), e => ({error: e.message}))",
             before.tab_id);
    ext_ask(neighbour, id, expr, answer, sizeof answer);
    CHECK(ext_has(answer, "\"ok\":true"));
    if (!ext_has(answer, "\"ok\":true"))
        printf("    executeScript answered %s\n", answer);
    CHECK(wait_for_title(page, "again"));
    CHECK(bdk_testing_chrome_window_count() == 0);
    bdk_harness_engine_window_close(first);
    bdk_harness_engine_window_close(second);
}

/* ---- session restoration (T-ARC-03) ---- */

/* The application keeps its layout and each page's session state; on the next launch Chromium has
   restored nothing on its own (no window, no tab), and the application rebuilds its pages from the
   states it kept. */
static void test_arc_after_a_relaunch_chromium_restores_nothing_and_the_application_does(void) {
    bdk_harness_engine_keep_profiles = 1;
    t_start_frozen();
    char id[64];
    t_create_profile("Arc relaunch", id, sizeof id);
    bdk_profile_t profile = t_open_profile(id);
    int window = bdk_harness_engine_window_open(t_runtime);
    REQUIRE(window >= 0);
    bdk_contents_t page = t_new_contents(profile);
    bdk_harness_engine_window_place(window, page, 0, 1);
    load(page, PANE_A);
    load(page, PANE_B);
    bdk_buffer_t state = 0;
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_session_state(page, &state, &err) == BDK_OK);
    bdk_bytes_t bytes = bdk_buffer_bytes(state);
    uint8_t* kept = (uint8_t*)malloc(bytes.length);
    REQUIRE(kept != NULL);
    memcpy(kept, bytes.data, bytes.length);
    size_t kept_length = bytes.length;
    bdk_release(state);
    bdk_harness_engine_window_close(window);
    t_stop();

    t_start_frozen();
    bdk_harness_engine_keep_profiles = 0;
    profile = t_open_profile(id);
    t_pump();
    /* Nothing came back by itself. */
    CHECK(bdk_testing_host_context_count() == 0);
    CHECK(bdk_testing_chrome_window_count() == 0);
    window = bdk_harness_engine_window_open(t_runtime);
    REQUIRE(window >= 0);
    bdk_contents_t restored = t_new_contents(profile);
    bdk_bytes_t again = {kept, kept_length};
    REQUIRE(bdk_contents_restore(restored, again, &err) == BDK_OK);
    bdk_harness_engine_window_place(window, restored, 0, 1);
    CHECK(wait_for_title(restored, PANE_B));
    bdk_buffer_t entries = 0;
    REQUIRE(bdk_contents_entries(restored, &entries, &err) == BDK_OK);
    char text[1024];
    t_buffer_text(entries, text, sizeof text);
    bdk_release(entries);
    CHECK(strstr(text, PANE_A) != NULL && strstr(text, "\"current\":1") != NULL);
    REQUIRE(bdk_contents_go_to_offset(restored, -1, &err) == BDK_OK);
    CHECK(wait_for_title(restored, PANE_A));
    CHECK(tab_info_of(restored).tab_count == 1);
    free(kept);
    bdk_harness_engine_window_close(window);
}

/* ---- automation (T-ARC-03) ---- */

#define SPACES_SCRIPT                                                                              \
    "\"automation\":{\"enabled\":true,\"external\":{\"enabled\":false},\"scripts\":{\"spaces\":{"  \
    "\"path\":\"Automation/spaces.ts\",\"digest\":\"sha256:0\",\"bundle\":"                        \
    "\"BDK/Automation/spaces.js\",\"bundle_digest\":\"" BDK_TEST_SPACES_DIGEST "\"}}}"

#define ACCOUNT                                                                                    \
    "<title>account</title><p id=\"who\"></p><script>document.getElementById('who').textContent"   \
    " = document.cookie.includes('session=') ? 'Signed in' : 'Signed out';</script>"

typedef struct {
    int finished;
    char finish[4096];
} finish_t;

static void on_task(void* ctx, bdk_task_t task, uint32_t kind, bdk_string_t detail) {
    (void)task;
    finish_t* f = (finish_t*)ctx;
    if (kind != BDK_TASK_EVENT_FINISHED)
        return;
    snprintf(f->finish, sizeof f->finish, "%.*s", (int)detail.length, detail.data);
    f->finished = 1;
}

static void profile_id_of(bdk_profile_t p, char* out, size_t n) {
    t_capture_t cap;
    t_cap_init(&cap);
    bdk_invoke_options_t o;
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.profile = p;
    REQUIRE(bdk_invoke_named(t_runtime, S("profiles.current"), S("{}"), &o, t_on_result, NULL, &cap,
                             t_on_release) != 0);
    for (int i = 0; i < 100 && cap.results == 0; ++i)
        settle();
    REQUIRE(t_json_string(cap.json, "id", out, n));
}

/* A script drives two pages the application shows in two windows, one per space and profile; the
   user signed in to the test site in one of them. The session sees only the pages it was given. */
static void test_arc_a_script_drives_pages_in_two_spaces_one_signed_in(void) {
    t_config_extra = SPACES_SCRIPT;
    t_start_frozen();
    bdk_profile_t work = t_new_profile("Work");
    bdk_profile_t personal = t_new_profile("Personal");
    int work_window = bdk_harness_engine_window_open(t_runtime);
    int personal_window = bdk_harness_engine_window_open(t_runtime);
    REQUIRE(work_window >= 0 && personal_window >= 0);
    script_page("https://site.bdk.test/login",
                "<title>login</title><script>document.cookie = 'session=s3; path=/; Secure';"
                "document.title = 'logged in';</script>");
    script_page("https://site.bdk.test/account", ACCOUNT);
    bdk_contents_t signed_in = t_new_contents(work);
    bdk_contents_t signed_out = t_new_contents(personal);
    bdk_contents_t unrelated = t_new_contents(work);
    bdk_harness_engine_window_place(work_window, signed_in, 0, 2);
    bdk_harness_engine_window_place(work_window, unrelated, 1, 2);
    bdk_harness_engine_window_place(personal_window, signed_out, 0, 1);
    load(signed_in, "https://site.bdk.test/login");
    REQUIRE(wait_for_title(signed_in, "logged in"));
    load(signed_out, PANE_A);
    load(unrelated, PANE_B);

    char work_id[64], personal_id[64], options[512];
    profile_id_of(work, work_id, sizeof work_id);
    profile_id_of(personal, personal_id, sizeof personal_id);
    snprintf(options, sizeof options,
             "{\"name\":\"spaces\",\"capabilities\":[\"contents.read\",\"contents.control\","
             "\"contents.script\"],\"scope\":{\"profiles\":[\"%s\",\"%s\"],"
             "\"contents\":\"explicit\"}}",
             work_id, personal_id);
    bdk_session_t session = 0;
    bdk_error_t err = 0;
    REQUIRE(bdk_session_create(t_runtime, S(options), &session, &err) == BDK_OK);
    REQUIRE(bdk_session_grant_contents(session, signed_in, &err) == BDK_OK);
    REQUIRE(bdk_session_grant_contents(session, signed_out, &err) == BDK_OK);

    char args[256];
    snprintf(args, sizeof args,
             "{\"signedIn\":\"%llu\",\"signedOut\":\"%llu\","
             "\"url\":\"https://site.bdk.test/account\"}",
             (unsigned long long)signed_in, (unsigned long long)signed_out);
    static finish_t finish;
    memset(&finish, 0, sizeof finish);
    bdk_task_t task = 0;
    REQUIRE(bdk_session_run(session, S("spaces"), S(args), &task, &err) == BDK_OK);
    REQUIRE(bdk_task_subscribe(task, on_task, &finish, NULL) != 0);
    for (int i = 0; i < 400 && !finish.finished; ++i)
        settle();
    REQUIRE(finish.finished);
    printf("    finished: %s\n", finish.finish);
    CHECK(strstr(finish.finish, "\"listed\":2") != NULL);
    CHECK(strstr(finish.finish, "\"signedIn\":\"Signed in\"") != NULL);
    CHECK(strstr(finish.finish, "\"signedOut\":\"Signed out\"") != NULL);
    /* The script moved no view: each page is still where the application put it. */
    CHECK(tab_info_of(signed_in).context_id == tab_info_of(unrelated).context_id);
    CHECK(tab_info_of(signed_out).context_id != tab_info_of(signed_in).context_id);
    bdk_release(task);
    bdk_harness_engine_window_close(work_window);
    bdk_harness_engine_window_close(personal_window);
}

const t_test_t t_engine_arc_tests[] = {
    {"engine_arc_every_visible_pane_is_foreground_for_dialogs_permissions_find_and_offers",
     test_arc_every_visible_pane_is_foreground_for_dialogs_permissions_find_and_offers},
    {"engine_arc_extensions_see_the_focused_pane_as_the_active_tab",
     test_arc_extensions_see_the_focused_pane_as_the_active_tab},
    {"engine_arc_a_page_never_shown_reports_hidden", test_arc_a_page_never_shown_reports_hidden},
    {"engine_arc_the_focused_pane_reports_hidden_when_its_view_leaves",
     test_arc_the_focused_pane_reports_hidden_when_its_view_leaves},
    {"engine_arc_a_page_in_a_hidden_space_keeps_running_and_comes_back",
     test_arc_a_page_in_a_hidden_space_keeps_running_and_comes_back},
    {"engine_arc_a_page_moved_between_panes_and_windows_keeps_its_tab_grant_and_renderer",
     test_arc_a_page_moved_between_panes_and_windows_keeps_its_tab_grant_and_renderer},
    {"engine_arc_after_a_relaunch_chromium_restores_nothing_and_the_application_does",
     test_arc_after_a_relaunch_chromium_restores_nothing_and_the_application_does},
    {"engine_arc_a_script_drives_pages_in_two_spaces_one_signed_in",
     test_arc_a_script_drives_pages_in_two_spaces_one_signed_in},
    {NULL, NULL},
};
