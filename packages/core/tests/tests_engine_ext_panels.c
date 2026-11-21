/* Engine scenarios for windows, side panels and DevTools extension pages (T-SWF-08, T-EXT-03,
   T-EXT-08): the registered window's kind and attachments as extensions see them, window requests
   from windows.update and windows.remove, default presenters attached to a window's anchor, the
   extension side panel in BDK's default host and in an application's container, and the panel
   that a `devtools_page` adds to undocked DevTools. Extensions are the unpacked fixtures of
   tests/extensions/fixtures (BDK_FIXTURES_DIR), loaded without the install prompt through the
   engine's test hooks (bdk_testing.h: extensions.load, extensions.eval). Built only into the engine
   conformance app. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bdk_testing.h"
#include "test_support.h"

void bdk_harness_engine_show(bdk_contents_t contents, int visible);
extern const char* bdk_harness_engine_extensions;

/* From harness_engine_ext.mm. */
int bdk_ext_window_open(bdk_runtime_t rt, uint32_t kind, bdk_window_t* handle);
void bdk_ext_window_attach(int window, bdk_contents_t contents);
void bdk_ext_window_close(int window);
void bdk_ext_window_frame(int window, double out[4]);
void bdk_ext_window_content_size(int window, double out[2]);
int bdk_ext_window_is_minimized(int window);
int bdk_ext_place_view(int window, bdk_native_handle_t view, double x, double y, double width,
                       double height);
int bdk_ext_view_in_window(bdk_native_handle_t view);
void bdk_ext_remove_view(bdk_native_handle_t view);

#define PAGE "https://bdk.test/panels"

/* ---- helpers ---- */

static void cmd(const char* name, const char* args, char* out, size_t n) {
    REQUIRE(bdk_testing_command(t_runtime, S(name), S(args), out, n) == BDK_OK);
}

static void pumps(int n) {
    for (int i = 0; i < n; ++i)
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

static int wait_for_title(bdk_contents_t c, const char* want) {
    char title[256];
    for (int i = 0; i < 80; ++i) {
        title_of(c, title, sizeof title);
        if (strcmp(title, want) == 0)
            return 1;
        t_pump();
    }
    printf("    title is \"%s\", not \"%s\"\n", title, want);
    return 0;
}

static void run_script(bdk_contents_t c, const char* js, int gesture) {
    REQUIRE(bdk_testing_execute_script(c, S(js), (uint8_t)gesture) == BDK_OK);
    t_pump();
}

/* The JSON text with the backslashes before quotes removed, so a result that carries JSON inside a
   JSON string can be searched for plain fragments. */
static void unescape(char* text) {
    char* w = text;
    for (const char* r = text; *r; ++r) {
        if (r[0] == '\\' && r[1] == '"')
            continue;
        *w++ = *r;
    }
    *w = 0;
}

static int has(const char* text, const char* fragment) {
    return strstr(text, fragment) != NULL;
}

static int number_after(const char* text, const char* key) {
    const char* at = strstr(text, key);
    return at ? atoi(at + strlen(key)) : -999999;
}

/* Starts a runtime with the extension configuration `extensions` (or none), a frozen registry and
   one page. `register_handlers` runs after the start and before the freeze. */
static bdk_contents_t g_page;
static bdk_profile_t g_profile;

static bdk_contents_t start(const char* extensions, void (*register_handlers)(void)) {
    bdk_harness_engine_extensions = extensions;
    t_start();
    if (register_handlers)
        register_handlers();
    t_freeze();
    g_profile = t_new_profile("Panels");
    g_page = t_new_contents(g_profile);
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_load(g_page, S(PAGE), &err) == BDK_OK);
    pumps(3);
    return g_page;
}

static void load_extension(bdk_contents_t c, const char* name, char* id, size_t n) {
    char args[512], out[512];
    snprintf(args, sizeof args, "{\"contents\":\"%llu\",\"path\":\"%s/%s\"}", (unsigned long long)c,
             BDK_FIXTURES_DIR, name);
    cmd("extensions.load", args, NULL, 0);
    id[0] = 0;
    for (int i = 0; i < 400 && !id[0]; ++i) {
        t_pump();
        cmd("extensions.loaded", "{}", out, sizeof out);
        if (has(out, "\"done\":true")) {
            if (!t_json_string(out, "id", id, n) || !id[0]) {
                printf("    extension %s did not load: %s\n", name, out);
                REQUIRE(0);
            }
        }
    }
    REQUIRE(id[0]);
    /* Its service worker starts, and so listens for what the test asks. */
    pumps(20);
}

/* Runs `script` in the extension's service worker and waits for its value (the raw result
   object); `out` has its quotes unescaped. */
