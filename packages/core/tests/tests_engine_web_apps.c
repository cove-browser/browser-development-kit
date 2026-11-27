/* Engine scenarios for installable web apps (web-apps.md, T-PWA-04): a fixture app the engine's
   interceptor serves (a page, its manifest and a 192 by 192 icon), installed through a page's
   beforeinstallprompt prompt() and through the commands, listed, launched as an app page and
   uninstalled. Default presenters are scripted, so no sheet blocks the suite. Built only into the
   engine conformance app (BDK_HARNESS_ENGINE_TESTS). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bdk/bdk_broker.h"
#include "bdk_testing.h"
#include "engine_http_server.h"
#include "test_support.h"

void bdk_harness_engine_show(bdk_contents_t contents, int visible);
bdk_native_handle_t bdk_harness_engine_gesture(bdk_contents_t contents, int kind);

#define ON "\"web_apps\":{\"enabled\":true}"

static const char* kManifest =
    "{\"name\":\"Example App\",\"short_name\":\"Example\",\"start_url\":\"/app/index.html\","
    "\"scope\":\"/app/\",\"display\":\"standalone\",\"theme_color\":\"#336699\","
    "\"icons\":[{\"src\":\"/icon.png\",\"sizes\":\"192x192\",\"type\":\"image/png\"}]}";

/* beforeinstallprompt is kept for prompt(); the page's title tells when it was first offered
   (Chromium offers again after a dismissal) and then what prompt() resolved to. */
static const char* kPage =
    "<html><head><title>app</title><link rel=\"manifest\" href=\"/manifest.json\"></head><body>"
    "<script>addEventListener('beforeinstallprompt', e => { e.preventDefault();"
    " if (!window.deferred) document.title = 'offered'; window.deferred = e; });"
    "addEventListener('appinstalled', () => window.wasInstalled = true);</script>"
    "</body></html>";

/* A service worker that tells its pages when a Background Sync event or a push message reaches
   it. A push shows a notification, as a subscription with userVisibleOnly must. */
static const char* kWorker =
    "const tell = m => clients.matchAll({includeUncontrolled: true})"
    ".then(cs => cs.forEach(c => c.postMessage(m)));"
    "self.addEventListener('sync', e => e.waitUntil(tell('synced:' + e.tag)));"
    "self.addEventListener('push', e => e.waitUntil(tell('pushed')"
    ".then(() => registration.showNotification('pushed'))));";

/* ---- the loopback server (engine_http_server.h) ----
   The renderer fetches the manifest and the browser the icon, which the engine's interceptor does
   not serve, so the app comes from 127.0.0.1, a secure context. */

static int start_server(void) {
    unsigned char icon[1024];
    const size_t icon_length = t_http_test_icon(icon, sizeof icon);
    static const char outside[] = "<html><head><title>outside</title></head></html>";
    const t_http_route_t routes[] = {
        {"/app/index.html", "text/html", kPage, strlen(kPage), NULL},
        {"/manifest.json", "application/manifest+json", kManifest, strlen(kManifest), NULL},
        {"/icon.png", "image/png", icon, icon_length, NULL},
        {"/elsewhere.html", "text/html", outside, sizeof outside - 1, NULL},
        {"/app/sw.js", "text/javascript", kWorker, strlen(kWorker), NULL},
    };
    return t_http_start(routes, 5);
}

#define PAGE t_http_url("/app/index.html")
#define OUTSIDE t_http_url("/elsewhere.html")

static bdk_profile_t g_profile;
static int g_calls;
static bdk_request_t g_request;
static char g_payload[8192];
static int g_complete; /* 1: grant with a gesture; 2: deny; 0: leave it open */
static bdk_contents_t g_page;

static void control(const char* verb, const char* args, char* out, size_t n) {
    REQUIRE(bdk_testing_command(t_runtime, S(verb), S(args), out, n) == BDK_OK);
}

static bdk_ui_response_t on_install(void* ctx, const bdk_ui_request_info_t* info) {
    (void)ctx;
    ++g_calls;
    g_request = info->request;
    snprintf(g_payload, sizeof g_payload, "%.*s", (int)info->payload_json.length,
             info->payload_json.data ? info->payload_json.data : "");
    if (g_complete) {
        bdk_error_t err = 0;
        bdk_native_handle_t none;
        memset(&none, 0, sizeof none);
        CHECK(bdk_ui_complete(info->request, S(g_complete == 1 ? "grant" : "deny"), S(""),
                              g_complete == 1 ? bdk_harness_engine_gesture(g_page, 0) : none,
                              &err) == BDK_OK);
    }
    return BDK_UI_HANDLED;
}

