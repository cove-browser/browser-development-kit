/* Engine scenarios for the extension UI of T-EXT-02 and T-EXT-04: action state and activation,
   popups (the default menu with no handler, a handler that places the secure view, blur, size
   updates, openPopup), the context-menu items of extensions, options pages and declared commands,
   and the assertions 5, 6 and 8 of T-EXT-01 that concern them. Each loads one of the unpacked
   fixtures of tests/extensions/fixtures (names that start with ui-) in the page's profile, through
   the extui.load test verb.

   Pages A and B are tabs of one registered, normal window, B focused. Gestures are the events
   bdk_harness_engine_gesture makes. Scenarios that need the window to be the key window (the
   extension API refuses a popup for an inactive window) are skipped with a message when macOS
   does not let the harness come to the front. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bdk_testing.h"
#include "test_support.h"

#ifndef BDK_EXT_UI_FIXTURES_DIR
#define BDK_EXT_UI_FIXTURES_DIR "tests/extensions/fixtures"
#endif

int bdk_harness_engine_window_open_normal(bdk_runtime_t rt);
bdk_window_t bdk_harness_engine_window_handle(int window);
void bdk_harness_engine_window_attach(int window, bdk_contents_t contents);
void bdk_harness_engine_window_close(int window);
void bdk_harness_engine_place_view(int window, bdk_native_handle_t view, double w, double h);
bdk_native_handle_t bdk_harness_engine_gesture(bdk_contents_t contents, int kind);
int bdk_harness_engine_window_is_key(bdk_contents_t contents);
extern const char* bdk_harness_engine_extensions;

#define PAGE_A "https://bdk.test/ext-ui-a"
#define PAGE_B "https://bdk.test/ext-ui-b"
#define PAGE_C "https://bdk.test/ext-ui-c"

/* ---- what the handlers recorded ---- */

typedef struct {
    int calls;
    bdk_request_t request;
    bdk_contents_t contents;
    bdk_native_handle_t secure_view;
    char payload[8192];
} log_t;

static log_t g_popups, g_menus, g_windows;
static int g_withdrawn;
static char g_updates[16][160];
static int g_update_count;
static char g_event[512];
static int g_event_count;

static bdk_profile_t g_profile;
static int g_window;
static bdk_contents_t g_a, g_b;

static bdk_ui_response_t record(void* ctx, const bdk_ui_request_info_t* info) {
    log_t* log = (log_t*)ctx;
    ++log->calls;
    log->request = info->request;
    log->contents = info->contents;
    log->secure_view = info->secure_view;
    snprintf(log->payload, sizeof log->payload, "%.*s", (int)info->payload_json.length,
             info->payload_json.data ? info->payload_json.data : "");
    return BDK_UI_HANDLED;
}

/* A new-window handler that accepts: the page is the application's. */
static bdk_ui_response_t record_and_accept(void* ctx, const bdk_ui_request_info_t* info) {
    (void)record(ctx, info);
    bdk_native_handle_t none;
    memset(&none, 0, sizeof none);
    bdk_error_t err = 0;
    CHECK(bdk_ui_complete(info->request, S("accept"), S(""), none, &err) == BDK_OK);
    return BDK_UI_HANDLED;
}

static void on_withdrawn(void* ctx) {
    (void)ctx;
    ++g_withdrawn;
}

static void on_update(void* ctx, bdk_request_t request, bdk_string_t update) {
    (void)ctx;
    (void)request;
    if (g_update_count < 16)
        snprintf(g_updates[g_update_count], sizeof g_updates[0], "%.*s", (int)update.length,
                 update.data ? update.data : "");
    ++g_update_count;
}

static void on_popup_event(void* ctx, uint32_t id, bdk_string_t name, bdk_string_t payload) {
    (void)ctx;
    (void)id;
    (void)name;
    ++g_event_count;
    snprintf(g_event, sizeof g_event, "%.*s", (int)payload.length,
             payload.data ? payload.data : "");
}

/* A command's answer, with room for an action icon. */
typedef struct {
    int results;
    int has_error;
    char domain[48], code[48];
    char json[65536];
} big_t;

static void on_big(void* ctx, bdk_error_t error, bdk_string_t json, bdk_buffer_t binary) {
    big_t* b = (big_t*)ctx;
    (void)binary;
    ++b->results;
    b->has_error = error != 0;
    b->domain[0] = b->code[0] = '\0';
    if (error)
        t_error_text(error, b->domain, sizeof b->domain, b->code, sizeof b->code);
    snprintf(b->json, sizeof b->json, "%.*s", (int)json.length, json.data ? json.data : "");
}

static void reset(void) {
    memset(&g_popups, 0, sizeof g_popups);
    memset(&g_menus, 0, sizeof g_menus);
    memset(&g_windows, 0, sizeof g_windows);
    g_withdrawn = g_update_count = g_event_count = 0;
    g_event[0] = '\0';
}