static void eval_in(bdk_contents_t c, const char* ext, const char* script, char* out, size_t n) {
    char args[1536];
    snprintf(args, sizeof args, "{\"contents\":\"%llu\",\"extension\":\"%s\",\"script\":\"%s\"}",
             (unsigned long long)c, ext, script);
    cmd("extensions.eval", args, NULL, 0);
    for (int i = 0; i < 100; ++i) {
        cmd("extensions.eval.result", "{}", out, n);
        if (has(out, "\"done\":true")) {
            unescape(out);
            return;
        }
        t_pump();
    }
    REQUIRE(0);
}

/* Runs a promise-returning expression in the service worker and returns what it resolved to, as
   JSON text with quotes unescaped. */
static void ask_worker(bdk_contents_t c, const char* ext, const char* promise_expr, char* out,
                       size_t n) {
    char script[1024];
    snprintf(script, sizeof script,
             "globalThis.out=undefined;(%s).then(v=>{globalThis.out=JSON.stringify(v)});1",
             promise_expr);
    eval_in(c, ext, script, out, n);
    for (int i = 0; i < 100; ++i) {
        eval_in(c, ext, "globalThis.out===undefined?'':globalThis.out", out, n);
        if (!has(out, "\"value\":\"\""))
            return;
        t_pump();
    }
    REQUIRE(0);
}

static void unregister_window(bdk_window_t w) {
    bdk_error_t err = 0;
    REQUIRE(bdk_window_unregister(w, &err) == BDK_OK);
}

static bdk_native_handle_t no_gesture(void) {
    bdk_native_handle_t none;
    memset(&none, 0, sizeof none);
    return none;
}

static char* state_of(bdk_window_t w, char* out, size_t n) {
    char args[64];
    snprintf(args, sizeof args, "{\"window\":\"%llu\"}", (unsigned long long)w);
    cmd("sidepanel.state", args, out, n);
    return out;
}

static int panel_is(bdk_window_t w, const char* fragment) {
    char out[1024];
    for (int i = 0; i < 100; ++i) {
        state_of(w, out, sizeof out);
        if (has(out, fragment))
            return 1;
        t_pump();
    }
    printf("    panel state is %s, wanted %s\n", out, fragment);
    return 0;
}

static bdk_contents_t extension_page(bdk_contents_t c, const char* ext, const char* file) {
    char url[256];
    snprintf(url, sizeof url, "chrome-extension://%s/%s", ext, file);
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_load(c, S(url), &err) == BDK_OK);
    for (int i = 0; i < 80; ++i)
        t_pump();
    return c;
}

/* ---- windows: kind and attachments ---- */

static void test_ext_a_registered_popup_window_reports_type_popup(void) {
    bdk_contents_t c = start(NULL, NULL);
    char ext[64], out[2048];
    load_extension(c, "panel-windows", ext, sizeof ext);
    bdk_window_t w = 0;
    int window = bdk_ext_window_open(t_runtime, BDK_WINDOW_KIND_POPUP, &w);
    REQUIRE(window >= 0);
    bdk_ext_window_attach(window, c);
    pumps(5);
    ask_worker(c, ext, "chrome.windows.getAll({populate:true})", out, sizeof out);
    CHECK(has(out, "\"type\":\"popup\""));
    CHECK(!has(out, "\"type\":\"normal\""));
    char args[64];
    snprintf(args, sizeof args, "{\"contents\":\"%llu\"}", (unsigned long long)c);
    cmd("window.extension_windows", args, out, sizeof out);
    CHECK(has(out, "\"type\":\"popup\""));
    unregister_window(w);
    bdk_ext_window_close(window);
}

static void test_ext_a_registered_normal_window_reports_type_normal(void) {
    bdk_contents_t c = start(NULL, NULL);
    char ext[64], out[2048];
    load_extension(c, "panel-windows", ext, sizeof ext);
    bdk_window_t w = 0;
    int window = bdk_ext_window_open(t_runtime, BDK_WINDOW_KIND_NORMAL, &w);
    REQUIRE(window >= 0);
    bdk_ext_window_attach(window, c);
    pumps(5);
    ask_worker(c, ext, "chrome.windows.getAll()", out, sizeof out);
    CHECK(has(out, "\"type\":\"normal\""));
    CHECK(!has(out, "\"type\":\"popup\""));
    unregister_window(w);
    bdk_ext_window_close(window);
}

