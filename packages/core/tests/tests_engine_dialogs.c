/* Engine scenarios for Chromium's other browser dialogs on BDK pages (ui-requests.md 8.2,
   T-UIR-10). Each of these was a tab-modal views dialog or bubble that ended the process on a BDK
   page; each now has a sheet of BDK's own, is refused, or is turned off. The scenarios only pass
   when the process survives and the page sees what the record says it sees. Default presenters are
   scripted, so no sheet blocks the suite. Built only into the engine conformance app
   (BDK_HARNESS_ENGINE_TESTS). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bdk_testing.h"
#include "test_support.h"

void bdk_harness_engine_show(bdk_contents_t contents, int visible);

/* http://localhost is a secure context, which every API below needs, and the engine's
   interceptor serves it. */
#define PAGE "http://localhost/dialogs"

static void control(const char* verb, const char* args, char* out, size_t n) {
    REQUIRE(bdk_testing_command(t_runtime, S(verb), S(args), out, n) == BDK_OK);
}

static void script(const char* surface, const char* answer) {
    char args[256];
    snprintf(args, sizeof args, "{\"surface\":\"%s\",\"answer\":{\"answer\":\"%s\"}}", surface,
             answer);
    control("present.script", args, NULL, 0);
}

static int json_int(const char* json, const char* key) {
    char pattern[64];
    snprintf(pattern, sizeof pattern, "\"%s\":", key);
    const char* at = strstr(json, pattern);
    return at ? atoi(at + strlen(pattern)) : -1;
}

/* What the surface's default presenter was asked to show (its record, as JSON). */
static void record(const char* surface, char* out, size_t n) {
    char args[96];
    snprintf(args, sizeof args, "{\"surface\":\"%s\"}", surface);
    control("present.record", args, out, n);
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
    for (int i = 0; i < 400; ++i) {
        title_of(c, title, sizeof title);
        if (strcmp(title, want) == 0)
            return 1;
        t_pump();
    }
    printf("    title is \"%s\", not \"%s\"\n", title, want);
    return 0;
}

static bdk_profile_t g_profile;

/* A runtime and a page at `url`, shown and focused. */
static bdk_contents_t start_page(const char* url) {
    t_start();
    control("present.clear", "{}", NULL, 0);
    t_freeze();
    g_profile = t_new_profile("Dialogs");
    bdk_contents_t c = t_new_contents(g_profile);
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_load(c, S(url), &err) == BDK_OK);
    t_pump();
    bdk_harness_engine_show(c, 1);
    REQUIRE(bdk_contents_focus(c) == BDK_OK);
    t_pump();
    return c;
}

/* Runs script in the page with a user gesture. */
static void run(bdk_contents_t c, const char* js) {
    REQUIRE(bdk_testing_execute_script(c, S(js), 1) == BDK_OK);
    t_pump();
}

static void finish(bdk_contents_t c) {
    control("present.clear", "{}", NULL, 0);
    bdk_harness_engine_show(c, 0);
    for (int i = 0; i < 10; ++i)
        t_pump();
    CHECK(bdk_testing_chrome_window_count() == 0);
}

static void contents_arg(bdk_contents_t c, char* out, size_t n) {
    snprintf(out, n, "%llu", (unsigned long long)c);
}

/* ---- Scenarios ---- */

/* HTTPS-First's "ask before HTTP" dialog failed `Check failed: host` in tab_dialog_manager.cc. With
   the dialog UI off, strict HTTPS-First asks with its interstitial page, in the page. The host is
   not resolvable, so the HTTPS upgrade fails and Chrome falls back to asking. */
static void test_https_first_asks_with_its_interstitial_page(void) {
    bdk_contents_t c = start_page(PAGE);
    char args[256], handle[32];
    contents_arg(c, handle, sizeof handle);
    snprintf(args, sizeof args,
             "{\"contents\":\"%s\",\"name\":\"https_only_mode_enabled\",\"value\":true}", handle);
    control("pref.set", args, NULL, 0);
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_load(c, S("http://hfm.bdk.test/"), &err) == BDK_OK);
    CHECK(wait_for_title(c, "Site is not secure"));
    finish(c);
}

/* An external scheme Chrome asks about (mailto: it opens without asking) gets the sheet, with
   Cancel first so that Return never opens another application. Cancelled: nothing opens. */
static void test_an_external_protocol_asks_with_a_sheet(void) {
    bdk_contents_t c = start_page(PAGE);
    script("external_protocols", "cancel");
    run(c, "location.href = 'maps:?q=BDK'");
    char out[4096];
    int shown = 0;
    for (int i = 0; i < 200 && shown < 1; ++i) {
        t_pump();
        record("external_protocols", out, sizeof out);
        shown = json_int(out, "shown");
    }
    CHECK(shown == 1);
    CHECK(strstr(out, "[\\\"cancel\\\",\\\"open\\\"]") != NULL);
    finish(c);
}

/* chrome::ShowTabModal (File System Access's and DevTools' questions) shows the model as a sheet,
   and the model's own callbacks run: accept, cancel, and a sheet dismissed with no answer. */
static void tab_modal(bdk_contents_t c, const char* answer, int default_cancel, const char* want) {
    script("browser_dialogs", answer);
    char args[256], handle[32], out[256];
    contents_arg(c, handle, sizeof handle);
    snprintf(args, sizeof args,
             "{\"contents\":\"%s\",\"title\":\"Edit files?\",\"text\":\"A question.\","
             "\"defaultCancel\":%s}",
             handle, default_cancel ? "true" : "false");
    control("dialogs.tab_modal", args, NULL, 0);
    for (int i = 0; i < 100; ++i) {
        t_pump();
        control("dialogs.tab_modal.result", "{}", out, sizeof out);
        if (strstr(out, "\"destroyed\":true"))
            break;
    }
    char expected[64];
    snprintf(expected, sizeof expected, "\"result\":\"%s\"", want);
    if (!strstr(out, expected))
        printf("    %s: %s\n", answer, out);
    CHECK(strstr(out, expected) != NULL);
    CHECK(strstr(out, "\"destroyed\":true") != NULL);
}