/* ---- helpers ---- */

static void ui(const char* command, const char* args, char* out, size_t n) {
    REQUIRE(bdk_testing_command(t_runtime, S(command), S(args), out, n) == BDK_OK);
}

static void page_ui(bdk_contents_t c, const char* command, const char* extra, char* out, size_t n) {
    char args[512];
    snprintf(args, sizeof args, "{\"contents\":\"%llu\"%s%s}", (unsigned long long)c,
             extra[0] ? "," : "", extra);
    ui(command, args, out, n);
}

static void script(const char* surface, const char* answer) {
    char args[512];
    snprintf(args, sizeof args, "{\"surface\":\"%s\",\"answer\":%s}", surface, answer);
    ui("present.script", args, NULL, 0);
}

static int count_of(const char* json, const char* key) {
    char pattern[64];
    snprintf(pattern, sizeof pattern, "\"%s\":", key);
    const char* at = strstr(json, pattern);
    return at ? atoi(at + strlen(pattern)) : -1;
}

static void pumps(int n) {
    for (int i = 0; i < n; ++i) {
        t_pump();
        bdk_testing_run_for(0.01);
    }
}

static void popup_state(char* out, size_t n) {
    ui("extui.popup", "{}", out, n);
}

/* Pumps until the popups' record counts `key` at least `want`. */
static int wait_popup_count(const char* key, int want) {
    char out[512];
    for (int i = 0; i < 300; ++i) {
        popup_state(out, sizeof out);
        if (count_of(out, key) >= want)
            return 1;
        pumps(1);
    }
    printf("    popup record: %s\n", out);
    return 0;
}

static void title_of(bdk_contents_t c, char* out, size_t n) {
    bdk_contents_state_t st;
    memset(&st, 0, sizeof st);
    st.struct_size = sizeof st;
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_state(c, &st, &err) == BDK_OK);
    snprintf(out, n, "%.*s", (int)st.title.length, st.title.data ? st.title.data : "");
}

static int wait_for_title_prefix(bdk_contents_t c, const char* prefix) {
    char title[256];
    for (int i = 0; i < 300; ++i) {
        title_of(c, title, sizeof title);
        if (strncmp(title, prefix, strlen(prefix)) == 0)
            return 1;
        pumps(1);
    }
    printf("    title is \"%s\", not \"%s...\"\n", title, prefix);
    return 0;
}

static int tab_id_of(bdk_contents_t c) {
    bdk_testing_tab_info_t info;
    memset(&info, 0, sizeof info);
    info.struct_size = sizeof info;
    REQUIRE(bdk_testing_tab_info(c, &info) == BDK_OK);
    return info.tab_id;
}

static void load_page(bdk_contents_t c, const char* url) {
    bdk_error_t err = 0;
    REQUIRE(bdk_harness_script_navigation(t_runtime, url, 0, "ext ui") == BDK_OK);
    REQUIRE(bdk_contents_load(c, S(url), &err) == BDK_OK);
    pumps(3);
}

/* A runtime with every surface at its default, a registered normal window with pages A and B,
   B focused, and handlers for the surfaces the scenario records. */
static void start(int popup_handler, int accept_windows) {
    reset();
    t_start();
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_NEW_WINDOWS,
                               accept_windows ? record_and_accept : record, &g_windows, NULL,
                               &err) == BDK_OK);
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_CONTEXT_MENUS, record, &g_menus, NULL,
                               &err) == BDK_OK);
    if (popup_handler)
        REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_EXTENSION_POPUPS, record, &g_popups, NULL,
                                   &err) == BDK_OK);
    t_freeze();
    g_profile = t_new_profile("Extension UI");
    g_window = bdk_harness_engine_window_open_normal(t_runtime);
    REQUIRE(g_window >= 0);
    g_a = t_new_contents(g_profile);
    g_b = t_new_contents(g_profile);
    bdk_harness_engine_window_attach(g_window, g_a);
    bdk_harness_engine_window_attach(g_window, g_b);
    load_page(g_a, PAGE_A);
    load_page(g_b, PAGE_B);
    REQUIRE(bdk_contents_focus(g_b) == BDK_OK);
    pumps(3);
}

static void stop(void) {
    bdk_harness_engine_window_close(g_window);
}

/* Loads the fixture and returns the extension's identifier. */
static void load_fixture(const char* name, char* id, size_t n) {
    char args[1024], out[1024];
    snprintf(args, sizeof args, "{\"contents\":\"%llu\",\"path\":\"%s/%s\"}",
             (unsigned long long)g_b, BDK_EXT_UI_FIXTURES_DIR, name);
    ui("extui.load", args, NULL, 0);
    for (int i = 0; i < 600; ++i) {
        ui("extui.loaded", "{}", out, sizeof out);
        if (strstr(out, "\"done\":true"))
            break;
        pumps(1);
    }
    REQUIRE(strstr(out, "\"done\":true") != NULL);
    REQUIRE(t_json_string(out, "id", id, n));
    REQUIRE(id[0] != '\0');
    pumps(20); /* the worker starts */
}