static void test_ext_window_attachments_match_what_extensions_see(void) {
    bdk_contents_t first = start(NULL, NULL);
    char ext[64], out[2048];
    load_extension(first, "panel-windows", ext, sizeof ext);
    bdk_contents_t second = t_new_contents(g_profile);
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_load(second, S(PAGE "2"), &err) == BDK_OK);
    bdk_window_t w = 0;
    int window = bdk_ext_window_open(t_runtime, BDK_WINDOW_KIND_NORMAL, &w);
    REQUIRE(window >= 0);
    bdk_ext_window_attach(window, first);
    bdk_ext_window_attach(window, second);
    REQUIRE(bdk_contents_focus(second) == BDK_OK);
    pumps(5);

    bdk_window_t of_first = 0, of_second = 0;
    REQUIRE(bdk_window_for_contents(first, &of_first, &err) == BDK_OK);
    REQUIRE(bdk_window_for_contents(second, &of_second, &err) == BDK_OK);
    CHECK(of_first == w);
    CHECK(of_second == w);

    bdk_buffer_t list = 0;
    REQUIRE(bdk_window_contents(w, &list, &err) == BDK_OK);
    char text[128], want[128];
    t_buffer_text(list, text, sizeof text);
    bdk_release(list);
    snprintf(want, sizeof want, "[\"%llu\",\"%llu\"]", (unsigned long long)first,
             (unsigned long long)second);
    CHECK(strcmp(text, want) == 0); /* in the order they arrived */

    bdk_contents_t active = 0;
    REQUIRE(bdk_window_active_contents(w, &active, &err) == BDK_OK);
    CHECK(active == second);

    int32_t window_id = -1;
    REQUIRE(bdk_window_extension_id(w, &window_id, &err) == BDK_OK);
    CHECK(window_id >= 0);

    /* What the extension sees: two tabs of that window, the second active. */
    bdk_testing_tab_info_t ti;
    memset(&ti, 0, sizeof ti);
    ti.struct_size = sizeof ti;
    REQUIRE(bdk_testing_tab_info(second, &ti) == BDK_OK);
    ask_worker(first, ext, "chrome.tabs.query({active:true})", out, sizeof out);
    CHECK(number_after(out, "\"id\":") == ti.tab_id);
    CHECK(number_after(out, "\"windowId\":") == window_id);
    ask_worker(first, ext, "chrome.tabs.query({})", out, sizeof out);
    CHECK(has(out, "\"active\":true"));
    CHECK(has(out, "\"active\":false"));

    /* A page that is in no window is attached to none; an unregistered window answers none. */
    bdk_contents_t lone = t_new_contents(g_profile);
    bdk_window_t none = 7;
    REQUIRE(bdk_window_for_contents(lone, &none, &err) == BDK_OK);
    CHECK(none == 0);
    unregister_window(w);
    REQUIRE(bdk_window_for_contents(first, &none, &err) == BDK_OK);
    CHECK(none == 0);
    bdk_ext_window_close(window);
}

static void test_ext_the_parking_context_is_hidden_from_extensions(void) {
    bdk_contents_t c = start(NULL, NULL);
    char ext[64], out[2048], args[64];
    load_extension(c, "panel-windows", ext, sizeof ext);
    /* The page waits in the parking context: no window, no tab to an extension (T-EXT-01 assertion
       10: windows.getAll does not list the parking context). */
    ask_worker(c, ext, "chrome.windows.getAll({populate:true})", out, sizeof out);
    CHECK(strstr(out, "\"value\":\"[]\"") != NULL);
    ask_worker(c, ext, "chrome.tabs.query({})", out, sizeof out);
    CHECK(strstr(out, "\"value\":\"[]\"") != NULL);
    snprintf(args, sizeof args, "{\"contents\":\"%llu\"}", (unsigned long long)c);
    cmd("window.extension_windows", args, out, sizeof out);
    CHECK(has(out, "\"windows\":[]"));

    bdk_window_t w = 0;
    int window = bdk_ext_window_open(t_runtime, BDK_WINDOW_KIND_NORMAL, &w);
    REQUIRE(window >= 0);
    bdk_ext_window_attach(window, c);
    pumps(5);
    cmd("window.extension_windows", args, out, sizeof out);
    CHECK(number_after(out, "\"id\":") >= 0);
    CHECK(!has(out, "\"windows\":[]"));
    unregister_window(w);
    bdk_ext_window_close(window);
    pumps(5);
    /* The page is parked again, and hidden again. */
    cmd("window.extension_windows", args, out, sizeof out);
    CHECK(has(out, "\"windows\":[]"));
}

/* ---- window requests ---- */

typedef struct {
    int calls;
    char last[512];
    char all[2048];
    bdk_ui_response_t answer;
} window_log_t;

static window_log_t g_wlog;

static bdk_ui_response_t on_window_request(void* ctx, bdk_window_t window, bdk_string_t json) {
    (void)window;
    window_log_t* log = (window_log_t*)ctx;
    ++log->calls;
    snprintf(log->last, sizeof log->last, "%.*s", (int)json.length, json.data);
    size_t used = strlen(log->all);
    snprintf(log->all + used, sizeof log->all - used, "%s;", log->last);
    return log->answer;
}

static int open_window_with_requests(bdk_contents_t c, bdk_window_t* w, bdk_ui_response_t answer) {
    memset(&g_wlog, 0, sizeof g_wlog);
    g_wlog.answer = answer;
    int window = bdk_ext_window_open(t_runtime, BDK_WINDOW_KIND_NORMAL, w);
    REQUIRE(window >= 0);
    bdk_error_t err = 0;
    REQUIRE(bdk_window_set_request_handler(*w, on_window_request, &g_wlog, NULL, &err) == BDK_OK);
    bdk_ext_window_attach(window, c);
    pumps(5);
    return window;
}

