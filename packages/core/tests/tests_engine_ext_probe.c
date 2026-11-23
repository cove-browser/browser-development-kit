/* The MV3 probe under native hosting (T-EXT-01, owed since M0 and run here for M5): what a real
   extension observes of pages the application places, by the assertions in tasks.md "Assertions for
   T-EXT-01". The probe is tests/extensions/probe, loaded unpacked without the install prompt; its
   worker records tab and window events and answers probes (tests/extensions/README.md, "The probe
   protocol"). Two pages A and B share one registered window, B focused unless a test says
   otherwise. Built only into the engine conformance app. Assertions 6, 8 and 9 (popup, context-menu
   item, side panel) are covered by tests_engine_ext_ui.c and tests_engine_ext_panels.c. */
#include <stdio.h>
#include <string.h>

#include "bdk_testing.h"
#include "engine_ext_support.h"
#include "test_support.h"

/* From harness_engine.mm and harness_engine_ext.mm. */
bdk_native_handle_t bdk_harness_engine_gesture(bdk_contents_t contents, int kind);
int bdk_ext_window_open(bdk_runtime_t rt, uint32_t kind, bdk_window_t* handle);
void bdk_ext_window_attach(int window, bdk_contents_t contents);
void bdk_ext_remove_view(bdk_native_handle_t view);

#define PAGE_A "https://bdk.test/probe/a"
#define PAGE_B "https://bdk.test/probe/b"
#define PAGE_C "https://bdk.test/probe/c"
/* Another origin: Chromium keeps an activeTab grant across navigations within the origin
   (ActiveTabPermissionGranter::DidFinishNavigation). */
#define PAGE_OTHER_ORIGIN "https://other.bdk.test/probe/d"

static bdk_profile_t g_profile;
static bdk_contents_t g_a, g_b;
static int g_window;
static char g_probe[40];

static int tab_id(bdk_contents_t c) {
    bdk_testing_tab_info_t info;
    memset(&info, 0, sizeof info);
    info.struct_size = sizeof info;
    REQUIRE(bdk_testing_tab_info(c, &info) == BDK_OK);
    REQUIRE(info.is_tab);
    return info.tab_id;
}

static void load(bdk_contents_t c, const char* url) {
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_load(c, S(url), &err) == BDK_OK);
    ext_pumps(5);
}

/* What a probe answered, as JSON text with quotes unescaped. */
static void probe(const char* name, const char* args, char* out, size_t n) {
    char expr[256];
    snprintf(expr, sizeof expr, "bdkProbe('%s',%s)", name, args);
    ext_ask(g_a, g_probe, expr, out, n);
}

static void with_tab(const char* name, int tab, char* out, size_t n) {
    char args[64];
    snprintf(args, sizeof args, "{tabId:%d}", tab);
    probe(name, args, out, n);
}

/* Copies the part of text between two markers (or to its end). */
static void section(const char* text, const char* from, const char* to, char* out, size_t n) {
    out[0] = '\0';
    const char* start = strstr(text, from);
    if (!start)
        return;
    const char* end = to ? strstr(start, to) : NULL;
    size_t len = end ? (size_t)(end - start) : strlen(start);
    snprintf(out, n, "%.*s", (int)(len < n ? len : n - 1), start);
}

/* The first event record of `type` in the probe's events: {"detail":...,"t":...,"type":...}, whose
   type comes last. */
static void event_of(const char* events, const char* type, char* out, size_t n) {
    out[0] = '\0';
    char key[64];
    snprintf(key, sizeof key, "\"type\":\"%s\"", type);
    const char* end = strstr(events, key);
    if (!end)
        return;
    const char* start = end;
    while (start > events && strncmp(start, "{\"detail\"", 9) != 0)
        --start;
    size_t len = (size_t)(end - start) + strlen(key);
    snprintf(out, n, "%.*s", (int)(len < n ? len : n - 1), start);
}

static int mentions_tab(const char* text, int tab) {
    char id[32], tab_key[32];
    snprintf(id, sizeof id, "\"id\":%d,", tab);
    snprintf(tab_key, sizeof tab_key, "\"tabId\":%d", tab);
    return ext_has(text, id) || ext_has(text, tab_key);
}

/* Waits until every content script of the given tabs has acknowledged (assertion 1). */
static int acked(int a, int b) {
    char out[4096];
    for (int i = 0; i < 40; ++i) {
        probe("acks", "{}", out, sizeof out);
        if (mentions_tab(out, a) && (b < 0 || mentions_tab(out, b)))
            return 1;
        ext_pumps(5);
    }
    printf("    acks: %s\n", out);
    return 0;
}

