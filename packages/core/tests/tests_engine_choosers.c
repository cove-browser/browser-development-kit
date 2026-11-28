/* Engine scenarios for the device choosers (ui-requests.md 5.16, T-UIR-13): WebUSB's, WebHID's and
   Web Serial's choosers are device_choosers requests, with BDK's own sheet as the default
   presenter. The web APIs' requests find whatever devices the Mac has, so the scenarios that
   choose a device use the engine's fake chooser (chooser.open, bdk_testing.h), which goes through
   the same session. Default presenters are scripted, so no sheet blocks the suite. Built only into
   the engine conformance app (BDK_HARNESS_ENGINE_TESTS). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bdk_testing.h"
#include "engine_ext_support.h"
#include "test_support.h"

void bdk_harness_engine_show(bdk_contents_t contents, int visible);
bdk_native_handle_t bdk_harness_engine_gesture(bdk_contents_t contents, int kind);
int bdk_ext_window_open(bdk_runtime_t rt, uint32_t kind, bdk_window_t* handle);
void bdk_ext_window_attach(int window, bdk_contents_t contents);
void bdk_ext_window_close(int window);
extern const char* bdk_harness_engine_surfaces;

/* A secure context the engine's interceptor serves. */
#define PAGE "http://localhost/choosers"

typedef struct {
    int calls;
    bdk_request_t request;
    char payloads[4][65536];
    int updates;
    char update[65536];
    /* 1: cancel each request at once; 0: leave it open for the scenario. */
    int deny;
} choosers_t;

static choosers_t g_choosers;
static bdk_profile_t g_profile;

static void on_update(void* ctx, bdk_request_t request, bdk_string_t update) {
    (void)ctx;
    (void)request;
    ++g_choosers.updates;
    snprintf(g_choosers.update, sizeof g_choosers.update, "%.*s", (int)update.length,
             update.data ? update.data : "");
}

static bdk_ui_response_t on_request(void* ctx, const bdk_ui_request_info_t* info) {
    choosers_t* log = (choosers_t*)ctx;
    if (log->calls < 4)
        snprintf(log->payloads[log->calls], sizeof log->payloads[0], "%.*s",
                 (int)info->payload_json.length,
                 info->payload_json.data ? info->payload_json.data : "");
    ++log->calls;
    log->request = info->request;
    bdk_ui_set_update_handler(info->request, on_update, NULL, NULL);
    if (log->deny) {
        bdk_native_handle_t none;
        memset(&none, 0, sizeof none);
        bdk_error_t err = 0;
        CHECK(bdk_ui_complete(info->request, S("deny"), S(""), none, &err) == BDK_OK);
    }
    return BDK_UI_HANDLED;
}

static void control(const char* verb, const char* args, char* out, size_t n) {
    REQUIRE(bdk_testing_command(t_runtime, S(verb), S(args), out, n) == BDK_OK);
}

static void script(const char* answer) {
    char args[256];
    snprintf(args, sizeof args, "{\"surface\":\"device_choosers\",\"answer\":%s}", answer);
    control("present.script", args, NULL, 0);
}

static int shown(char* out, size_t n) {
    control("present.record", "{\"surface\":\"device_choosers\"}", out, n);
    const char* at = strstr(out, "\"shown\":");
    return at ? atoi(at + 8) : 0;
}

static int wait_for_title(bdk_contents_t c, const char* want) {
    char title[256] = "";
    for (int i = 0; i < 400; ++i) {
        bdk_contents_state_t st;
        memset(&st, 0, sizeof st);
        st.struct_size = sizeof st;
        bdk_error_t err = 0;
        REQUIRE(bdk_contents_state(c, &st, &err) == BDK_OK);
        snprintf(title, sizeof title, "%.*s", (int)st.title.length,
                 st.title.data ? st.title.data : "");
        if (strcmp(title, want) == 0)
            return 1;
        t_pump();
    }
    printf("    title is \"%s\", not \"%s\"\n", title, want);
    return 0;
}

/* A runtime (with a handler of `surface` unless `handler` is 0, and the surfaces of
   `surfaces_json`) and a page, shown and focused. */