static void test_ext_windows_update_reaches_the_handler_and_use_default_applies_it(void) {
    bdk_contents_t c = start(NULL, NULL);
    char ext[64], out[1024];
    load_extension(c, "panel-windows", ext, sizeof ext);
    bdk_window_t w = 0;
    int window = open_window_with_requests(c, &w, BDK_UI_USE_DEFAULT);
    ask_worker(c, ext,
               "chrome.windows.getCurrent().then(w=>chrome.windows.update(w.id,"
               "{left:140,top:130,width:520,height:410}))",
               out, sizeof out);
    for (int i = 0; i < 20 && g_wlog.calls == 0; ++i)
        t_pump();
    REQUIRE(g_wlog.calls >= 1);
    CHECK(has(g_wlog.all, "\"kind\":\"setFrame\""));
    CHECK(has(g_wlog.all, "\"width\":520"));
    CHECK(has(g_wlog.all, "\"height\":410"));
    pumps(5);
    double frame[4];
    bdk_ext_window_frame(window, frame);
    CHECK((int)frame[2] == 520 && (int)frame[3] == 410); /* BDK applied it to the NSWindow */

    /* Minimising is a request too, and applied. */
    g_wlog.calls = 0;
    ask_worker(c, ext,
               "chrome.windows.getCurrent().then(w=>chrome.windows.update(w.id,"
               "{state:'minimized'}))",
               out, sizeof out);
    for (int i = 0; i < 20 && !has(g_wlog.all, "\"kind\":\"minimize\""); ++i)
        t_pump();
    CHECK(has(g_wlog.all, "\"kind\":\"minimize\""));
    pumps(5);
    CHECK(bdk_ext_window_is_minimized(window));
    unregister_window(w);
    bdk_ext_window_close(window);
}

static void test_ext_a_rejected_window_request_is_not_applied(void) {
    bdk_contents_t c = start(NULL, NULL);
    char ext[64], out[1024];
    load_extension(c, "panel-windows", ext, sizeof ext);
    bdk_window_t w = 0;
    int window = open_window_with_requests(c, &w, BDK_UI_REJECT);
    double before[4], after[4];
    bdk_ext_window_frame(window, before);
    ask_worker(c, ext,
               "chrome.windows.getCurrent().then(w=>chrome.windows.update(w.id,"
               "{left:200,top:200,width:500,height:400}))",
               out, sizeof out);
    for (int i = 0; i < 20 && g_wlog.calls == 0; ++i)
        t_pump();
    CHECK(g_wlog.calls >= 1);
    pumps(10);
    bdk_ext_window_frame(window, after);
    CHECK(before[0] == after[0] && before[1] == after[1] && before[2] == after[2] &&
          before[3] == after[3]);
    unregister_window(w);
    bdk_ext_window_close(window);
}

static void test_ext_windows_remove_raises_close_but_moving_a_page_out_does_not(void) {
    bdk_contents_t c = start(NULL, NULL);
    char ext[64], out[1024];
    load_extension(c, "panel-windows", ext, sizeof ext);
    bdk_window_t w = 0;
    int first = open_window_with_requests(c, &w, BDK_UI_REJECT);
    /* The page moves to another window: the first context's tab strip empties, and Chromium closes
       the window it thinks it has. That is no request. */
    bdk_window_t other = 0;
    int second = bdk_ext_window_open(t_runtime, BDK_WINDOW_KIND_NORMAL, &other);
    REQUIRE(second >= 0);
    bdk_ext_window_attach(second, c);
    pumps(10);
    CHECK(!has(g_wlog.all, "\"kind\":\"close\""));
    /* An extension removing the window is one. */
    bdk_ext_window_attach(first, c);
    pumps(5);
    ask_worker(c, ext, "chrome.windows.getCurrent().then(w=>chrome.windows.remove(w.id))", out,
               sizeof out);
    for (int i = 0; i < 20 && !has(g_wlog.all, "\"kind\":\"close\""); ++i)
        t_pump();
    CHECK(has(g_wlog.all, "\"kind\":\"close\""));
    unregister_window(w);
    unregister_window(other);
    bdk_ext_window_close(first);
    bdk_ext_window_close(second);
}

/* ---- default presenters at an anchor (T-SWF-08) ---- */

static uint8_t on_anchor(void* ctx, bdk_window_t window, uint32_t role, double out[4]) {
    (void)ctx;
    (void)window;
    if (role != BDK_ANCHOR_ADDRESS_BAR)
        return 0;
    out[0] = 40;
    out[1] = 12;
    out[2] = 300;
    out[3] = 28;
    return 1;
}

