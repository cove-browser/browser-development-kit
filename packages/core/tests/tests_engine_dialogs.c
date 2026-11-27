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
extern const char* bdk_harness_engine_surfaces;

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

/* Runs script in the page with no user gesture. */
static void run_no_gesture(bdk_contents_t c, const char* js) {
    REQUIRE(bdk_testing_execute_script(c, S(js), 0) == BDK_OK);
    t_pump();
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
   the dialog UI off, strict HTTPS-First asks with its interstitial page, in the page. The host goes
   to the network (the harness would serve it before HTTPS-First could upgrade it) and does not
   resolve (.test), so the HTTPS upgrade fails and Chrome falls back to asking. */
static void test_https_first_asks_with_its_interstitial_page(void) {
    bdk_contents_t c = start_page(PAGE);
    control("network.passthrough", "{\"host\":\"hfm.bdk.test\"}", NULL, 0);
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

/* The question is an external_protocols request (T-UIR-13): a handler answers it and no sheet is
   shown; with the surface disabled nothing is asked and nothing opens. */
typedef struct protocol_seen {
    int calls;
    char payload[1024];
} protocol_seen_t;

static protocol_seen_t g_protocol;

static bdk_ui_response_t deny_protocol(void* ctx, const bdk_ui_request_info_t* info) {
    protocol_seen_t* seen = (protocol_seen_t*)ctx;
    ++seen->calls;
    snprintf(seen->payload, sizeof seen->payload, "%.*s", (int)info->payload_json.length,
             info->payload_json.data ? info->payload_json.data : "");
    bdk_native_handle_t none;
    memset(&none, 0, sizeof none);
    bdk_error_t err = 0;
    CHECK(bdk_ui_complete(info->request, S("deny"), S(""), none, &err) == BDK_OK);
    return BDK_UI_HANDLED;
}

static void test_an_external_protocol_is_a_request_to_a_handler(void) {
    memset(&g_protocol, 0, sizeof g_protocol);
    t_start();
    control("present.clear", "{}", NULL, 0);
    script("external_protocols", "cancel");
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_EXTERNAL_PROTOCOLS, deny_protocol,
                               &g_protocol, NULL, &err) == BDK_OK);
    t_freeze();
    g_profile = t_new_profile("Protocols");
    bdk_contents_t c = t_new_contents(g_profile);
    REQUIRE(bdk_contents_load(c, S(PAGE), &err) == BDK_OK);
    t_pump();
    bdk_harness_engine_show(c, 1);
    REQUIRE(bdk_contents_focus(c) == BDK_OK);
    t_pump();
    /* An earlier scenario's launch used up the one Chrome accepts before a gesture. */
    control("protocols.permit", "{}", NULL, 0);
    run(c, "location.href = 'maps:?q=BDK'");
    for (int i = 0; i < 200 && g_protocol.calls == 0; ++i)
        t_pump();
    CHECK(g_protocol.calls == 1);
    CHECK(strstr(g_protocol.payload, "\"url\":\"maps:?q=BDK\"") != NULL);
    CHECK(strstr(g_protocol.payload, "\"initiatingOrigin\":\"http://localhost\"") != NULL);
    char out[4096];
    record("external_protocols", out, sizeof out);
    CHECK(json_int(out, "shown") <= 0);
    finish(c);
}