static bdk_contents_t start_surface(uint32_t surface, int handler, const char* surfaces_json) {
    memset(&g_choosers, 0, sizeof g_choosers);
    bdk_harness_engine_surfaces = surfaces_json;
    t_start();
    control("present.clear", "{}", NULL, 0);
    bdk_error_t err = 0;
    if (handler)
        REQUIRE(bdk_ui_set_handler(t_runtime, surface, on_request, &g_choosers, NULL, &err) ==
                BDK_OK);
    t_freeze();
    g_profile = t_new_profile("Choosers");
    bdk_contents_t c = t_new_contents(g_profile);
    REQUIRE(bdk_contents_load(c, S(PAGE), &err) == BDK_OK);
    t_pump();
    bdk_harness_engine_show(c, 1);
    REQUIRE(bdk_contents_focus(c) == BDK_OK);
    t_pump();
    return c;
}

static bdk_contents_t start_page(int handler, const char* surfaces_json) {
    return start_surface(BDK_SURFACE_DEVICE_CHOOSERS, handler, surfaces_json);
}

static bdk_contents_t start_screen_capture(int handler, const char* surfaces_json) {
    return start_surface(BDK_SURFACE_SCREEN_CAPTURE, handler, surfaces_json);
}

static void finish(bdk_contents_t c) {
    control("present.clear", "{}", NULL, 0);
    bdk_harness_engine_show(c, 0);
    for (int i = 0; i < 10; ++i)
        t_pump();
    CHECK(bdk_testing_chrome_window_count() == 0);
}

static void run(bdk_contents_t c, const char* js) {
    REQUIRE(bdk_testing_execute_script(c, S(js), 1) == BDK_OK);
    t_pump();
}

/* Each API in turn, each with a gesture of its own (a chooser takes the page's activation): the
   page's title becomes what each saw, an error's name or the number of HID devices, joined, and
   whether navigator.bluetooth exists. */
static const char* const ask[] = {
    "navigator.usb.requestDevice({filters: []}).then(() => 'usb', e => e.name)"
    ".then(r => document.title = r)",
    "navigator.serial.requestPort().then(() => 'serial', e => e.name)"
    ".then(r => document.title += ',' + r)",
    "navigator.hid.requestDevice({filters: []}).then(d => String(d.length), e => e.name)"
    ".then(r => document.title += ',' + r + ',' + typeof navigator.bluetooth)",
};

static int ask_each(bdk_contents_t c) {
    static const char* const want[] = {"NotFoundError", "NotFoundError,NotFoundError",
                                       "NotFoundError,NotFoundError,0,undefined"};
    for (int i = 0; i < 3; ++i) {
        run(c, ask[i]);
        if (!wait_for_title(c, want[i]))
            return 0;
    }
    return 1;
}

static void open_fake(bdk_contents_t c, const char* devices_json) {
    char args[256];
    snprintf(args, sizeof args, "{\"contents\":\"%llu\",\"kind\":\"usb\",\"devices\":%s}",
             (unsigned long long)c, devices_json);
    control("chooser.open", args, NULL, 0);
    t_pump();
}

/* Waits for the fake chooser to end; returns its result. */
static void fake_result(char* out, size_t n) {
    for (int i = 0; i < 100; ++i) {
        control("chooser.result", "{}", out, n);
        if (strstr(out, "\"ended\":true"))
            return;
        t_pump();
    }
}

/* ---- Scenarios ---- */

/* Each API raises a request of its kind, from the page's origin; a handler's cancel rejects it
   (HID: no device). With no Bluetooth usage string in the harness, Web Bluetooth is absent. */
static void test_each_api_raises_a_device_chooser_request(void) {
    bdk_contents_t c = start_page(1, NULL);
    g_choosers.deny = 1;
    CHECK(ask_each(c));
    CHECK(g_choosers.calls == 3);
    CHECK(strstr(g_choosers.payloads[0], "\"kind\":\"usb\"") != NULL);
    CHECK(strstr(g_choosers.payloads[1], "\"kind\":\"serial\"") != NULL);
    CHECK(strstr(g_choosers.payloads[2], "\"kind\":\"hid\"") != NULL);
    CHECK(strstr(g_choosers.payloads[0], "\"origin\":\"http://localhost\"") != NULL);
    CHECK(strstr(g_choosers.payloads[0], "\"devices\":[") != NULL);
    finish(c);
}

/* With no handler, BDK's sheet asks; cancelled, the page's request rejects. */
static void test_with_no_handler_the_default_chooser_asks(void) {
    bdk_contents_t c = start_page(0, NULL);
    script("{\"answer\":\"cancel\"}");
    CHECK(ask_each(c));
    char out[4096];
    CHECK(shown(out, sizeof out) == 3);
    CHECK(strstr(out, "kind\\\":\\\"hid") != NULL);
    finish(c);
}