static int wait_for_title(bdk_contents_t c, const char* want) {
    char title[256] = "";
    for (int i = 0; i < 300; ++i) {
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
        bdk_testing_run_for(0.05);
    }
    printf("    title is \"%s\", not \"%s\"\n", title, want);
    return 0;
}

static void call(bdk_profile_t profile, bdk_contents_t page, const char* command, const char* args,
                 t_capture_t* cap);

/* A new profile whose app registry is ready: Chromium offers no page for installation before it
   is (webApps.list waits for it). */
static bdk_profile_t ready_profile(const char* name, int on) {
    bdk_profile_t p = t_new_profile(name);
    if (on) {
        t_capture_t cap;
        call(p, 0, "webApps.list", "{}", &cap);
    }
    return p;
}

/* A runtime with web apps on or off, a handler of installs unless `handler` is 0, and the app's
   page, shown. */
static bdk_contents_t start(int on, int handler) {
    g_calls = 0;
    g_request = 0;
    g_payload[0] = '\0';
    g_complete = 0;
    t_config_extra = on ? ON : NULL;
    t_start();
    t_config_extra = NULL;
    control("present.clear", "{}", NULL, 0);
    bdk_error_t err = 0;
    if (handler)
        REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_WEB_APP_INSTALLS, on_install, NULL, NULL,
                                   &err) == BDK_OK);
    t_freeze();
    REQUIRE(start_server());
    g_profile = ready_profile("Apps", on);
    g_page = t_new_contents(g_profile);
    /* Shown before it loads: Chromium checks a page for installability when it has loaded. */
    bdk_harness_engine_show(g_page, 1);
    REQUIRE(bdk_contents_focus(g_page) == BDK_OK);
    t_pump();
    REQUIRE(bdk_contents_load(g_page, S(PAGE), &err) == BDK_OK);
    t_pump();
    return g_page;
}

static void finish(bdk_contents_t c) {
    control("present.clear", "{}", NULL, 0);
    bdk_harness_engine_show(c, 0);
    for (int i = 0; i < 10; ++i)
        t_pump();
    t_http_stop();
}

static void run(bdk_contents_t c, const char* js) {
    REQUIRE(bdk_testing_execute_script(c, S(js), 1) == BDK_OK);
    t_pump();
}

static void call(bdk_profile_t profile, bdk_contents_t page, const char* command, const char* args,
                 t_capture_t* cap) {
    t_cap_init(cap);
    bdk_invoke_options_t o;
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.profile = profile;
    o.contents = page;
    REQUIRE(bdk_invoke_named(t_runtime, S(command), S(args), &o, t_on_result, t_on_progress, cap,
                             t_on_release) != 0);
    for (int i = 0; i < 200 && cap->results == 0; ++i) {
        t_pump();
        bdk_testing_run_for(0.05);
    }
    REQUIRE(cap->results == 1);
}

/* The app's id, from a list or an install result. */
static void app_id(const char* json, char* out, size_t n) {
    out[0] = '\0';
    const char* at = strstr(json, "\"id\":\"");
    if (!at)
        return;
    at += 6;
    size_t len = strcspn(at, "\"");
    snprintf(out, n, "%.*s", (int)(len < n ? len : n - 1), at);
}

/* Installs the page's app through its prompt(), granted by the handler. */
static void install_through_prompt(bdk_contents_t c) {
    REQUIRE(wait_for_title(c, "offered"));
    g_complete = 1;
    run(c, "deferred.prompt().then(r => document.title = r.outcome)");
    CHECK(wait_for_title(c, "accepted"));
}

/* ---- Scenarios ---- */

/* With [web_apps] enabled = false (the default) no page is offered for installation. */
static void test_web_apps_off_offer_nothing(void) {
    bdk_contents_t c = start(0, 1);
    for (int i = 0; i < 40; ++i) {
        t_pump();
        bdk_testing_run_for(0.05);
    }
    bdk_contents_state_t st;
    memset(&st, 0, sizeof st);
    st.struct_size = sizeof st;
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_state(c, &st, &err) == BDK_OK);
    CHECK(st.title.length == 3 && memcmp(st.title.data, "app", 3) == 0);
    CHECK(g_calls == 0);
    finish(c);
}