/* A and B in one registered window, the probe loaded before they navigate (so its content script
   runs in both), B focused. */
static void setup(void) {
    t_start_frozen();
    g_profile = t_new_profile("Probe");
    g_a = t_new_contents(g_profile);
    g_b = t_new_contents(g_profile);
    bdk_window_t handle = 0;
    g_window = bdk_ext_window_open(t_runtime, BDK_WINDOW_KIND_NORMAL, &handle);
    REQUIRE(g_window >= 0);
    bdk_ext_window_attach(g_window, g_a);
    bdk_ext_window_attach(g_window, g_b);
    ext_pumps(3);
    ext_load(g_a, "../probe", g_probe, sizeof g_probe);
    load(g_a, PAGE_A);
    load(g_b, PAGE_B);
    REQUIRE(bdk_contents_focus(g_b) == BDK_OK);
    ext_pumps(5);
}

/* ---- scenarios ---- */

/* Assertion 1: the worker starts and each content script receives an acknowledgement. */
static void test_engine_ext_probe_worker_and_content_ack(void) {
    setup();
    CHECK(acked(tab_id(g_a), tab_id(g_b)));
}

/* Assertion 2: tabs.query({active, currentWindow}) is exactly the focused page, and
   windows.getCurrent is its window. */
static void test_engine_ext_probe_active_tab_is_focused_page(void) {
    setup();
    const int a = tab_id(g_a), b = tab_id(g_b);
    char out[4096], active[1024];
    probe("snapshot", "{}", out, sizeof out);
    section(out, "\"activeCurrent\":", "\"all\":", active, sizeof active);
    CHECK(mentions_tab(active, b));
    CHECK(!mentions_tab(active, a));
    bdk_testing_tab_info_t info;
    memset(&info, 0, sizeof info);
    info.struct_size = sizeof info;
    REQUIRE(bdk_testing_tab_info(g_b, &info) == BDK_OK);
    char current[48];
    snprintf(current, sizeof current, "\"currentWindowId\":%d", info.context_id);
    CHECK(ext_has(out, current));
}

/* Assertion 3: focusing A fires tabs.onActivated for A. */
static void test_engine_ext_probe_focus_fires_on_activated(void) {
    setup();
    char out[4096];
    probe("clearEvents", "{}", out, sizeof out);
    REQUIRE(bdk_contents_focus(g_a) == BDK_OK);
    ext_pumps(10);
    probe("events", "{}", out, sizeof out);
    char activated[1024];
    event_of(out, "tabs.onActivated", activated, sizeof activated);
    CHECK(activated[0] != '\0');
    if (!mentions_tab(activated, tab_id(g_a)))
        printf("    tab %d focused; the events were %s\n", tab_id(g_a), out);
    CHECK(mentions_tab(activated, tab_id(g_a)));
}

/* Assertion 4: moving A to a second window keeps its tab identifier and fires detach, then
   attach. Assertion 7 after the move: messages still reach A. */
static void test_engine_ext_probe_move_keeps_tab_id_and_events(void) {
    setup();
    const int a = tab_id(g_a);
    REQUIRE(acked(a, tab_id(g_b)));
    char out[4096];
    probe("clearEvents", "{}", out, sizeof out);
    bdk_window_t second = 0;
    const int window = bdk_ext_window_open(t_runtime, BDK_WINDOW_KIND_NORMAL, &second);
    REQUIRE(window >= 0);
    bdk_ext_window_attach(window, g_a);
    ext_pumps(10);
    CHECK(tab_id(g_a) == a);
    probe("events", "{}", out, sizeof out);
    const char* detached = strstr(out, "tabs.onDetached");
    const char* attached = strstr(out, "tabs.onAttached");
    CHECK(detached && attached && detached < attached);
    CHECK(mentions_tab(out, a));
    with_tab("pingContent", a, out, sizeof out);
    CHECK(ext_has(out, "\"succeeded\":true") && ext_has(out, "\"pong\":true"));
}

/* Assertion 7: tabs.sendMessage reaches the content script of both pages. */
static void test_engine_ext_probe_messages_reach_both_pages(void) {
    setup();
    const int a = tab_id(g_a), b = tab_id(g_b);
    REQUIRE(acked(a, b));
    char out[2048];
    with_tab("pingContent", a, out, sizeof out);
    CHECK(ext_has(out, "\"succeeded\":true") && ext_has(out, PAGE_A));
    with_tab("pingContent", b, out, sizeof out);
    CHECK(ext_has(out, "\"succeeded\":true") && ext_has(out, PAGE_B));
}