static void test_ext_a_default_permission_prompt_attaches_to_the_supplied_anchor(void) {
    bdk_contents_t c = start(NULL, NULL);
    bdk_window_t w = 0;
    int window = bdk_ext_window_open(t_runtime, BDK_WINDOW_KIND_NORMAL, &w);
    REQUIRE(window >= 0);
    bdk_error_t err = 0;
    REQUIRE(bdk_window_set_anchor_provider(w, on_anchor, NULL, NULL, &err) == BDK_OK);
    bdk_ext_window_attach(window, c);
    REQUIRE(bdk_contents_focus(c) == BDK_OK);
    pumps(5);
    run_script(c, "Notification.requestPermission().then(p=>document.title=p)", 1);
    char out[512];
    int attached = 0;
    for (int i = 0; i < 60 && !attached; ++i) {
        cmd("prompt.anchored", "{}", out, sizeof out);
        attached = has(out, "\"attached\":true");
        t_pump();
    }
    REQUIRE(attached);
    /* Just under the anchor (x 40, y 12 + 28 + 4): the prompt is placed from it, not a sheet. */
    CHECK(has(out, "\"frame\":[40.0,44.0,"));
    cmd("prompt.anchored.answer", "{\"index\":0}", NULL, 0); /* Allow */
    CHECK(wait_for_title(c, "granted"));
    unregister_window(w);
    bdk_ext_window_close(window);
}

/* ---- side panels ---- */

static void open_panel_from_page(bdk_contents_t c, const char* ext, const char* what) {
    char js[128];
    extension_page(c, ext, "opener.html");
    snprintf(js, sizeof js, "run('%s')", what);
    run_script(c, js, 1);
}

static bdk_contents_t start_panel_page(const char* extensions, void (*handlers)(void), int* window,
                                       bdk_window_t* w, char* ext, size_t n) {
    bdk_contents_t c = start(extensions, handlers);
    load_extension(c, "panel-sidepanel", ext, n);
    *window = bdk_ext_window_open(t_runtime, BDK_WINDOW_KIND_NORMAL, w);
    REQUIRE(*window >= 0);
    bdk_ext_window_attach(*window, c);
    REQUIRE(bdk_contents_focus(c) == BDK_OK);
    pumps(5);
    return c;
}

static void test_ext_side_panel_open_shows_the_default_panel_at_the_trailing_edge(void) {
    int window;
    bdk_window_t w;
    char ext[64], out[1024];
    bdk_contents_t c = start_panel_page(NULL, NULL, &window, &w, ext, sizeof ext);
    open_panel_from_page(c, ext, "openWindow");
    REQUIRE(wait_for_title(c, "ok:null"));
    REQUIRE(panel_is(w, "\"open\":true"));
    state_of(w, out, sizeof out);
    CHECK(has(out, "\"defaultPresenter\":true"));
    CHECK(has(out, "\"pageScoped\":false"));
    CHECK(has(out, "\"visible\":true"));
    CHECK(has(out, "panel.html"));
    /* On the trailing edge, at Chromium's default width, full height. */
    double size[2];
    bdk_ext_window_content_size(window, size);
    const char* frame = strstr(out, "\"frame\":[");
    REQUIRE(frame != NULL);
    double x, y, fw, fh;
    REQUIRE(sscanf(frame, "\"frame\":[%lf,%lf,%lf,%lf]", &x, &y, &fw, &fh) == 4);
    CHECK((int)(x + fw) == (int)size[0]);
    CHECK((int)fw == 360);
    CHECK((int)fh == (int)size[1]);
    /* The presenter was asked, the panel page loaded and sidePanel.onOpened went out, and the
       layout reports the side. */
    cmd("present.record", "{\"surface\":\"extension_side_panels\"}", out, sizeof out);
    CHECK(has(out, "\"shown\":1"));
    CHECK(panel_is(w, "\"opened\":true"));
    open_panel_from_page(c, ext, "layout");
    CHECK(wait_for_title(c, "ok:\"right\""));
    /* Dragging the divider resizes the panel and BDK remembers the width in the profile. */
    char args[96];
    snprintf(args, sizeof args, "{\"window\":\"%llu\",\"delta\":-60}", (unsigned long long)w);
    cmd("sidepanel.drag", args, NULL, 0);
    state_of(w, out, sizeof out);
    frame = strstr(out, "\"frame\":[");
    REQUIRE(sscanf(frame, "\"frame\":[%lf,%lf,%lf,%lf]", &x, &y, &fw, &fh) == 4);
    CHECK((int)fw == 420);
    snprintf(args, sizeof args, "{\"contents\":\"%llu\",\"name\":\"side_panel.id_to_width\"}",
             (unsigned long long)c);
    cmd("pref.json", args, out, sizeof out);
    CHECK(has(out, ":420"));
    unregister_window(w);
    bdk_ext_window_close(window);
}

/* An application's container: places the secure view and says so. */
typedef struct {
    int calls, withdrawn, updates;
    bdk_request_t request;
    char payload[1024];
    char last_update[128];
    int window;
    bdk_native_handle_t view;
    const char* side;
    int placed;
} panel_log_t;