static bdk_status_t activate(bdk_contents_t c, const char* id, int gesture_kind, char* result,
                             size_t n, bdk_error_t* err) {
    bdk_native_handle_t gesture;
    memset(&gesture, 0, sizeof gesture);
    if (gesture_kind >= 0)
        gesture = bdk_harness_engine_gesture(c, gesture_kind);
    bdk_buffer_t out = 0;
    bdk_status_t s = bdk_extension_action_activate(c, S(id), gesture, &out, err);
    if (s == BDK_OK) {
        t_buffer_text(out, result, n);
        bdk_release(out);
    }
    return s;
}

static void invoke(const char* command, const char* args, bdk_profile_t profile,
                   bdk_contents_t contents, big_t* big) {
    bdk_invoke_options_t o;
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.profile = profile;
    o.contents = contents;
    memset(big, 0, sizeof *big);
    REQUIRE(bdk_invoke_named(t_runtime, S(command), S(args), &o, on_big, NULL, big, NULL) != 0);
    for (int i = 0; i < 1000 && big->results == 0; ++i)
        pumps(1);
    CHECK(big->results == 1);
}

static int window_is_key(void) {
    return bdk_harness_engine_window_is_key(g_b);
}

/* ---- action state, activation, and the grant (T-EXT-01 assertion 5) ---- */

static void test_engine_ext_ui_actions_report_state_and_badge_changes(void) {
    start(0, 1);
    char id[64];
    load_fixture("ui-action-click", id, sizeof id);
    bdk_invoke_options_t o;
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.profile = g_profile;
    bdk_subscription_t sub = bdk_subscribe_named(t_runtime, S("extensions.actionChanged"), S(""),
                                                 &o, on_popup_event, NULL, NULL);
    REQUIRE(sub != 0);

    big_t big;
    invoke("extensions.actions", "{}", g_profile, 0, &big);
    CHECK(!big.has_error);
    CHECK(strstr(big.json, id) != NULL);
    CHECK(strstr(big.json, "\"title\":\"UI click\"") != NULL);
    CHECK(strstr(big.json, "\"badgeText\":null") != NULL);
    CHECK(strstr(big.json, "\"isEnabled\":true") != NULL);
    CHECK(strstr(big.json, "\"hasPopup\":false") != NULL);
    CHECK(strstr(big.json, "\"isPinned\":") != NULL);

    char result[256];
    bdk_error_t err = 0;
    REQUIRE(activate(g_b, id, 0, result, sizeof result, &err) == BDK_OK);
    CHECK(strstr(result, "\"outcome\":\"dispatchedClick\"") != NULL);
    REQUIRE(wait_for_title_prefix(g_b, "injected:"));

    /* The badge the click set is B's, and the change was reported for B. */
    char b_handle[32];
    snprintf(b_handle, sizeof b_handle, "\"%llu\"", (unsigned long long)g_b);
    for (int i = 0; i < 100 && strstr(g_event, b_handle) == NULL; ++i)
        pumps(1);
    CHECK(g_event_count > 0);
    CHECK(strstr(g_event, id) != NULL);
    CHECK(strstr(g_event, b_handle) != NULL);
    invoke("extensions.actions", "{}", g_profile, g_b, &big);
    CHECK(strstr(big.json, "\"badgeText\":\"7\"") != NULL);
    invoke("extensions.actions", "{}", g_profile, g_a, &big);
    CHECK(strstr(big.json, "\"badgeText\":null") != NULL);
    bdk_unsubscribe(sub);
    stop();
}

static void test_engine_ext_ui_activation_without_a_gesture_fails(void) {
    start(0, 1);
    char id[64], result[256];
    load_fixture("ui-action-click", id, sizeof id);
    bdk_error_t err = 0;
    /* A mouse-up a minute old, a mouse-down, and no event at all. */
    CHECK(activate(g_b, id, 2, result, sizeof result, &err) != BDK_OK);
    CHECK(t_error_is(err, "ui", "gestureRequired"));
    err = 0;
    CHECK(activate(g_b, id, 3, result, sizeof result, &err) != BDK_OK);
    CHECK(t_error_is(err, "ui", "gestureRequired"));
    err = 0;
    CHECK(activate(g_b, id, -1, result, sizeof result, &err) != BDK_OK);
    if (err)
        bdk_error_release(err);
    pumps(5);
    char title[128];
    title_of(g_b, title, sizeof title);
    CHECK(strncmp(title, "injected:", 9) != 0);
    /* An extension that is not installed. */
    err = 0;
    CHECK(activate(g_b, "abcdefghijklmnopabcdefghijklmnop", 0, result, sizeof result, &err) !=
          BDK_OK);
    CHECK(t_error_is(err, "extension", "notFound"));
    stop();
}