/* Disabled: no request and no sheet; each request rejects as a cancel would. */
static void test_disabled_choosers_reject_without_asking(void) {
    bdk_contents_t c = start_page(1, "{\"device_choosers\":\"disabled\"}");
    script("{\"answer\":\"cancel\"}");
    CHECK(ask_each(c));
    CHECK(g_choosers.calls == 0);
    char out[4096];
    CHECK(shown(out, sizeof out) == 0);
    finish(c);
}

/* The request's devices follow the chooser's list, each keeping its id; "select" with the
   person's gesture connects to the device it names, and without one is refused. */
static void test_a_handler_selects_a_device_as_the_list_changes(void) {
    bdk_contents_t c = start_page(1, NULL);
    open_fake(c, "[\"Alpha\"]");
    CHECK(g_choosers.calls == 1);
    CHECK(strstr(g_choosers.payloads[0], "{\"id\":\"device0\",\"name\":\"Alpha\"}") != NULL);
    control("chooser.add", "{\"name\":\"Beta\"}", NULL, 0);
    control("chooser.remove", "{\"name\":\"Alpha\"}", NULL, 0);
    for (int i = 0; i < 10 && g_choosers.updates < 2; ++i)
        t_pump();
    CHECK(g_choosers.updates == 2);
    CHECK(strstr(g_choosers.update, "\"name\":\"Beta\"") != NULL);
    CHECK(strstr(g_choosers.update, "device1") != NULL);
    CHECK(strstr(g_choosers.update, "Alpha") == NULL);
    bdk_error_t err = 0;
    CHECK(bdk_ui_complete(g_choosers.request, S("select"), S("{\"device\":\"device1\"}"),
                          bdk_harness_engine_gesture(c, 0), &err) == BDK_OK);
    char out[256];
    fake_result(out, sizeof out);
    CHECK(strstr(out, "\"chosen\":\"Beta\"") != NULL);

    open_fake(c, "[\"Gamma\"]");
    bdk_native_handle_t none;
    memset(&none, 0, sizeof none);
    err = 0;
    CHECK(bdk_ui_complete(g_choosers.request, S("select"), S("{\"device\":\"device0\"}"), none,
                          &err) != BDK_OK);
    bdk_ui_request_release(g_choosers.request);
    fake_result(out, sizeof out);
    CHECK(strstr(out, "\"ended\":true") != NULL);
    CHECK(strstr(out, "chosen") == NULL);
    finish(c);
}

/* The default chooser offers a device that appears after it opened. */
static void test_the_default_chooser_offers_a_device_that_appears_later(void) {
    bdk_contents_t c = start_page(0, NULL);
    script("{\"device\":\"Gamma\"}");
    open_fake(c, "[]");
    char out[4096];
    control("chooser.result", "{}", out, sizeof out);
    CHECK(strstr(out, "\"ended\":false") != NULL);
    control("chooser.add", "{\"name\":\"Gamma\"}", NULL, 0);
    fake_result(out, sizeof out);
    CHECK(strstr(out, "\"chosen\":\"Gamma\"") != NULL);
    CHECK(shown(out, sizeof out) == 1);
    finish(c);
}

/* ---- Screen capture ---- */

#define SHARED_PAGE "http://localhost/shared"

static const char* share_script =
    "navigator.mediaDevices.getDisplayMedia({video: true}).then(s => {"
    "  window.stream = s; s.getVideoTracks()[0].onended = () => document.title = 'ended';"
    "  document.title = 'shared'; }, e => document.title = e.name)";

static uint32_t capture_of(bdk_contents_t c) {
    bdk_contents_state_t st;
    memset(&st, 0, sizeof st);
    st.struct_size = sizeof st;
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_state(c, &st, &err) == BDK_OK);
    return st.capture;
}

static int wait_for_capture(bdk_contents_t c, uint32_t flag, int set) {
    for (int i = 0; i < 200; ++i) {
        if (((capture_of(c) & flag) != 0) == (set != 0))
            return 1;
        t_pump();
    }
    return 0;
}

/* A second page, titled "Shared", that the first may share. */
static bdk_contents_t shared_page(void) {
    REQUIRE(bdk_harness_script_navigation(t_runtime, SHARED_PAGE, 0, "Shared") == BDK_OK);
    bdk_contents_t c = t_new_contents(g_profile);
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_load(c, S(SHARED_PAGE), &err) == BDK_OK);
    t_pump();
    bdk_harness_engine_show(c, 1);
    t_pump();
    return c;
}

/* The id of the source named `name` in `json` (a payload or an update), or "" if it is not
   offered. */