static panel_log_t g_plog;

static void on_panel_withdrawn(void* ctx) {
    ++((panel_log_t*)ctx)->withdrawn;
}

static void on_panel_update(void* ctx, bdk_request_t request, bdk_string_t json) {
    (void)request;
    panel_log_t* log = (panel_log_t*)ctx;
    ++log->updates;
    snprintf(log->last_update, sizeof log->last_update, "%.*s", (int)json.length, json.data);
}

static bdk_ui_response_t on_panel_request(void* ctx, const bdk_ui_request_info_t* info) {
    panel_log_t* log = (panel_log_t*)ctx;
    ++log->calls;
    log->request = info->request;
    log->view = info->secure_view;
    snprintf(log->payload, sizeof log->payload, "%.*s", (int)info->payload_json.length,
             info->payload_json.data);
    bdk_ui_set_withdrawn_handler(info->request, on_panel_withdrawn, log, NULL);
    bdk_ui_set_update_handler(info->request, on_panel_update, log, NULL);
    log->placed = bdk_ext_place_view(log->window, info->secure_view, 500, 40, 380, 500);
    char update[96];
    snprintf(update, sizeof update, "{\"placed\":true,\"side\":\"%s\"}", log->side);
    bdk_error_t err = 0;
    CHECK(bdk_ui_update(info->request, S(update), &err) == BDK_OK);
    return BDK_UI_HANDLED;
}

static void register_panel_handler(void) {
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_EXTENSION_SIDE_PANELS, on_panel_request,
                               &g_plog, NULL, &err) == BDK_OK);
}

static void test_ext_side_panel_open_reaches_an_application_container(void) {
    memset(&g_plog, 0, sizeof g_plog);
    g_plog.side = "leading";
    int window;
    bdk_window_t w;
    char ext[64], out[1024];
    bdk_contents_t c = start_panel_page(NULL, register_panel_handler, &window, &w, ext, sizeof ext);
    g_plog.window = window;
    open_panel_from_page(c, ext, "openWindow");
    REQUIRE(wait_for_title(c, "ok:null"));
    for (int i = 0; i < 60 && g_plog.calls == 0; ++i)
        t_pump();
    REQUIRE(g_plog.calls == 1);
    CHECK(has(g_plog.payload, ext));
    CHECK(has(g_plog.payload, "\"scope\":\"window\""));
    CHECK(has(g_plog.payload, "\"title\":\"Panel Side Panel\""));
    CHECK(has(g_plog.payload, "panel.html"));
    CHECK(g_plog.placed == 1);
    CHECK(bdk_ext_view_in_window(g_plog.view));
    /* The application's container, not BDK's frame; the request stays open while the view is up. */
    REQUIRE(panel_is(w, "\"requestOpen\":true"));
    state_of(w, out, sizeof out);
    CHECK(has(out, "\"defaultPresenter\":false"));
    /* The side the application reported is what getLayout answers. */
    open_panel_from_page(c, ext, "layout");
    CHECK(wait_for_title(c, "ok:\"left\""));
    /* Closing from the container completes the request and takes the panel down. */
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_complete(g_plog.request, S("accept"), S("{\"action\":\"close\"}"), no_gesture(),
                            &err) == BDK_OK);
    CHECK(panel_is(w, "\"open\":false"));
    bdk_ext_remove_view(g_plog.view);
    unregister_window(w);
    bdk_ext_window_close(window);
}

static void test_ext_an_extension_closing_its_panel_withdraws_the_request(void) {
    memset(&g_plog, 0, sizeof g_plog);
    g_plog.side = "trailing";
    int window;
    bdk_window_t w;
    char ext[64];
    bdk_contents_t c = start_panel_page(NULL, register_panel_handler, &window, &w, ext, sizeof ext);
    g_plog.window = window;
    open_panel_from_page(c, ext, "openWindow");
    REQUIRE(wait_for_title(c, "ok:null"));
    REQUIRE(panel_is(w, "\"requestOpen\":true"));
    open_panel_from_page(c, ext, "close");
    CHECK(wait_for_title(c, "ok:null"));
    CHECK(panel_is(w, "\"open\":false"));
    pumps(5);
    CHECK(g_plog.withdrawn == 1);
    bdk_ext_remove_view(g_plog.view);
    unregister_window(w);
    bdk_ext_window_close(window);
}