/* A page's prompt() is a request naming the app and its origin; a grant with the gesture installs
   it into the page's profile only, and a deny resolves "dismissed". */
static void test_a_handler_installs_from_the_page_prompt(void) {
    bdk_contents_t c = start(1, 1);
    REQUIRE(wait_for_title(c, "offered"));
    g_complete = 2;
    run(c, "deferred.prompt().then(r => document.title = r.outcome)");
    CHECK(wait_for_title(c, "dismissed"));
    CHECK(g_calls == 1);
    CHECK(strstr(g_payload, "\"source\":\"page\"") != NULL);
    CHECK(strstr(g_payload, "\"name\":\"Example App\"") != NULL);
    char origin[96];
    snprintf(origin, sizeof origin, "\"origin\":\"%s\"", t_http_origin());
    CHECK(strstr(g_payload, origin) != NULL);
    t_capture_t cap;
    call(g_profile, 0, "webApps.list", "{}", &cap);
    CHECK(strcmp(cap.json, "[]") == 0);

    /* Chromium offers a dismissed site again only days later: a second profile installs it, and
       only that profile lists it. */
    bdk_harness_engine_show(c, 0);
    bdk_profile_t other = ready_profile("Other", 1);
    bdk_contents_t again = t_new_contents(other);
    g_page = again;
    bdk_harness_engine_show(again, 1);
    REQUIRE(bdk_contents_focus(again) == BDK_OK);
    t_pump();
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_load(again, S(PAGE), &err) == BDK_OK);
    install_through_prompt(again);
    call(other, 0, "webApps.list", "{}", &cap);
    CHECK(strstr(cap.json, "\"name\":\"Example App\"") != NULL);
    CHECK(strstr(cap.json, "\"displayMode\":\"standalone\"") != NULL);
    call(g_profile, 0, "webApps.list", "{}", &cap);
    CHECK(strcmp(cap.json, "[]") == 0);
    finish(again);
}

/* A grant needs the gesture: without one the completion is refused. */
static void test_an_install_needs_the_gesture(void) {
    bdk_contents_t c = start(1, 1);
    REQUIRE(wait_for_title(c, "offered"));
    run(c, "deferred.prompt().then(r => document.title = r.outcome)");
    for (int i = 0; i < 100 && g_calls == 0; ++i)
        t_pump();
    REQUIRE(g_calls == 1);
    bdk_native_handle_t none;
    memset(&none, 0, sizeof none);
    bdk_error_t err = 0;
    CHECK(bdk_ui_complete(g_request, S("grant"), S(""), none, &err) != BDK_OK);
    bdk_ui_request_release(g_request);
    CHECK(wait_for_title(c, "dismissed"));
    finish(c);
}

/* With no handler BDK's sheet asks; Install installs. */
static void test_the_default_sheet_installs(void) {
    bdk_contents_t c = start(1, 0);
    control("present.script",
            "{\"surface\":\"web_app_installs\",\"answer\":{\"answer\":\"install\"}}", NULL, 0);
    REQUIRE(wait_for_title(c, "offered"));
    run(c, "deferred.prompt().then(r => document.title = r.outcome)");
    CHECK(wait_for_title(c, "accepted"));
    char out[2048];
    control("present.record", "{\"surface\":\"web_app_installs\"}", out, sizeof out);
    CHECK(strstr(out, "Example App") != NULL);
    finish(c);
}

typedef struct {
    int count;
    char last[4096];
} events_t;

static void on_event(void* ctx, uint32_t id, bdk_string_t name, bdk_string_t payload) {
    events_t* e = (events_t*)ctx;
    (void)id, (void)name;
    ++e->count;
    snprintf(e->last, sizeof e->last, "%.*s", (int)payload.length,
             payload.data ? payload.data : "");
}

