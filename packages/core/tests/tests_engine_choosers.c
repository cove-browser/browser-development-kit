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
#include "test_support.h"

void bdk_harness_engine_show(bdk_contents_t contents, int visible);
bdk_native_handle_t bdk_harness_engine_gesture(bdk_contents_t contents, int kind);
extern const char* bdk_harness_engine_surfaces;

/* A secure context the engine's interceptor serves. */
#define PAGE "http://localhost/choosers"

typedef struct {
    int calls;
    bdk_request_t request;
    char payloads[4][2048];
    int updates;
    char update[2048];
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

/* A runtime (with a device_choosers handler unless `handler` is 0, and the surfaces of
   `surfaces_json`) and a page, shown and focused. */
static bdk_contents_t start_page(int handler, const char* surfaces_json) {
    memset(&g_choosers, 0, sizeof g_choosers);
    bdk_harness_engine_surfaces = surfaces_json;
    t_start();
    control("present.clear", "{}", NULL, 0);
    bdk_error_t err = 0;
    if (handler)
        REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_DEVICE_CHOOSERS, on_request, &g_choosers,
                                   NULL, &err) == BDK_OK);
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
    {NULL, NULL},
};