static void
test_ext_a_page_panel_hides_and_shows_with_the_active_page_and_a_second_panel_replaces_it(void) {
    memset(&g_plog, 0, sizeof g_plog);
    g_plog.side = "trailing";
    int window;
    bdk_window_t w;
    char ext[64];
    bdk_contents_t a = start_panel_page(NULL, register_panel_handler, &window, &w, ext, sizeof ext);
    g_plog.window = window;
    bdk_contents_t b = t_new_contents(g_profile);
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_load(b, S(PAGE "-b"), &err) == BDK_OK);
    bdk_ext_window_attach(window, b);
    pumps(5);
    REQUIRE(bdk_contents_focus(a) == BDK_OK);
    /* A page-scoped panel for a: the extension gives its tab a path of its own, then opens it. */
    open_panel_from_page(a, ext, "setTabPanel");
    REQUIRE(wait_for_title(a, "ok:null"));
    open_panel_from_page(a, ext, "openTab");
    REQUIRE(wait_for_title(a, "ok:null"));
    for (int i = 0; i < 60 && g_plog.calls == 0; ++i)
        t_pump();
    REQUIRE(g_plog.calls == 1);
    CHECK(has(g_plog.payload, "\"scope\":\"page\""));
    char want[64];
    snprintf(want, sizeof want, "\"contents\":\"%llu\"", (unsigned long long)a);
    CHECK(has(g_plog.payload, want));
    CHECK(has(g_plog.payload, "tab.html"));
    REQUIRE(panel_is(w, "\"pageScoped\":true"));

    /* b is the focused page: the panel of a is not visible. a again: visible. */
    REQUIRE(bdk_contents_focus(b) == BDK_OK);
    for (int i = 0; i < 40 && !has(g_plog.last_update, "\"visible\":false"); ++i)
        t_pump();
    CHECK(has(g_plog.last_update, "\"visible\":false"));
    CHECK(panel_is(w, "\"visible\":false"));
    REQUIRE(bdk_contents_focus(a) == BDK_OK);
    for (int i = 0; i < 40 && !has(g_plog.last_update, "\"visible\":true"); ++i)
        t_pump();
    CHECK(has(g_plog.last_update, "\"visible\":true"));

    /* The window panel opens in its place: one panel per window, so the first is withdrawn. */
    open_panel_from_page(a, ext, "openWindow");
    CHECK(wait_for_title(a, "ok:null"));
    for (int i = 0; i < 60 && g_plog.calls < 2; ++i)
        t_pump();
    CHECK(g_plog.calls == 2);
    CHECK(g_plog.withdrawn == 1);
    CHECK(has(g_plog.payload, "\"scope\":\"window\""));
    bdk_ext_remove_view(g_plog.view);
    unregister_window(w);
    bdk_ext_window_close(window);
}

static void test_ext_a_disabled_side_panel_surface_creates_nothing(void) {
    memset(&g_plog, 0, sizeof g_plog);
    g_plog.side = "trailing";
    int window;
    bdk_window_t w;
    char ext[64], out[1024];
    bdk_contents_t c = start_panel_page("{\"ui\":{\"side_panels\":\"disabled\"}}",
                                        register_panel_handler, &window, &w, ext, sizeof ext);
    g_plog.window = window;
    open_panel_from_page(c, ext, "openWindow");
    wait_for_title(c, "ok:null");
    pumps(20);
    CHECK(g_plog.calls == 0);
    state_of(w, out, sizeof out);
    CHECK(has(out, "\"open\":false"));
    cmd("present.record", "{\"surface\":\"extension_side_panels\"}", out, sizeof out);
    CHECK(has(out, "\"shown\":0"));
    unregister_window(w);
    bdk_ext_window_close(window);
}

/* ---- DevTools extension pages (T-EXT-08) ---- */

typedef struct {
    int calls;
    bdk_request_t request;
    char payload[2048];
    const char* complete;
    const char* complete_args;
} ui_log_t;

static ui_log_t g_menu, g_windows;

static bdk_ui_response_t record(void* ctx, const bdk_ui_request_info_t* info) {
    ui_log_t* log = (ui_log_t*)ctx;
    ++log->calls;
    log->request = info->request;
    snprintf(log->payload, sizeof log->payload, "%.*s", (int)info->payload_json.length,
             info->payload_json.data);
    if (log->complete) {
        bdk_error_t err = 0;
        CHECK(bdk_ui_complete(info->request, S(log->complete),
                              S(log->complete_args ? log->complete_args : ""), no_gesture(),
                              &err) == BDK_OK);
    }
    return BDK_UI_HANDLED;
}

static void register_devtools_handlers(void) {
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_NEW_WINDOWS, record, &g_windows, NULL,
                               &err) == BDK_OK);
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_CONTEXT_MENUS, record, &g_menu, NULL, &err) ==
            BDK_OK);
}

/* Opens undocked DevTools for c through the context menu's Inspect item; returns the front end. */
static bdk_contents_t open_devtools(bdk_contents_t c) {
    memset(&g_menu, 0, sizeof g_menu);
    memset(&g_windows, 0, sizeof g_windows);
    g_windows.complete = "accept";
    bdk_harness_engine_show(c, 1);
    REQUIRE(bdk_testing_click(c, 20, 20, 2) == BDK_OK);
    for (int i = 0; i < 40 && g_menu.calls == 0; ++i)
        t_pump();
    REQUIRE(g_menu.calls == 1);
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_complete(g_menu.request, S("accept"),
                            S("{\"action\":\"perform\",\"item\":\"inspect\"}"), no_gesture(),
                            &err) == BDK_OK);
    pumps(3);
    REQUIRE(g_windows.calls == 1);
    char handle[32];
    REQUIRE(t_json_string(g_windows.payload, "newContents", handle, sizeof handle));
    bdk_contents_t front = (bdk_contents_t)strtoull(handle, NULL, 10);
    bdk_harness_engine_show(front, 1);
    return front;
}