static bdk_ui_response_t accept_window(void* ctx, const bdk_ui_request_info_t* info) {
    char* payload = (char*)ctx;
    snprintf(payload, 4096, "%.*s", (int)info->payload_json.length,
             info->payload_json.data ? info->payload_json.data : "");
    bdk_error_t err = 0;
    bdk_native_handle_t none;
    memset(&none, 0, sizeof none);
    CHECK(bdk_ui_complete(info->request, S("accept"), S(""), none, &err) == BDK_OK);
    return BDK_UI_HANDLED;
}

static void contents_events(void* ctx, const bdk_contents_event_t* ev) {
    events_t* e = (events_t*)ctx;
    if (ev->kind != BDK_CONTENTS_EVENT_WEB_APP)
        return;
    ++e->count;
    snprintf(e->last, sizeof e->last, "%.*s", (int)ev->detail_json.length,
             ev->detail_json.data ? ev->detail_json.data : "");
}

/* A launched app's page is a new-window request of kind webApp, reports display-mode standalone
   in scope, reports leaving the scope, sets a badge, and goes with an uninstall. */
static void test_a_launched_app_runs_as_an_app_page(void) {
    static char window[4096];
    window[0] = '\0';
    g_calls = 0;
    t_config_extra = ON;
    t_start();
    t_config_extra = NULL;
    control("present.clear", "{}", NULL, 0);
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_WEB_APP_INSTALLS, on_install, NULL, NULL,
                               &err) == BDK_OK);
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_NEW_WINDOWS, accept_window, window, NULL,
                               &err) == BDK_OK);
    events_t installed = {0}, badges = {0}, uninstalled = {0};
    REQUIRE(bdk_subscribe_named(t_runtime, S("webApps.installed"), S(""), NULL, on_event,
                                &installed, NULL) != 0);
    REQUIRE(bdk_subscribe_named(t_runtime, S("webApps.badgeChanged"), S(""), NULL, on_event,
                                &badges, NULL) != 0);
    REQUIRE(bdk_subscribe_named(t_runtime, S("webApps.uninstalled"), S(""), NULL, on_event,
                                &uninstalled, NULL) != 0);
    t_freeze();
    REQUIRE(start_server());
    g_profile = ready_profile("Apps", 1);
    g_page = t_new_contents(g_profile);
    bdk_harness_engine_show(g_page, 1);
    REQUIRE(bdk_contents_focus(g_page) == BDK_OK);
    t_pump();
    REQUIRE(bdk_contents_load(g_page, S(PAGE), &err) == BDK_OK);
    install_through_prompt(g_page);
    for (int i = 0; i < 50 && installed.count == 0; ++i)
        t_pump();
    CHECK(installed.count == 1);
    char id[64];
    app_id(installed.last, id, sizeof id);
    REQUIRE(id[0] != 0);

    char args[128];
    snprintf(args, sizeof args, "{\"id\":\"%s\"}", id);
    t_capture_t cap;
    call(g_profile, 0, "webApps.launch", args, &cap);
    REQUIRE(!cap.has_error);
    CHECK(strstr(window, "\"kind\":\"webApp\"") != NULL);
    CHECK(strstr(window, "\"webApp\":{") != NULL);
    const char* handle = strstr(cap.json, "\"contents\":\"");
    REQUIRE(handle != NULL);
    bdk_contents_t app = (bdk_contents_t)strtoull(handle + 12, NULL, 10);
    REQUIRE(app != 0);
    events_t scope = {0};
    REQUIRE(bdk_contents_subscribe(app, contents_events, &scope, NULL) != 0);
    bdk_harness_engine_show(app, 1);
    CHECK(wait_for_title(app, "app"));
    run(app, "document.title = matchMedia('(display-mode: standalone)').matches ? 'standalone'"
             " : 'browser'");
    CHECK(wait_for_title(app, "standalone"));
    run(app, "navigator.setAppBadge(3)");
    for (int i = 0; i < 50 && badges.count == 0; ++i)
        t_pump();
    CHECK(strstr(badges.last, "\"count\":3") != NULL);
    REQUIRE(bdk_contents_load(app, S(OUTSIDE), &err) == BDK_OK);
    CHECK(wait_for_title(app, "outside"));
    for (int i = 0; i < 50 && scope.count == 0; ++i)
        t_pump();
    CHECK(strstr(scope.last, "\"isInScope\":false") != NULL);
    bdk_harness_engine_show(app, 0);

    call(g_profile, 0, "webApps.uninstall", args, &cap);
    CHECK(!cap.has_error);
    for (int i = 0; i < 50 && uninstalled.count == 0; ++i)
        t_pump();
    CHECK(uninstalled.count == 1);
    call(g_profile, 0, "webApps.list", "{}", &cap);
    CHECK(strcmp(cap.json, "[]") == 0);
    finish(g_page);
}