static void test_engine_ext_ui_activation_targets_the_focused_page(void) {
    start(0, 1);
    char id[64], result[256], out[256], extra[128];
    load_fixture("ui-action-click", id, sizeof id);
    bdk_error_t err = 0;
    REQUIRE(activate(g_b, id, 0, result, sizeof result, &err) == BDK_OK);
    REQUIRE(wait_for_title_prefix(g_b, "injected:"));
    char title[128];
    title_of(g_a, title, sizeof title);
    CHECK(strncmp(title, "injected:", 9) != 0);
    snprintf(extra, sizeof extra, "\"id\":\"%s\"", id);
    page_ui(g_b, "extui.has_active_tab", extra, out, sizeof out);
    CHECK(strstr(out, "\"granted\":true") != NULL);
    page_ui(g_a, "extui.has_active_tab", extra, out, sizeof out);
    CHECK(strstr(out, "\"granted\":false") != NULL);
    /* Navigating B removes the grant. */
    load_page(g_b, PAGE_C);
    page_ui(g_b, "extui.has_active_tab", extra, out, sizeof out);
    CHECK(strstr(out, "\"granted\":false") != NULL);
    stop();
}

/* ---- popups (T-EXT-01 assertion 6) ---- */

static void place_popup(void) {
    bdk_harness_engine_place_view(g_window, g_popups.secure_view, 300, 200);
    bdk_error_t err = 0;
    CHECK(bdk_ui_update(g_popups.request, S("{\"placed\":true}"), &err) == BDK_OK);
    pumps(3);
}

static void test_engine_ext_ui_popup_opens_from_the_default_menu_with_no_handler(void) {
    start(0, 1);
    char id[64], out[8192];
    load_fixture("ui-action-popup", id, sizeof id);
    /* The default presenters are scripted: the menu picks the extension, the popup shows
       nothing. */
    char answer[160];
    snprintf(answer, sizeof answer, "{\"choose\":\"%s\"}", id);
    script("extension_menu", answer);
    script("extension_popups", "{\"none\":true}");
    bdk_native_handle_t gesture = bdk_harness_engine_gesture(g_b, 0);
    bdk_error_t err = 0;
    REQUIRE(bdk_extension_menu_present(bdk_harness_engine_window_handle(g_window), gesture, &err) ==
            BDK_OK);
    REQUIRE(wait_popup_count("shown", 1));
    ui("present.record", "{\"surface\":\"extension_menu\"}", out, sizeof out);
    CHECK(strstr(out, id) != NULL);
    CHECK(strstr(out, "UI popup") != NULL);
    CHECK(strstr(out, "hasPopup") != NULL);
    ui("present.record", "{\"surface\":\"extension_popups\"}", out, sizeof out);
    CHECK(strstr(out, "extensionName") != NULL);
    CHECK(strstr(out, "userClick") != NULL);
    CHECK(strstr(out, "preferredSize") != NULL);
    /* The popup reports the tab it was opened for. */
    char want[64];
    snprintf(want, sizeof want, "popup-tab:%d", tab_id_of(g_b));
    REQUIRE(wait_for_title_prefix(g_b, "popup-tab:"));
    char title[128];
    title_of(g_b, title, sizeof title);
    CHECK(strcmp(title, want) == 0);
    /* The menu needs the event that asked for it. */
    memset(&gesture, 0, sizeof gesture);
    gesture.kind = BDK_NATIVE_NSEVENT;
    err = 0;
    CHECK(bdk_extension_menu_present(bdk_harness_engine_window_handle(g_window), gesture, &err) !=
          BDK_OK);
    CHECK(t_error_is(err, "ui", "gestureRequired"));
    stop();
}