static int devtools_reported(bdk_contents_t c, const char* ext, char* out, size_t n) {
    for (int i = 0; i < 150; ++i) {
        eval_in(c, ext, "JSON.stringify(globalThis.ready)", out, n);
        if (has(out, "created"))
            return 1;
        t_pump();
    }
    return 0;
}

static void test_ext_a_devtools_page_adds_its_panel_to_undocked_devtools(void) {
    bdk_contents_t c = start(NULL, register_devtools_handlers);
    char ext[64], out[1024];
    load_extension(c, "panel-devtools", ext, sizeof ext);
    bdk_contents_t front = open_devtools(c);
    /* The extension's devtools page ran in the front end and created its panel, for this tab. */
    CHECK(devtools_reported(c, ext, out, sizeof out));
    bdk_testing_tab_info_t ti;
    memset(&ti, 0, sizeof ti);
    ti.struct_size = sizeof ti;
    REQUIRE(bdk_testing_tab_info(c, &ti) == BDK_OK);
    CHECK(number_after(out, "\"tabId\":") == ti.tab_id);
    bdk_harness_engine_show(front, 0);
    CHECK(bdk_contents_destroy(front) == BDK_OK);
    bdk_harness_engine_show(c, 0);
}

static void test_ext_a_disabled_devtools_surface_loads_no_extension_page(void) {
    bdk_contents_t c = start("{\"ui\":{\"devtools\":\"disabled\"}}", register_devtools_handlers);
    char ext[64], out[1024];
    load_extension(c, "panel-devtools", ext, sizeof ext);
    bdk_contents_t front = open_devtools(c);
    pumps(60);
    eval_in(c, ext, "JSON.stringify(globalThis.ready)", out, sizeof out);
    CHECK(!has(out, "created"));
    bdk_harness_engine_show(front, 0);
    CHECK(bdk_contents_destroy(front) == BDK_OK);
    bdk_harness_engine_show(c, 0);
}

const t_test_t t_engine_extension_panel_tests[] = {
    {"engine_ext_a_registered_popup_window_reports_type_popup",
     test_ext_a_registered_popup_window_reports_type_popup},
    {"engine_ext_a_registered_normal_window_reports_type_normal",
     test_ext_a_registered_normal_window_reports_type_normal},
    {"engine_ext_window_attachments_match_what_extensions_see",
     test_ext_window_attachments_match_what_extensions_see},
    {"engine_ext_the_parking_context_is_hidden_from_extensions",
     test_ext_the_parking_context_is_hidden_from_extensions},
    {"engine_ext_windows_update_reaches_the_handler_and_use_default_applies_it",
     test_ext_windows_update_reaches_the_handler_and_use_default_applies_it},
    {"engine_ext_a_rejected_window_request_is_not_applied",
     test_ext_a_rejected_window_request_is_not_applied},
    {"engine_ext_windows_remove_raises_close_but_moving_a_page_out_does_not",
     test_ext_windows_remove_raises_close_but_moving_a_page_out_does_not},
    {"engine_ext_a_default_permission_prompt_attaches_to_the_supplied_anchor",
     test_ext_a_default_permission_prompt_attaches_to_the_supplied_anchor},
    {"engine_ext_side_panel_open_shows_the_default_panel_at_the_trailing_edge",
     test_ext_side_panel_open_shows_the_default_panel_at_the_trailing_edge},
    {"engine_ext_side_panel_open_reaches_an_application_container",
     test_ext_side_panel_open_reaches_an_application_container},
    {"engine_ext_an_extension_closing_its_panel_withdraws_the_request",
     test_ext_an_extension_closing_its_panel_withdraws_the_request},
    {"engine_ext_a_page_panel_hides_and_shows_with_the_active_page_and_a_second_panel_replaces_it",
     test_ext_a_page_panel_hides_and_shows_with_the_active_page_and_a_second_panel_replaces_it},
    {"engine_ext_a_disabled_side_panel_surface_creates_nothing",
     test_ext_a_disabled_side_panel_surface_creates_nothing},
    {"engine_ext_a_devtools_page_adds_its_panel_to_undocked_devtools",
     test_ext_a_devtools_page_adds_its_panel_to_undocked_devtools},
    {"engine_ext_a_disabled_devtools_surface_loads_no_extension_page",
     test_ext_a_disabled_devtools_surface_loads_no_extension_page},
    {NULL, NULL},
};