/* The installability command reports what the page would install. */
static void test_the_page_reports_its_installability(void) {
    bdk_contents_t c = start(1, 1);
    REQUIRE(wait_for_title(c, "offered"));
    t_capture_t cap;
    call(g_profile, c, "webApps.installability", "{}", &cap);
    CHECK(strstr(cap.json, "\"isInstallable\":true") != NULL);
    CHECK(strstr(cap.json, "\"name\":\"Example App\"") != NULL);
    CHECK(strstr(cap.json, "\"installedId\":null") != NULL);
    finish(c);
}

/* webApps.install {url} with no page: the URL loads hidden, the request names the app with source
   "command"; a deny is webApps.cancelled and a grant installs. A URL with no manifest is not
   installable, and the application's confirmed install raises no request. */
static void test_a_url_installs_with_no_page(void) {
    bdk_contents_t c = start(1, 1);
    char args[256];
    snprintf(args, sizeof args, "{\"url\":\"%s\"}", PAGE);
    t_capture_t cap;
    g_complete = 2;
    call(g_profile, 0, "webApps.install", args, &cap);
    if (!(cap.has_error && strcmp(cap.code, BDK_CODE_CANCELLED) == 0))
        printf("    got %s.%s: %s\n", cap.domain, cap.code, cap.message);
    CHECK(cap.has_error && strcmp(cap.code, BDK_CODE_CANCELLED) == 0);
    CHECK(g_calls == 1);
    CHECK(strstr(g_payload, "\"source\":\"command\"") != NULL);
    CHECK(strstr(g_payload, "\"name\":\"Example App\"") != NULL);
    g_complete = 1;
    call(g_profile, 0, "webApps.install", args, &cap);
    CHECK(!cap.has_error);
    CHECK(strstr(cap.json, "\"name\":\"Example App\"") != NULL);
    CHECK(g_calls == 2);

    snprintf(args, sizeof args, "{\"url\":\"%s\"}", OUTSIDE);
    call(g_profile, 0, "webApps.install", args, &cap);
    CHECK(cap.has_error && strcmp(cap.code, BDK_CODE_NOT_INSTALLABLE) == 0);
    CHECK(g_calls == 2);

    bdk_profile_t other = ready_profile("Other", 1);
    snprintf(args, sizeof args, "{\"url\":\"%s\"}", PAGE);
    t_cap_init(&cap);
    bdk_invoke_options_t o;
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.profile = other;
    REQUIRE(bdk_web_app_install(t_runtime, S(args), &o, bdk_harness_engine_gesture(c, 0),
                                t_on_result, &cap, t_on_release) != 0);
    for (int i = 0; i < 200 && cap.results == 0; ++i) {
        t_pump();
        bdk_testing_run_for(0.05);
    }
    CHECK(cap.results == 1 && !cap.has_error);
    CHECK(g_calls == 2);
    call(other, 0, "webApps.list", "{}", &cap);
    CHECK(strstr(cap.json, "\"name\":\"Example App\"") != NULL);
    finish(c);
}

/* An installed app's service worker (web-apps.md 9, a recorded result): a one-off Background Sync
   registered while online fires at once, and Periodic Background Sync registers, which Chromium
   allows only for an origin with an installed app. Its first event is at least 12 hours away
   (Chromium's minimum interval), so a scenario cannot see it. */
static void test_an_installed_app_gets_background_sync(void) {
    bdk_contents_t c = start(1, 1);
    install_through_prompt(c);
    run(c, "navigator.serviceWorker.onmessage = e => document.title = e.data;"
           "navigator.serviceWorker.register('/app/sw.js')"
           ".then(() => navigator.serviceWorker.ready)"
           ".then(reg => reg.periodicSync.register('p', {minInterval: 1000})"
           "  .then(() => 'ok', e => e.name)"
           "  .then(p => { window.periodic = p; return reg.sync.register('s'); }))"
           ".catch(e => document.title = 'sync ' + e.name)");
    CHECK(wait_for_title(c, "synced:s"));
    run(c, "document.title = 'periodic ' + window.periodic");
    CHECK(wait_for_title(c, "periodic ok"));
    finish(c);
}

