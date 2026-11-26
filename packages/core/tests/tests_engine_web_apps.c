/* Engine scenarios for installable web apps (web-apps.md, T-PWA-04): a fixture app the engine's
   interceptor serves (a page, its manifest and a 192 by 192 icon), installed through a page's
   beforeinstallprompt prompt() and through the commands, listed, launched as an app page and
   uninstalled. Default presenters are scripted, so no sheet blocks the suite. Built only into the
   engine conformance app (BDK_HARNESS_ENGINE_TESTS). */
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "bdk/bdk_broker.h"
#include "bdk_testing.h"
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

static const char* kIcon =
    "iVBORw0KGgoAAAANSUhEUgAAAMAAAADACAIAAADdvvtQAAABZElEQVR42u3SQQ0AAAjEsPOEJzzhFguEd5MqWJbqgbdI"
    "gIEwEAbCQGAgDISBMBAYCANhIAwEBsJAGAgDgYEwEAbCQBgIDISBMBAGAgNhIAyEgcBAGAgDYSAwEAbCQBgIA4GBMBAG"
    "wkBgIAyEgTAQGAgDYSAMBAbCQBgIA4GBMBAGwkAYCAyEgTAQBgIDYSAMhIHAQBgIA2EgMBAGwkAYCAOBgTAQBsJAYCAM"
    "hIEwEBgIA2EgDAQGwkAYCANhIDAQBsJAGAgMhIEwEAYCA2EgDISBwEAYCANhIDAQBsJAGAgDgYEwEAbCQGAgDISBMBAY"
    "CANhIAwEBsJAGAgDYSAwEAbCQBgIDISBMBAGAgNhIAyEgcBAGAgDYSAMpAIGwkAYCAOBgTAQBsJAYCAMhIEwEBgIA2Eg"
    "DAQGwkAYCANhIDAQBsJAGAgMhIEwEAYCA2EgDISBwEAYCANhIAwEBsJAGAgDwdUCCpUqFQe2NBgAAAAASUVORK5CYII=";

/* ---- the loopback server ----
   The app is served from 127.0.0.1, a secure context, by a child process: the engine's interceptor
   serves navigations only, and the renderer fetches the manifest and the browser the icon. The
   replies are built before the fork; the child makes only system calls. */

static pid_t g_server;
static int g_port;
static char g_origin[64];

typedef struct {
    const char* path;
    char reply[4096];
    size_t length;
} route_t;

static route_t g_routes[4];

static size_t decode_base64(const char* in, unsigned char* out, size_t cap) {
    static const char* abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t n = 0;
    unsigned value = 0;
    int bits = 0;
    for (const char* c = in; *c && *c != '='; ++c) {
        const char* at = strchr(abc, *c);
        if (!at)
            continue;
        value = (value << 6) | (unsigned)(at - abc);
        bits += 6;
        if (bits >= 8 && n < cap) {
            bits -= 8;
            out[n++] = (unsigned char)((value >> bits) & 0xff);
        }
    }
    return n;
}

static void route(int i, const char* path, const char* type, const void* body, size_t length) {
    route_t* r = &g_routes[i];
    r->path = path;
    int head = snprintf(r->reply, sizeof r->reply,
                        "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nCache-Control: no-store\r\n"
                        "Connection: close\r\nContent-Length: %zu\r\n\r\n",
                        type, length);
    REQUIRE(head > 0 && (size_t)head + length <= sizeof r->reply);
    memcpy(r->reply + head, body, length);
    r->length = (size_t)head + length;
}

static void serve(int listener) {
    for (;;) {
        int fd = accept(listener, NULL, NULL);
        if (fd < 0)
            continue;
        char request[4096];
        ssize_t n = read(fd, request, sizeof request - 1);
        request[n > 0 ? n : 0] = '\0';
        const char* sp = strchr(request, ' ');
        const route_t* found = NULL;
        for (int i = 0; sp && i < 4; ++i) {
            size_t len = strlen(g_routes[i].path);
            if (strncmp(sp + 1, g_routes[i].path, len) == 0 &&
                (sp[1 + len] == ' ' || sp[1 + len] == '?'))
                found = &g_routes[i];
        }
        if (found) {
            (void)!write(fd, found->reply, found->length);
        } else {
            static const char missing[] =
                "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            (void)!write(fd, missing, sizeof missing - 1);
        }
        close(fd);
    }
}

static int start_server(void) {
    unsigned char icon[1024];
    const size_t icon_length = decode_base64(kIcon, icon, sizeof icon);
    route(0, "/app/index.html", "text/html", kPage, strlen(kPage));
    route(1, "/manifest.json", "application/manifest+json", kManifest, strlen(kManifest));
    route(2, "/icon.png", "image/png", icon, icon_length);
    static const char outside[] = "<html><head><title>outside</title></head></html>";
    route(3, "/elsewhere.html", "text/html", outside, sizeof outside - 1);
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof a;
    if (listener < 0 || bind(listener, (struct sockaddr*)&a, sizeof a) != 0 ||
        listen(listener, 16) != 0 || getsockname(listener, (struct sockaddr*)&a, &len) != 0)
        return 0;
    g_port = ntohs(a.sin_port);
    snprintf(g_origin, sizeof g_origin, "http://127.0.0.1:%d", g_port);
    g_server = fork();
    if (g_server == 0) {
        /* Off the suite's output, which a pipe may be waiting to see closed. */
        int null = open("/dev/null", O_RDWR);
        dup2(null, 1);
        dup2(null, 2);
        serve(listener);
        _exit(0);
    }
    close(listener);
    return g_server > 0;
}

static void stop_server(void) {
    if (g_server > 0) {
        kill(g_server, SIGKILL); /* not SIGTERM: the child has Chromium's handler for it */
        waitpid(g_server, NULL, 0);
        g_server = 0;
    }
}

static const char* at(const char* path) {
    static char buf[4][128];
    static int next;
    char* out = buf[next++ % 4];
    snprintf(out, 128, "%s%s", g_origin, path);
    return out;
}

#define PAGE at("/app/index.html")
#define OUTSIDE at("/elsewhere.html")

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
    stop_server();
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
    snprintf(origin, sizeof origin, "\"origin\":\"%s\"", g_origin);
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

const t_test_t t_engine_web_app_tests[] = {
    {"engine_web_apps_off_offer_nothing", test_web_apps_off_offer_nothing},
    {"engine_web_apps_a_handler_installs_from_the_page_prompt",
     test_a_handler_installs_from_the_page_prompt},
    {"engine_web_apps_an_install_needs_the_gesture", test_an_install_needs_the_gesture},
    {"engine_web_apps_the_default_sheet_installs", test_the_default_sheet_installs},
    {"engine_web_apps_the_page_reports_its_installability",
     test_the_page_reports_its_installability},
    {"engine_web_apps_a_launched_app_runs_as_an_app_page", test_a_launched_app_runs_as_an_app_page},
    {NULL, NULL},
};