static void source_named(const char* json, const char* name, char* id, size_t n) {
    id[0] = 0;
    char pattern[128];
    snprintf(pattern, sizeof pattern, "\"name\":\"%s\"", name);
    const char* at = strstr(json, pattern);
    if (!at)
        return;
    /* The id comes first in each source: walk back to its object. */
    const char* start = at;
    while (start > json && *start != '{')
        --start;
    const char* key = strstr(start, "\"id\":\"");
    if (!key || key > at)
        return;
    key += 6;
    size_t len = strcspn(key, "\"");
    snprintf(id, n, "%.*s", (int)(len < n ? len : n - 1), key);
}

/* A handler sees the page's sources and shares one with the person's gesture: the page gets the
   stream, the capturing page reports BDK_CAPTURE_DISPLAY and the shared one BDK_CAPTURE_CAPTURED,
   until the page stops. */
static void test_a_handler_shares_a_page(void) {
    bdk_contents_t c = start_screen_capture(1, NULL);
    bdk_contents_t other = shared_page();
    run(c, share_script);
    char id[32] = "";
    for (int i = 0; i < 200 && !id[0]; ++i) {
        source_named(g_choosers.updates ? g_choosers.update : g_choosers.payloads[0], "Shared", id,
                     sizeof id);
        if (!id[0])
            t_pump();
    }
    CHECK(g_choosers.calls == 1);
    CHECK(strstr(g_choosers.payloads[0], "\"origin\":\"http://localhost\"") != NULL);
    CHECK(strstr(g_choosers.payloads[0], "\"requestsAudio\":false") != NULL);
    REQUIRE(id[0] != 0);
    char args[96];
    snprintf(args, sizeof args, "{\"source\":\"%s\"}", id);
    bdk_error_t err = 0;
    CHECK(bdk_ui_complete(g_choosers.request, S("share"), S(args), bdk_harness_engine_gesture(c, 0),
                          &err) == BDK_OK);
    CHECK(wait_for_capture(c, BDK_CAPTURE_DISPLAY, 1));
    CHECK(wait_for_capture(other, BDK_CAPTURE_CAPTURED, 1));
    /* The stream reaches the page once the shared page has drawn. */
    CHECK(wait_for_title(c, "shared"));
    run(c, "window.stream.getTracks().forEach(t => t.stop())");
    CHECK(wait_for_capture(c, BDK_CAPTURE_DISPLAY, 0));
    CHECK(wait_for_capture(other, BDK_CAPTURE_CAPTURED, 0));
    bdk_harness_engine_show(other, 0);
    finish(c);
}

/* With no handler BDK's picker asks, and BDK's sharing bar shows while the page shares; its Stop
   ends the stream. */
static void test_the_default_picker_shares_and_the_bar_stops_it(void) {
    bdk_contents_t c = start_screen_capture(0, NULL);
    bdk_contents_t other = shared_page();
    control("present.script", "{\"surface\":\"screen_capture\",\"answer\":{\"source\":\"Shared\"}}",
            NULL, 0);
    control("present.script", "{\"surface\":\"sharing_indicator\",\"answer\":{\"none\":true}}",
            NULL, 0);
    run(c, share_script);
    CHECK(wait_for_title(c, "shared"));
    char out[4096];
    control("present.record", "{\"surface\":\"sharing_indicator\"}", out, sizeof out);
    CHECK(strstr(out, "\"shown\":1") != NULL);
    CHECK(strstr(out, "Shared") != NULL);
    control("present.script",
            "{\"surface\":\"sharing_indicator\",\"answer\":{\"answer\":\"stop\"}}", NULL, 0);
    /* A second share, whose bar stops it as the person would. */
    run(c, "document.title = 'again'");
    run(c, share_script);
    CHECK(wait_for_title(c, "ended"));
    control("present.record", "{\"surface\":\"sharing_indicator\"}", out, sizeof out);
    printf("    indicator %s\n", out);
    control("present.record", "{\"surface\":\"screen_capture\"}", out, sizeof out);
    printf("    picker %.300s\n", out);
    bdk_harness_engine_show(other, 0);
    finish(c);
}

/* A handler's cancel and a disabled surface each refuse: NotAllowedError. */
static void test_a_cancelled_or_disabled_capture_is_refused(void) {
    bdk_contents_t c = start_screen_capture(1, NULL);
    g_choosers.deny = 1;
    run(c, share_script);
    CHECK(wait_for_title(c, "NotAllowedError"));
    CHECK(g_choosers.calls == 1);
    finish(c);
}