static void test_a_tab_modal_dialog_model_is_a_sheet(void) {
    bdk_contents_t c = start_page(PAGE);
    tab_modal(c, "accept", 0, "accepted");
    char out[4096];
    record("browser_dialogs", out, sizeof out);
    CHECK(strstr(out, "Edit files?") != NULL);
    CHECK(strstr(out, "[\\\"accept\\\",\\\"cancel\\\"]") != NULL);
    tab_modal(c, "cancel", 1, "cancelled");
    record("browser_dialogs", out, sizeof out);
    CHECK(strstr(out, "[\\\"cancel\\\",\\\"accept\\\"]") != NULL); /* the model's default first */
    tab_modal(c, "noSuchButton", 0, "closed");
    finish(c);
}

/* The device choosers were views bubbles anchored to a BrowserView; they are refused as a person
   cancelling would, so each request rejects or comes back empty, and Web Bluetooth is absent. */
static void test_device_choosers_are_refused(void) {
    bdk_contents_t c = start_page(PAGE);
    run(c, "(async () => { const r = [];"
           "try { await navigator.usb.requestDevice({filters: []}); r.push('usb'); }"
           "  catch (e) { r.push(e.name); }"
           "try { await navigator.serial.requestPort(); r.push('serial'); }"
           "  catch (e) { r.push(e.name); }"
           "try { r.push(String((await navigator.hid.requestDevice({filters: []})).length)); }"
           "  catch (e) { r.push(e.name); }"
           "r.push(typeof navigator.bluetooth);"
           "document.title = r.join(','); })()");
    CHECK(wait_for_title(c, "NotFoundError,NotFoundError,0,undefined"));
    finish(c);
}

/* getDisplayMedia's source picker was a tab-modal views dialog; it is refused as the
   screen_capture surface's disabled mode would refuse it. */
static void test_screen_capture_is_refused(void) {
    bdk_contents_t c = start_page(PAGE);
    run(c, "navigator.mediaDevices.getDisplayMedia({video: true})"
           ".then(() => { document.title = 'shared'; },"
           "      e => { document.title = e.name; })");
    CHECK(wait_for_title(c, "NotAllowedError"));
    finish(c);
}

/* The APIs whose UI is views only are off at start, so a site finds them unsupported, and a page
   never offers itself as an application to install. */
static void test_apis_with_views_only_ui_are_absent(void) {
    bdk_contents_t c = start_page(PAGE);
    run(c, "document.title = [typeof PaymentRequest, typeof IdentityCredential,"
           " typeof DigitalCredential, typeof navigator.bluetooth].join(',')");
    CHECK(wait_for_title(c, "undefined,undefined,undefined,undefined"));
    char args[256], handle[32], out[256];
    contents_arg(c, handle, sizeof handle);
    snprintf(args, sizeof args,
             "{\"contents\":\"%s\",\"name\":\"profile.web_app.install_by_user_enabled\"}", handle);
    control("pref.json", args, out, sizeof out);
    CHECK(strstr(out, "\"value\":false") != NULL);
    finish(c);
}

/* navigator.credentials.get({password: true}) showed the account chooser as a views dialog. It is
   a sheet of the passwords surface; choosing the account gives the page Chrome's credential. */
static void test_the_password_account_chooser_is_a_sheet(void) {
    bdk_contents_t c = start_page(PAGE);
    t_capture_t cap;
    bdk_invoke_options_t o;
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.profile = g_profile;
    t_cap_init(&cap);
    REQUIRE(bdk_invoke_named(
                t_runtime, S("passwords.add"),
                S("{\"origin\":\"http://localhost\",\"username\":\"ada\",\"password\":\"s3cret\"}"),
                &o, t_on_result, NULL, &cap, t_on_release) != 0);
    for (int i = 0; i < 40 && cap.results == 0; ++i)
        t_pump();
    REQUIRE(cap.results == 1 && !cap.has_error);
    script("passwords", "account0");
    run(c, "navigator.credentials.get({password: true, mediation: 'required'})"
           ".then(cred => { document.title = cred ? 'got:' + cred.id : 'none'; },"
           "      e => { document.title = e.name; })");
    CHECK(wait_for_title(c, "got:ada"));
    char out[4096];
    record("passwords", out, sizeof out);
    CHECK(json_int(out, "shown") >= 1);
    finish(c);
}

const t_test_t t_engine_dialogs_tests[] = {
    {"engine_dialogs_https_first_asks_with_its_interstitial_page",
     test_https_first_asks_with_its_interstitial_page},
    {"engine_dialogs_an_external_protocol_asks_with_a_sheet",
     test_an_external_protocol_asks_with_a_sheet},
    {"engine_dialogs_a_tab_modal_dialog_model_is_a_sheet",
     test_a_tab_modal_dialog_model_is_a_sheet},
    {"engine_dialogs_device_choosers_are_refused", test_device_choosers_are_refused},
    {"engine_dialogs_screen_capture_is_refused", test_screen_capture_is_refused},
    {"engine_dialogs_apis_with_views_only_ui_are_absent", test_apis_with_views_only_ui_are_absent},
    {"engine_dialogs_the_password_account_chooser_is_a_sheet",
     test_the_password_account_chooser_is_a_sheet},
    {NULL, NULL},
};