static void test_a_disabled_external_protocol_surface_asks_nothing(void) {
    bdk_harness_engine_surfaces = "{\"external_protocols\":\"disabled\"}";
    bdk_contents_t c = start_page(PAGE);
    script("external_protocols", "open");
    run(c, "location.href = 'maps:?q=BDK'");
    for (int i = 0; i < 100; ++i)
        t_pump();
    char out[4096];
    record("external_protocols", out, sizeof out);
    CHECK(json_int(out, "shown") <= 0);
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

/* The APIs whose UI is views only are off at start, so a site finds them unsupported, and a page
   never offers itself as an application to install. Payment Request is routed since T-UIR-14
   (tests_engine_payments.c checks it absent when its surface is disabled). */
static void test_apis_with_views_only_ui_are_absent(void) {
    bdk_contents_t c = start_page(PAGE);
    run(c, "document.title = [typeof IdentityCredential, typeof DigitalCredential,"
           " typeof navigator.bluetooth].join(',')");
    CHECK(wait_for_title(c, "undefined,undefined,undefined"));
    char args[256], handle[32], out[256];
    contents_arg(c, handle, sizeof handle);
    snprintf(args, sizeof args,
             "{\"contents\":\"%s\",\"name\":\"profile.web_app.install_by_user_enabled\"}", handle);
    control("pref.json", args, out, sizeof out);
    CHECK(strstr(out, "\"value\":false") != NULL);
    finish(c);
}

/* navigator.install() and the <install> element reach views dialogs whether web app installs are
   on or off (web-apps.md 10, T-PWA-06): both are absent, with [web_apps] enabled false and true,
   and the runtime keeps running. */
static void test_page_initiated_install_apis_are_absent(void) {
    for (int on = 0; on <= 1; ++on) {
        t_config_extra = on ? "\"web_apps\":{\"enabled\":true}" : NULL;
        bdk_contents_t c = start_page(PAGE);
        run(c, "(async () => { const r = [typeof navigator.install];"
               "try { await navigator.install(); r.push('installed'); }"
               "  catch (e) { r.push(e.name); }"
               "const el = document.createElement('install');"
               "document.body.appendChild(el); el.click();"
               "r.push(el instanceof HTMLUnknownElement ? 'unknown' : 'element');"
               "document.title = r.join(','); })()");
        CHECK(wait_for_title(c, "undefined,TypeError,unknown"));
        finish(c);
        t_stop();
        t_config_extra = NULL;
    }
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

/* ---- page health (ui-requests.md 5.19, T-UIR-15) ---- */

typedef struct health_seen {
    int calls;
    char payload[1024];
    const char* completion; /* how the handler answers; NULL leaves the request to the default */
    const char* args;
} health_seen_t;

static health_seen_t g_health;

static bdk_ui_response_t on_health(void* ctx, const bdk_ui_request_info_t* info) {
    health_seen_t* seen = (health_seen_t*)ctx;
    ++seen->calls;
    snprintf(seen->payload, sizeof seen->payload, "%.*s", (int)info->payload_json.length,
             info->payload_json.data ? info->payload_json.data : "");
    if (!seen->completion)
        return BDK_UI_USE_DEFAULT;
    bdk_native_handle_t none;
    memset(&none, 0, sizeof none);
    bdk_error_t err = 0;
    CHECK(bdk_ui_complete(info->request, S(seen->completion), S(seen->args ? seen->args : ""), none,
                          &err) == BDK_OK);
    return BDK_UI_HANDLED;
}

/* A runtime with (or without) a page_health handler and a page at `url`, shown. */
static bdk_contents_t start_health_page(int with_handler, const char* url) {
    memset(&g_health, 0, sizeof g_health);
    t_start();
    control("present.clear", "{}", NULL, 0);
    if (with_handler) {
        bdk_error_t err = 0;
        REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_PAGE_HEALTH, on_health, &g_health, NULL,
                                   &err) == BDK_OK);
    }
    t_freeze();
    g_profile = t_new_profile("Health");
    bdk_contents_t c = t_new_contents(g_profile);
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_load(c, S(url), &err) == BDK_OK);
    t_pump();
    bdk_harness_engine_show(c, 1);
    t_pump();
    return c;
}

static int shown_count(const char* surface) {
    char out[2048];
    record(surface, out, sizeof out);
    return json_int(out, "shown");
}

/* window.open with no gesture is blocked by Chromium's popup blocker: a notice listing it, to the
   handler, or with none to the default presenter. */