static void test_engine_ext_ui_popup_with_a_handler_gets_the_secure_view_and_sizes(void) {
    start(1, 1);
    char id[64], result[256], out[512];
    load_fixture("ui-action-popup", id, sizeof id);
    bdk_error_t err = 0;
    REQUIRE(activate(g_b, id, 0, result, sizeof result, &err) == BDK_OK);
    CHECK(strstr(result, "\"outcome\":\"popupRequested\"") != NULL);
    for (int i = 0; i < 300 && g_popups.calls == 0; ++i)
        pumps(1);
    REQUIRE(g_popups.calls == 1);
    CHECK(g_popups.contents == g_b);
    CHECK(strstr(g_popups.payload, id) != NULL);
    CHECK(strstr(g_popups.payload, "\"extensionName\":\"UI Action Popup\"") != NULL);
    CHECK(strstr(g_popups.payload, "\"trigger\":\"userClick\"") != NULL);
    CHECK(strstr(g_popups.payload, "\"preferredSize\"") != NULL);
    CHECK(strstr(g_popups.payload, "popup.html") != NULL);
    CHECK(g_popups.secure_view.kind == BDK_NATIVE_NSVIEW);
    CHECK(g_popups.secure_view.pointer != NULL);
    bdk_ui_set_update_handler(g_popups.request, on_update, NULL, NULL);
    bdk_ui_set_withdrawn_handler(g_popups.request, on_withdrawn, NULL, NULL);
    place_popup();
    /* The page grows from 300x200 to 320x260 after it opened: the new size arrives as an update,
       within Chromium's minimum and maximum. */
    int grown = 0;
    for (int i = 0; i < 400 && !grown; ++i) {
        pumps(1);
        for (int u = 0; u < g_update_count && u < 16; ++u)
            if (strstr(g_updates[u], "\"width\":320") && strstr(g_updates[u], "\"height\":260"))
                grown = 1;
    }
    if (!grown)
        for (int u = 0; u < g_update_count && u < 16; ++u)
            printf("    update: %s\n", g_updates[u]);
    CHECK(grown);
    for (int u = 0; u < g_update_count && u < 16; ++u)
        CHECK(strstr(g_updates[u], "\"preferredSize\"") != NULL);
    popup_state(out, sizeof out);
    CHECK(count_of(out, "shown") >= 1);
    CHECK(g_withdrawn == 0);
    stop();
}

static void test_engine_ext_ui_popup_closes_on_blur_and_the_request_is_withdrawn(void) {
    start(1, 1);
    char id[64], result[256], out[512];
    load_fixture("ui-action-popup", id, sizeof id);
    bdk_error_t err = 0;
    REQUIRE(activate(g_b, id, 0, result, sizeof result, &err) == BDK_OK);
    for (int i = 0; i < 300 && g_popups.calls == 0; ++i)
        pumps(1);
    REQUIRE(g_popups.calls == 1);
    bdk_ui_set_withdrawn_handler(g_popups.request, on_withdrawn, NULL, NULL);
    place_popup();
    page_ui(g_b, "extui.popup", "", out, sizeof out);
    CHECK(strstr(out, "\"open\":true") != NULL);
    page_ui(g_b, "extui.blur", "", NULL, 0);
    pumps(5);
    CHECK(g_withdrawn == 1);
    page_ui(g_b, "extui.popup", "", out, sizeof out);
    CHECK(strstr(out, "\"open\":false") != NULL);
    CHECK(strstr(out, "\"lastCloseReason\":\"blur\"") != NULL);
    /* The request is over: the application can no longer complete it. */
    bdk_native_handle_t none;
    memset(&none, 0, sizeof none);
    err = 0;
    CHECK(bdk_ui_complete(g_popups.request, S("accept"), S("{\"action\":\"close\"}"), none, &err) !=
          BDK_OK);
    if (err)
        bdk_error_release(err);
    stop();
}

static void test_engine_ext_ui_the_application_closes_a_popup_and_navigation_closes_it(void) {
    start(1, 1);
    char id[64], result[256], out[512];
    load_fixture("ui-action-popup", id, sizeof id);
    bdk_error_t err = 0;
    REQUIRE(activate(g_b, id, 0, result, sizeof result, &err) == BDK_OK);
    for (int i = 0; i < 300 && g_popups.calls == 0; ++i)
        pumps(1);
    REQUIRE(g_popups.calls == 1);
    place_popup();
    bdk_native_handle_t none;
    memset(&none, 0, sizeof none);
    REQUIRE(bdk_ui_complete(g_popups.request, S("accept"), S("{\"action\":\"close\"}"), none,
                            &err) == BDK_OK);
    pumps(5);
    page_ui(g_b, "extui.popup", "", out, sizeof out);
    CHECK(strstr(out, "\"open\":false") != NULL);
    CHECK(strstr(out, "\"lastCloseReason\":\"application\"") != NULL);

    /* A second popup closes when the page it was opened for navigates. */
    reset();
    REQUIRE(activate(g_b, id, 0, result, sizeof result, &err) == BDK_OK);
    for (int i = 0; i < 300 && g_popups.calls == 0; ++i)
        pumps(1);
    REQUIRE(g_popups.calls == 1);
    bdk_ui_set_withdrawn_handler(g_popups.request, on_withdrawn, NULL, NULL);
    place_popup();
    load_page(g_b, PAGE_C);
    for (int i = 0; i < 50 && g_withdrawn == 0; ++i)
        pumps(1);
    CHECK(g_withdrawn == 1);
    page_ui(g_b, "extui.popup", "", out, sizeof out);
    CHECK(strstr(out, "\"lastCloseReason\":\"navigated\"") != NULL);
    stop();
}