static void test_a_disabled_capture_is_refused_without_asking(void) {
    bdk_contents_t c = start_screen_capture(1, "{\"screen_capture\":\"disabled\"}");
    run(c, share_script);
    CHECK(wait_for_title(c, "NotAllowedError"));
    CHECK(g_choosers.calls == 0);
    finish(c);
}

/* Asks the extension to choose a source for its tab; the answer lands in globalThis.out. */
static void extension_chooses(bdk_contents_t c, const char* ext) {
    char out[256];
    ext_eval(c, ext,
             "globalThis.out = undefined; chrome.tabs.query({}, tabs =>"
             " chrome.desktopCapture.chooseDesktopMedia(['tab'], tabs[0],"
             " id => globalThis.out = id ? 'chosen' : 'cancelled')); 1",
             out, sizeof out);
}

/* What the extension's choice gave: "chosen", "cancelled", or "" while it waits. */
static int extension_answer(bdk_contents_t c, const char* ext, const char* want) {
    char out[256];
    for (int i = 0; i < 100; ++i) {
        ext_eval(c, ext, "String(globalThis.out)", out, sizeof out);
        if (ext_has(out, want))
            return 1;
        t_pump();
    }
    printf("    the extension has %s, not %s\n", out, want);
    return 0;
}

/* chrome.desktopCapture.chooseDesktopMedia with the extension's tab asks on that page, naming the
   extension; a shared source gives the extension a stream id, a cancel none. */
static void test_an_extension_asks_through_the_page(void) {
    bdk_contents_t c = start_screen_capture(1, NULL);
    bdk_contents_t other = shared_page();
    char ext[64];
    ext_load(c, "capture-desktop", ext, sizeof ext);
    /* A page is a tab an extension sees once it is in a registered window. */
    bdk_window_t w = 0;
    int window = bdk_ext_window_open(t_runtime, BDK_WINDOW_KIND_NORMAL, &w);
    REQUIRE(window >= 0);
    bdk_ext_window_attach(window, c);
    ext_pumps(5);
    g_choosers.deny = 1;
    extension_chooses(c, ext);
    CHECK(extension_answer(c, ext, "cancelled"));
    CHECK(g_choosers.calls == 1);
    CHECK(strstr(g_choosers.payloads[0], "\"extensionName\":\"Capture Desktop\"") != NULL);
    g_choosers.deny = 0;
    g_choosers.updates = 0;
    extension_chooses(c, ext);
    char id[32] = "";
    for (int i = 0; i < 200 && !id[0]; ++i) {
        source_named(g_choosers.updates ? g_choosers.update : g_choosers.payloads[1], "Shared", id,
                     sizeof id);
        if (!id[0])
            t_pump();
    }
    REQUIRE(id[0] != 0);
    char args[96];
    snprintf(args, sizeof args, "{\"source\":\"%s\"}", id);
    bdk_error_t err = 0;
    CHECK(bdk_ui_complete(g_choosers.request, S("share"), S(args), bdk_harness_engine_gesture(c, 0),
                          &err) == BDK_OK);
    CHECK(extension_answer(c, ext, "chosen"));
    bdk_harness_engine_show(other, 0);
    bdk_ext_window_close(window);
    finish(c);
}

const t_test_t t_engine_chooser_tests[] = {
    {"engine_choosers_each_api_raises_a_device_chooser_request",
     test_each_api_raises_a_device_chooser_request},
    {"engine_choosers_with_no_handler_the_default_chooser_asks",
     test_with_no_handler_the_default_chooser_asks},
    {"engine_choosers_disabled_reject_without_asking",
     test_disabled_choosers_reject_without_asking},
    {"engine_choosers_a_handler_selects_a_device_as_the_list_changes",
     test_a_handler_selects_a_device_as_the_list_changes},
    {"engine_choosers_the_default_chooser_offers_a_device_that_appears_later",
     test_the_default_chooser_offers_a_device_that_appears_later},
    {"engine_screen_capture_a_handler_shares_a_page", test_a_handler_shares_a_page},
    {"engine_screen_capture_the_default_picker_shares_and_the_bar_stops_it",
     test_the_default_picker_shares_and_the_bar_stops_it},
    {"engine_screen_capture_a_cancelled_capture_is_refused",
     test_a_cancelled_or_disabled_capture_is_refused},
    {"engine_screen_capture_a_disabled_capture_is_refused_without_asking",
     test_a_disabled_capture_is_refused_without_asking},
    {"engine_screen_capture_an_extension_asks_through_the_page",
     test_an_extension_asks_through_the_page},
    {NULL, NULL},
};