static void test_a_blocked_popup_raises_a_page_health_notice(void) {
    for (int with_handler = 1; with_handler >= 0; --with_handler) {
        bdk_contents_t c = start_health_page(with_handler, PAGE);
        if (with_handler) {
            g_health.completion = "deny";
        } else {
            script("page_health", "done");
        }
        run_no_gesture(c,
                       "window.open('http://localhost/blocked-popup'); document.title = 'opened'");
        CHECK(wait_for_title(c, "opened"));
        for (int i = 0;
             i < 100 && (with_handler ? g_health.calls == 0 : shown_count("page_health") <= 0); ++i)
            t_pump();
        if (with_handler) {
            CHECK(g_health.calls == 1);
            CHECK(strstr(g_health.payload, "\"kind\":\"blockedPopups\"") != NULL);
            CHECK(strstr(g_health.payload, "http://localhost/blocked-popup") != NULL);
        } else {
            CHECK(shown_count("page_health") == 1);
        }
        finish(c);
        t_stop();
    }
}

/* A page that never yields to an input event: Chromium's hang monitor, after its delay, asks
   whether to wait or end the page. The handler ends it, and so does the default presenter as the
   script answers. */
static void test_a_hung_page_raises_a_page_health_request(void) {
    for (int with_handler = 1; with_handler >= 0; --with_handler) {
        bdk_contents_t c = start_health_page(with_handler, PAGE);
        t_events_t ev;
        t_ev_init(&ev);
        REQUIRE(bdk_contents_subscribe(c, t_on_contents_event, &ev, t_on_event_release) != 0);
        if (with_handler) {
            g_health.completion = "accept";
            g_health.args = "{\"action\":\"terminate\"}";
        } else {
            script("page_health", "terminate");
        }
        run_no_gesture(c, "document.addEventListener('mousedown', () => {});"
                          "setTimeout(() => { for (;;) {} }, 50)");
        bdk_testing_run_for(0.5);
        /* Input the renderer must answer, and never does, starts the hang monitor: the page
           listens for the click on its main thread, which the loop holds. */
        REQUIRE(bdk_testing_click(c, 20, 20, 0) == BDK_OK);
        int gone = 0;
        for (int i = 0; i < 90 && !gone; ++i) {
            bdk_testing_run_for(0.5);
            for (int k = 0; k < ev.count; ++k)
                gone |= ev.kind[k] == BDK_CONTENTS_EVENT_RENDERER_GONE;
        }
        CHECK(gone);
        if (with_handler) {
            CHECK(g_health.calls == 1);
            CHECK(strstr(g_health.payload, "\"kind\":\"unresponsive\"") != NULL);
        } else {
            CHECK(shown_count("page_health") == 1);
        }
        finish(c);
        t_stop();
    }
}

const t_test_t t_engine_dialogs_tests[] = {
    {"engine_dialogs_https_first_asks_with_its_interstitial_page",
     test_https_first_asks_with_its_interstitial_page},
    {"engine_dialogs_an_external_protocol_asks_with_a_sheet",
     test_an_external_protocol_asks_with_a_sheet},
    {"engine_dialogs_an_external_protocol_is_a_request_to_a_handler",
     test_an_external_protocol_is_a_request_to_a_handler},
    {"engine_dialogs_a_disabled_external_protocol_surface_asks_nothing",
     test_a_disabled_external_protocol_surface_asks_nothing},
    {"engine_dialogs_a_tab_modal_dialog_model_is_a_sheet",
     test_a_tab_modal_dialog_model_is_a_sheet},
    {"engine_dialogs_apis_with_views_only_ui_are_absent", test_apis_with_views_only_ui_are_absent},
    {"engine_dialogs_a_blocked_popup_raises_a_page_health_notice",
     test_a_blocked_popup_raises_a_page_health_notice},
    {"engine_dialogs_a_hung_page_raises_a_page_health_request",
     test_a_hung_page_raises_a_page_health_request},
    {"engine_dialogs_page_initiated_install_apis_are_absent",
     test_page_initiated_install_apis_are_absent},
    {"engine_dialogs_the_password_account_chooser_is_a_sheet",
     test_the_password_account_chooser_is_a_sheet},
    {NULL, NULL},
};