static void test_engine_ext_ui_a_disabled_popup_surface_blocks_a_popup_only_click(void) {
    bdk_harness_engine_extensions = "{\"ui\":{\"popups\":\"disabled\"}}";
    start(0, 1);
    char id[64], result[256];
    load_fixture("ui-action-popup", id, sizeof id);
    bdk_error_t err = 0;
    REQUIRE(activate(g_b, id, 0, result, sizeof result, &err) == BDK_OK);
    CHECK(strstr(result, "\"outcome\":\"blocked\"") != NULL);
    CHECK(strstr(result, "\"reason\":\"popupsDisabled\"") != NULL);
    pumps(5);
    char out[512];
    popup_state(out, sizeof out);
    CHECK(count_of(out, "opened") == 0);
    stop();
}

static void test_engine_ext_ui_action_open_popup_opens_a_popup(void) {
    start(1, 1);
    if (!window_is_key()) {
        printf("    skipped: the harness window is not the key window\n");
        stop();
        return;
    }
    char id[64];
    load_fixture("ui-action-popup", id, sizeof id);
    char js[512];
    snprintf(
        js, sizeof js,
        "chrome.runtime.sendMessage('%s',{},function(r){document.title='open:'+JSON.stringify(r)})",
        id);
    REQUIRE(bdk_testing_execute_script(g_b, S(js), 1) == BDK_OK);
    for (int i = 0; i < 400 && g_popups.calls == 0; ++i)
        pumps(1);
    REQUIRE(g_popups.calls == 1);
    CHECK(strstr(g_popups.payload, "\"trigger\":\"openPopupAPI\"") != NULL);
    place_popup();
    REQUIRE(wait_for_title_prefix(g_b, "open:"));
    char title[128];
    title_of(g_b, title, sizeof title);
    CHECK(strstr(title, "\"ok\":true") != NULL);
    stop();
}

/* ---- the default menu with no application code ---- */

static void test_engine_ext_ui_a_window_with_no_anchor_gets_a_title_bar_button(void) {
    start(0, 1);
    char id[64], out[512];
    load_fixture("ui-action-click", id, sizeof id);
    pumps(10);
    page_ui(g_b, "extui.titlebar", "", out, sizeof out);
    CHECK(strstr(out, "\"present\":true") != NULL);
    CHECK(strstr(out, "\"visible\":true") != NULL);
    /* In the title bar: above the content, at the trailing edge. */
    CHECK(count_of(out, "width") > 0);
    CHECK(count_of(out, "y") < 40);
    CHECK(count_of(out, "x") > 300);
    stop();
}

static uint8_t anchor_provider(void* ctx, bdk_window_t window, uint32_t role, double r[4]) {
    (void)ctx;
    (void)window;
    if (role != BDK_ANCHOR_EXTENSIONS)
        return 0;
    r[0] = 500;
    r[1] = 4;
    r[2] = 30;
    r[3] = 24;
    return 1;
}

static void test_engine_ext_ui_a_registered_anchor_replaces_the_title_bar_button(void) {
    reset();
    t_start_frozen();
    g_profile = t_new_profile("Extension UI anchor");
    g_window = bdk_harness_engine_window_open_normal(t_runtime);
    REQUIRE(g_window >= 0);
    bdk_error_t err = 0;
    REQUIRE(bdk_window_set_anchor_provider(bdk_harness_engine_window_handle(g_window),
                                           anchor_provider, NULL, NULL, &err) == BDK_OK);
    g_b = t_new_contents(g_profile);
    bdk_harness_engine_window_attach(g_window, g_b);
    load_page(g_b, PAGE_B);
    char id[64], out[512];
    load_fixture("ui-action-click", id, sizeof id);
    pumps(10);
    page_ui(g_b, "extui.titlebar", "", out, sizeof out);
    CHECK(strstr(out, "\"present\":false") != NULL);
    stop();
}

/* ---- context-menu items (T-EXT-01 assertion 8, T-EXT-04) ---- */

static int right_click_for_menu(void) {
    g_menus.calls = 0;
    REQUIRE(bdk_testing_click(g_b, 20, 20, 2) == BDK_OK);
    for (int i = 0; i < 200 && g_menus.calls == 0; ++i)
        pumps(1);
    return g_menus.calls == 1;
}