/* ---- push through Google (T-PWA-05) ---- */

/* The sender (fixtures/push/send.mjs), run with node; its first `n` lines of output in `out`. */
static int push_sender(const char* args, char* out, size_t n) {
    char dir[512];
    snprintf(dir, sizeof dir, "%s", __FILE__);
    char* slash = strrchr(dir, '/');
    REQUIRE(slash != NULL);
    *slash = '\0';
    char command[4096];
    snprintf(command, sizeof command, "node '%s/fixtures/push/send.mjs' %s", dir, args);
    FILE* pipe = popen(command, "r");
    if (!pipe)
        return 0;
    size_t used = fread(out, 1, n - 1, pipe);
    out[used] = '\0';
    return pclose(pipe) == 0;
}

static bdk_ui_response_t grant_notifications(void* ctx, const bdk_ui_request_info_t* info) {
    (void)ctx;
    bdk_error_t err = 0;
    CHECK(bdk_ui_complete(info->request, S("grant"),
                          S("{\"kinds\":[\"notifications\"],\"duration\":\"persistent\"}"),
                          bdk_harness_engine_gesture(g_page, 0), &err) == BDK_OK);
    return BDK_UI_HANDLED;
}

#define PUSH_LAUNCH "engine_launch_a_push_from_google_reaches_the_app"
#define PUSH_CONFIG ON ",\"services\":{\"push\":{\"provider\":\"google\"}}"

static int g_push_prepared;

/* Called by the harness before its first start: Chromium reads the push provider as it starts, so
   this scenario runs in a launch of its own (scripts/test-engine.sh runs each engine_launch_*
   alone), and only when asked, since it contacts Google. */
void bdk_engine_push_launch_prepare(const char* name) {
    if (strcmp(name, PUSH_LAUNCH) != 0 || !getenv("BDK_TEST_PUSH_GOOGLE"))
        return;
    t_config_extra = PUSH_CONFIG;
    g_push_prepared = 1;
}

/* With [services.push] provider = "google" (and no Google key: push takes none), a page subscribes,
   its endpoint is Google's push service, and a push sent to it reaches the app's service worker. */