/* Assertion 10: a page that was never placed is a tab of its profile's parking context, which
   extensions do not see in windows.getAll or tabs.query. */
static void test_engine_ext_probe_parking_context_hidden(void) {
    setup();
    bdk_contents_t c = t_new_contents(g_profile);
    load(c, PAGE_C);
    bdk_testing_tab_info_t info;
    memset(&info, 0, sizeof info);
    info.struct_size = sizeof info;
    REQUIRE(bdk_testing_tab_info(c, &info) == BDK_OK);
    REQUIRE(info.parking);
    char out[4096];
    probe("snapshot", "{}", out, sizeof out);
    CHECK(mentions_tab(out, tab_id(g_a)));
    CHECK(!mentions_tab(out, info.tab_id));
    char context[48];
    snprintf(context, sizeof context, "\"id\":%d,\"focused\"", info.context_id);
    CHECK(!ext_has(out, context));
}

/* Decision D1 (a): a page whose view the application removed stays a background tab of the
   window it was last attached to. */
static void test_engine_ext_probe_offscreen_pages_in_query(void) {
    setup();
    const int a = tab_id(g_a);
    bdk_testing_tab_info_t before;
    memset(&before, 0, sizeof before);
    before.struct_size = sizeof before;
    REQUIRE(bdk_testing_tab_info(g_a, &before) == BDK_OK);
    bdk_native_handle_t view;
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_native_view(g_a, &view, &err) == BDK_OK);
    bdk_ext_remove_view(view);
    ext_pumps(10);
    char out[4096], all[2048], expected[64];
    probe("snapshot", "{}", out, sizeof out);
    section(out, "\"all\":", "\"currentWindowId\":", all, sizeof all);
    CHECK(mentions_tab(all, a));
    snprintf(expected, sizeof expected, "\"id\":%d,\"windowId\":%d", a, before.context_id);
    CHECK(ext_has(all, expected));
}

/* Assertion 5: clicking the action with B focused lets the worker inject into B and not into A;
   navigating B to another origin removes the grant. */
static void test_engine_ext_probe_action_grants_active_tab_to_one_page(void) {
    setup();
    const int a = tab_id(g_a), b = tab_id(g_b);
    REQUIRE(acked(a, b));
    char out[2048];
    with_tab("inject", b, out, sizeof out);
    CHECK(ext_has(out, "\"succeeded\":false")); /* no host permission, no grant yet */

    bdk_buffer_t result = 0;
    bdk_error_t err = 0;
    REQUIRE(bdk_extension_action_activate(g_b, S(g_probe), bdk_harness_engine_gesture(g_b, 0),
                                          &result, &err) == BDK_OK);
    char text[256];
    t_buffer_text(result, text, sizeof text);
    CHECK(ext_has(text, "popupRequested")); /* the probe's action has a popup */
    ext_pumps(10);

    with_tab("inject", b, out, sizeof out);
    CHECK(ext_has(out, "\"succeeded\":true"));
    with_tab("inject", a, out, sizeof out);
    CHECK(ext_has(out, "\"succeeded\":false"));

    load(g_b, PAGE_OTHER_ORIGIN);
    with_tab("inject", b, out, sizeof out);
    CHECK(ext_has(out, "\"succeeded\":false"));
}

const t_test_t t_engine_extension_probe_tests[] = {
    {"engine_ext_probe_worker_and_content_ack", test_engine_ext_probe_worker_and_content_ack},
    {"engine_ext_probe_active_tab_is_focused_page",
     test_engine_ext_probe_active_tab_is_focused_page},
    {"engine_ext_probe_focus_fires_on_activated", test_engine_ext_probe_focus_fires_on_activated},
    {"engine_ext_probe_move_keeps_tab_id_and_events",
     test_engine_ext_probe_move_keeps_tab_id_and_events},
    {"engine_ext_probe_messages_reach_both_pages", test_engine_ext_probe_messages_reach_both_pages},
    {"engine_ext_probe_parking_context_hidden", test_engine_ext_probe_parking_context_hidden},
    {"engine_ext_probe_offscreen_pages_in_query", test_engine_ext_probe_offscreen_pages_in_query},
    {"engine_ext_probe_action_grants_active_tab_to_one_page",
     test_engine_ext_probe_action_grants_active_tab_to_one_page},
    {NULL, NULL},
};