static void test_engine_ext_ui_a_context_menu_item_is_listed_performed_and_grants_active_tab(void) {
    start(0, 1);
    char id[64], extra[128], out[256];
    load_fixture("ui-context-menu", id, sizeof id);
    snprintf(extra, sizeof extra, "\"id\":\"%s\"", id);
    for (int i = 0; i < 300; ++i) {
        page_ui(g_b, "extui.context_items", extra, out, sizeof out);
        if (count_of(out, "count") > 0)
            break;
        pumps(1);
    }
    REQUIRE(count_of(out, "count") == 1);
    REQUIRE(right_click_for_menu());
    CHECK(strstr(g_menus.payload, "\"source\":\"extension\"") != NULL);
    char want[96];
    snprintf(want, sizeof want, "\"extensionId\":\"%s\"", id);
    CHECK(strstr(g_menus.payload, want) != NULL);
    CHECK(strstr(g_menus.payload, "\"title\":\"UI item\"") != NULL);
    /* The item's id, to perform it. */
    const char* at = strstr(g_menus.payload, "\"id\":\"ext:");
    REQUIRE(at != NULL);
    char item[32];
    REQUIRE(sscanf(at, "\"id\":\"%31[^\"]\"", item) == 1);
    char args[128];
    snprintf(args, sizeof args, "{\"action\":\"perform\",\"item\":\"%s\"}", item);
    bdk_native_handle_t none;
    memset(&none, 0, sizeof none);
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_complete(g_menus.request, S("accept"), S(args), none, &err) == BDK_OK);
    /* onClicked fired for B: its title names B's tab. */
    char title_want[64];
    snprintf(title_want, sizeof title_want, "menu-click:%d", tab_id_of(g_b));
    REQUIRE(wait_for_title_prefix(g_b, "menu-click:"));
    char title[128];
    title_of(g_b, title, sizeof title);
    CHECK(strcmp(title, title_want) == 0);
    page_ui(g_b, "extui.has_active_tab", extra, out, sizeof out);
    CHECK(strstr(out, "\"granted\":true") != NULL);
    page_ui(g_a, "extui.has_active_tab", extra, out, sizeof out);
    CHECK(strstr(out, "\"granted\":false") != NULL);
    stop();
}

static void test_engine_ext_ui_a_disabled_context_menu_surface_leaves_the_items_out(void) {
    bdk_harness_engine_extensions = "{\"ui\":{\"context_menus\":\"disabled\"}}";
    start(0, 1);
    char id[64], extra[128], out[256];
    load_fixture("ui-context-menu", id, sizeof id);
    snprintf(extra, sizeof extra, "\"id\":\"%s\"", id);
    for (int i = 0; i < 100; ++i) {
        page_ui(g_b, "extui.context_items", extra, out, sizeof out);
        if (count_of(out, "count") > 0)
            break;
        pumps(1);
    }
    REQUIRE(right_click_for_menu());
    CHECK(strstr(g_menus.payload, "\"source\":\"extension\"") == NULL);
    CHECK(strstr(g_menus.payload, "\"id\":\"reload\"") != NULL);
    stop();
}

/* ---- options pages (T-EXT-04) ---- */

static void test_engine_ext_ui_openoptions_raises_a_new_page_request_with_the_extension(void) {
    start(0, 1);
    char id[64], args[128];
    load_fixture("ui-options", id, sizeof id);
    snprintf(args, sizeof args, "{\"id\":\"%s\"}", id);
    big_t big;
    invoke("extensions.openOptions", args, g_profile, 0, &big);
    CHECK(!big.has_error);
    for (int i = 0; i < 100 && g_windows.calls == 0; ++i)
        pumps(1);
    REQUIRE(g_windows.calls == 1);
    CHECK(strstr(g_windows.payload, "\"kind\":\"newPage\"") != NULL);
    char want[160];
    snprintf(want, sizeof want, "\"initiator\":{\"extensionId\":\"%s\"}", id);
    CHECK(strstr(g_windows.payload, want) != NULL);
    snprintf(want, sizeof want, "chrome-extension://%s/options.html", id);
    CHECK(strstr(g_windows.payload, want) != NULL);
    stop();
}

static void test_engine_ext_ui_chrome_runtime_openoptionspage_raises_the_same_request(void) {
    start(0, 1);
    char id[64], result[256];
    load_fixture("ui-options", id, sizeof id);
    bdk_error_t err = 0;
    REQUIRE(activate(g_b, id, 0, result, sizeof result, &err) == BDK_OK);
    for (int i = 0; i < 300 && g_windows.calls == 0; ++i)
        pumps(1);
    REQUIRE(g_windows.calls == 1);
    CHECK(strstr(g_windows.payload, "\"kind\":\"newPage\"") != NULL);
    CHECK(strstr(g_windows.payload, "\"extensionId\"") != NULL);
    CHECK(strstr(g_windows.payload, "/options.html") != NULL);
    stop();
}