static void test_a_push_from_google_reaches_the_app(void) {
    if (!g_push_prepared) {
        printf("    skipped: run alone with BDK_TEST_PUSH_GOOGLE=1 and node on the PATH (it "
               "contacts Google)\n");
        return;
    }
    char keys[1024];
    REQUIRE(push_sender("keys", keys, sizeof keys));
    char* newline = strchr(keys, '\n');
    REQUIRE(newline != NULL);
    *newline = '\0';
    const char* public_key = keys;
    char* jwk = newline + 1;
    jwk[strcspn(jwk, "\n")] = '\0';

    t_config_extra = PUSH_CONFIG;
    bdk_start_config_t config;
    t_fill_start_config(&config);
    t_config_extra = NULL;
    bdk_error_t err = 0;
    if (bdk_runtime_start(&config, &t_runtime, &err) != BDK_OK) {
        bdk_string_t message = bdk_error_message(err);
        printf("    start: %.*s\n", (int)message.length, message.data ? message.data : "");
        REQUIRE(0);
    }
    control("present.clear", "{}", NULL, 0);
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_PERMISSIONS, grant_notifications, NULL, NULL,
                               &err) == BDK_OK);
    t_freeze();
    REQUIRE(start_server());
    g_profile = ready_profile("Push", 1);
    g_page = t_new_contents(g_profile);
    bdk_harness_engine_show(g_page, 1);
    REQUIRE(bdk_contents_focus(g_page) == BDK_OK);
    t_pump();
    REQUIRE(bdk_contents_load(g_page, S(PAGE), &err) == BDK_OK);
    REQUIRE(wait_for_title(g_page, "offered"));

    char js[1024];
    snprintf(js, sizeof js,
             "navigator.serviceWorker.onmessage = e => document.title = e.data;"
             "Notification.requestPermission()"
             ".then(() => navigator.serviceWorker.register('/app/sw.js'))"
             ".then(() => navigator.serviceWorker.ready)"
             ".then(reg => reg.pushManager.subscribe({userVisibleOnly: true,"
             "  applicationServerKey: '%s'}))"
             ".then(sub => document.title = 'endpoint ' + sub.endpoint,"
             "  e => document.title = 'refused ' + e.name + ': ' + e.message)",
             public_key);
    REQUIRE(bdk_testing_execute_script(g_page, S(js), 1) == BDK_OK);
    char title[1024] = "";
    for (int i = 0; i < 600; ++i) {
        bdk_contents_state_t st;
        memset(&st, 0, sizeof st);
        st.struct_size = sizeof st;
        REQUIRE(bdk_contents_state(g_page, &st, &err) == BDK_OK);
        snprintf(title, sizeof title, "%.*s", (int)st.title.length,
                 st.title.data ? st.title.data : "");
        if (strncmp(title, "endpoint ", 9) == 0 || strncmp(title, "refused ", 8) == 0)
            break;
        t_pump();
        bdk_testing_run_for(0.05);
    }
    printf("    %s\n", title);
    REQUIRE(strncmp(title, "endpoint https://fcm.googleapis.com/wp/", 39) == 0);

    char args[2048], status[64];
    snprintf(args, sizeof args, "send '%s' '%s'", title + 9, jwk);
    /* Google may refuse a registration for the first seconds after making it: once more, later. */
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!push_sender(args, status, sizeof status))
            snprintf(status, sizeof status, "nothing (node failed)\n");
        printf("    the push service answered %s", status);
        if (strncmp(status, "201", 3) == 0)
            break;
        for (int i = 0; i < 100; ++i) {
            t_pump();
            bdk_testing_run_for(0.05);
        }
    }
    CHECK(strncmp(status, "201", 3) == 0);
    CHECK(wait_for_title(g_page, "pushed"));

    /* The network audit of the run (tests_engine_privacy.c describes the net log): every host is
       the loopback server or one of the messaging hosts (web-apps.md 10.6). */
    const char* netlog = getenv("BDK_TEST_NETLOG");
    if (netlog && *netlog) {
        static const char* const kAllowed[] = {
            "127.0.0.1",        "localhost",          "android.clients.google.com",
            "mtalk.google.com", "fcm.googleapis.com", NULL};
        char hosts_args[600], hosts[8192];
        snprintf(hosts_args, sizeof hosts_args, "{\"path\":\"%s\"}", netlog);
        control("network.hosts", hosts_args, hosts, sizeof hosts);
        printf("    hosts: %s\n", hosts);
        for (const char* q = strchr(hosts, '['); q && (q = strchr(q, '"')) != NULL;) {
            const char* end = strchr(q + 1, '"');
            if (!end)
                break;
            const size_t n = (size_t)(end - q - 1);
            int allowed = 0;
            for (int i = 0; kAllowed[i]; ++i)
                allowed =
                    allowed || (strlen(kAllowed[i]) == n && strncmp(q + 1, kAllowed[i], n) == 0);
            if (!allowed)
                printf("    another host contacted: %.*s\n", (int)n, q + 1);
            CHECK(allowed);
            q = end + 1;
        }
    } else {
        printf("    network audit skipped: add BDK_TEST_NETLOG and --log-net-log\n");
    }
    finish(g_page);
}

const t_test_t t_engine_web_app_tests[] = {
    {"engine_web_apps_off_offer_nothing", test_web_apps_off_offer_nothing},
    {"engine_web_apps_a_handler_installs_from_the_page_prompt",
     test_a_handler_installs_from_the_page_prompt},
    {"engine_web_apps_an_install_needs_the_gesture", test_an_install_needs_the_gesture},
    {"engine_web_apps_the_default_sheet_installs", test_the_default_sheet_installs},
    {"engine_web_apps_the_page_reports_its_installability",
     test_the_page_reports_its_installability},
    {"engine_web_apps_a_launched_app_runs_as_an_app_page", test_a_launched_app_runs_as_an_app_page},
    {"engine_web_apps_a_url_installs_with_no_page", test_a_url_installs_with_no_page},
    {"engine_web_apps_an_installed_app_gets_background_sync",
     test_an_installed_app_gets_background_sync},
    {PUSH_LAUNCH, test_a_push_from_google_reaches_the_app},
    {NULL, NULL},
};