static void test_engine_ext_ui_a_disabled_options_surface_refuses_openoptions(void) {
    bdk_harness_engine_extensions = "{\"ui\":{\"options_pages\":\"disabled\"}}";
    start(0, 1);
    char id[64], args[128];
    load_fixture("ui-options", id, sizeof id);
    snprintf(args, sizeof args, "{\"id\":\"%s\"}", id);
    big_t big;
    invoke("extensions.openOptions", args, g_profile, 0, &big);
    CHECK(big.has_error);
    CHECK(strcmp(big.domain, "extension") == 0);
    CHECK(strcmp(big.code, "surfaceDisabled") == 0);
    pumps(5);
    CHECK(g_windows.calls == 0);
    stop();
}

/* ---- commands ---- */

static void test_engine_ext_ui_a_declared_command_fires_oncommand_for_the_page(void) {
    start(0, 1);
    char id[64], result[256];
    load_fixture("ui-command", id, sizeof id);
    bdk_native_handle_t gesture = bdk_harness_engine_gesture(g_b, 0);
    bdk_buffer_t out = 0;
    bdk_error_t err = 0;
    REQUIRE(bdk_extension_command_perform(g_b, S(id), S("run-it"), gesture, &out, &err) == BDK_OK);
    t_buffer_text(out, result, sizeof result);
    bdk_release(out);
    CHECK(strstr(result, "\"outcome\":\"dispatchedClick\"") != NULL);
    char want[96];
    snprintf(want, sizeof want, "command:run-it:%d", tab_id_of(g_b));
    REQUIRE(wait_for_title_prefix(g_b, "command:run-it:"));
    char title[128];
    title_of(g_b, title, sizeof title);
    CHECK(strcmp(title, want) == 0);
    /* A command the extension does not declare. */
    gesture = bdk_harness_engine_gesture(g_b, 0);
    err = 0;
    CHECK(bdk_extension_command_perform(g_b, S(id), S("nope"), gesture, &out, &err) != BDK_OK);
    CHECK(t_error_is(err, "extension", "notFound"));
    stop();
}

const t_test_t t_engine_extension_ui_tests[] = {
    {"engine_ext_ui_actions_report_state_and_badge_changes",
     test_engine_ext_ui_actions_report_state_and_badge_changes},
    {"engine_ext_ui_activation_without_a_gesture_fails",
     test_engine_ext_ui_activation_without_a_gesture_fails},
    {"engine_ext_ui_activation_targets_the_focused_page",
     test_engine_ext_ui_activation_targets_the_focused_page},
    {"engine_ext_ui_popup_opens_from_the_default_menu_with_no_handler",
     test_engine_ext_ui_popup_opens_from_the_default_menu_with_no_handler},
    {"engine_ext_ui_popup_with_a_handler_gets_the_secure_view_and_sizes",
     test_engine_ext_ui_popup_with_a_handler_gets_the_secure_view_and_sizes},
    {"engine_ext_ui_popup_closes_on_blur_and_the_request_is_withdrawn",
     test_engine_ext_ui_popup_closes_on_blur_and_the_request_is_withdrawn},
    {"engine_ext_ui_the_application_closes_a_popup_and_navigation_closes_it",
     test_engine_ext_ui_the_application_closes_a_popup_and_navigation_closes_it},
    {"engine_ext_ui_a_disabled_popup_surface_blocks_a_popup_only_click",
     test_engine_ext_ui_a_disabled_popup_surface_blocks_a_popup_only_click},
    {"engine_ext_ui_action_open_popup_opens_a_popup",
     test_engine_ext_ui_action_open_popup_opens_a_popup},
    {"engine_ext_ui_a_window_with_no_anchor_gets_a_title_bar_button",
     test_engine_ext_ui_a_window_with_no_anchor_gets_a_title_bar_button},
    {"engine_ext_ui_a_registered_anchor_replaces_the_title_bar_button",
     test_engine_ext_ui_a_registered_anchor_replaces_the_title_bar_button},
    {"engine_ext_ui_a_context_menu_item_is_listed_performed_and_grants_active_tab",
     test_engine_ext_ui_a_context_menu_item_is_listed_performed_and_grants_active_tab},
    {"engine_ext_ui_a_disabled_context_menu_surface_leaves_the_items_out",
     test_engine_ext_ui_a_disabled_context_menu_surface_leaves_the_items_out},
    {"engine_ext_ui_openoptions_raises_a_new_page_request_with_the_extension",
     test_engine_ext_ui_openoptions_raises_a_new_page_request_with_the_extension},
    {"engine_ext_ui_chrome_runtime_openoptionspage_raises_the_same_request",
     test_engine_ext_ui_chrome_runtime_openoptionspage_raises_the_same_request},
    {"engine_ext_ui_a_disabled_options_surface_refuses_openoptions",
     test_engine_ext_ui_a_disabled_options_surface_refuses_openoptions},
    {"engine_ext_ui_a_declared_command_fires_oncommand_for_the_page",
     test_engine_ext_ui_a_declared_command_fires_oncommand_for_the_page},
    {NULL, NULL},
};
