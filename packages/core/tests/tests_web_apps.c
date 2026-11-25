/* Installable web apps (web-apps.md, T-PWA-02, T-PWA-03): the rules the core enforces for every
 * runtime (web apps off by default, the configuration read at start, runtime.info, the Google key
 * for push, a private profile, the confirmed host install) and, where the runtime has the stub's
 * registry (harness_web_apps.h), the install request, launching, scope, uninstalling and badges.
 * The engine's scenarios against a fixture app are T-PWA-04's. */
#include <stdlib.h>

#include "harness_web_apps.h"
#include "test_support.h"

#define DOC "https://app.example/index.html"
#define START "https://app.example/app/"
#define ON "\"web_apps\":{\"enabled\":true}"

static const char* kManifest =
    "{\"name\":\"Example\",\"short_name\":\"Ex\",\"start_url\":\"/app/\",\"scope\":\"/app/\","
    "\"display\":\"standalone\",\"theme_color\":\"#336699\","
    "\"icons\":[{\"src\":\"/icon.png\",\"sizes\":\"192x192\"}],"
    "\"screenshots\":[{\"src\":\"/shot.png\",\"sizes\":\"640x480\"}],"
    "\"shortcuts\":[{\"name\":\"New\",\"url\":\"/app/new\"}]}";

/* ---- helpers ---- */

static bdk_request_t g_request;
static uint32_t g_surface;
static int g_calls;
static char g_payload[4096];
static bdk_ui_response_t g_answer = BDK_UI_HANDLED;

static bdk_ui_response_t handler(void* ctx, const bdk_ui_request_info_t* info) {
    (void)ctx;
    ++g_calls;
    g_request = info->request;
    g_surface = info->surface;
    snprintf(g_payload, sizeof g_payload, "%.*s", (int)info->payload_json.length,
             info->payload_json.data);
    return g_answer;
}

static void reset(void) {
    g_request = 0;
    g_surface = 0;
    g_calls = 0;
    g_payload[0] = '\0';
    g_answer = BDK_UI_HANDLED;
}

static bdk_native_handle_t gesture(void) {
    bdk_native_handle_t g;
    memset(&g, 0, sizeof g);
    g.kind = BDK_NATIVE_NSEVENT;
    g.id = 7;
    return g;
}

static bdk_native_handle_t no_gesture(void) {
    bdk_native_handle_t g;
    memset(&g, 0, sizeof g);
    return g;
}

/* Starts with the harness's resolved configuration plus the given members (JSON text without
   braces, may be empty) and the Google key (NULL for none). */
static bdk_status_t start_with(const char* members, const char* key, bdk_error_t* err) {
    bdk_start_config_t c;
    t_fill_start_config(&c);
    const bdk_string_t base = c.resolved_config_json;
    REQUIRE(base.length > 1 && base.data[base.length - 1] == '}');
    static char resolved[4096];
    snprintf(resolved, sizeof resolved, "%.*s%s%s}", (int)(base.length - 1), base.data,
             base.length > 2 && members[0] ? "," : "", members);
    c.resolved_config_json = S(resolved);
    if (key)
        c.google_api_key = S(key);
    return bdk_runtime_start(&c, &t_runtime, err);
}

/* Started with members, a handler on the web_app_installs and new_windows surfaces unless
   without_handler, and frozen. */
static void start_frozen_with(const char* members, int without_handler) {
    reset();
    bdk_error_t err = 0;
    REQUIRE(start_with(members, NULL, &err) == BDK_OK);
    if (!without_handler) {
        REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_WEB_APP_INSTALLS, handler, NULL, NULL,
                                   &err) == BDK_OK);
        REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_NEW_WINDOWS, handler, NULL, NULL, &err) ==
                BDK_OK);
    }
    t_freeze();
}

static bdk_invoke_options_t options(bdk_profile_t profile, bdk_contents_t page) {
    bdk_invoke_options_t o;
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.profile = profile;
    o.contents = page;
    return o;
}

static bdk_call_t invoke(bdk_profile_t profile, bdk_contents_t page, const char* command,
                         const char* args, t_capture_t* cap) {
    t_cap_init(cap);
    bdk_invoke_options_t o = options(profile, page);
    return bdk_invoke_named(t_runtime, S(command), S(args), &o, t_on_result, t_on_progress, cap,
                            t_on_release);
}

/* Invokes and pumps; the result is in cap. */
static void call(bdk_profile_t profile, const char* command, const char* args, t_capture_t* cap) {
    REQUIRE(invoke(profile, 0, command, args, cap) != 0);
    t_pump();
    REQUIRE(cap->results == 1);
}

static int failed_with(const t_capture_t* cap, const char* domain, const char* code) {
    return cap->has_error && strcmp(cap->domain, domain) == 0 && strcmp(cap->code, code) == 0;
}

/* Events by name, counted with the last payload. */
typedef struct named_events {
    int count;
    char last[4096];
} named_events_t;

static void on_event(void* ctx, uint32_t id, bdk_string_t name, bdk_string_t payload) {
    named_events_t* e = (named_events_t*)ctx;
    (void)id, (void)name;
    ++e->count;
    snprintf(e->last, sizeof e->last, "%.*s", (int)payload.length, payload.data);
}

static void watch(const char* name, named_events_t* e) {
    memset(e, 0, sizeof *e);
    REQUIRE(bdk_subscribe_named(t_runtime, S(name), S(""), NULL, on_event, e, NULL) != 0);
}

/* A page of profile p at url, loaded. */
static bdk_contents_t page_at(bdk_profile_t p, const char* url) {
    bdk_contents_t c = t_new_contents(p);
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_load(c, S(url), &err) == BDK_OK);
    t_pump();
    return c;
}

/* Installs the fixture app into p from the command, granting the request. The app's id is in id. */
static void install_granted(bdk_profile_t p, char* id, size_t n) {
    t_capture_t cap;
    g_calls = 0;
    REQUIRE(invoke(p, 0, "webApps.install", "{\"url\":\"" DOC "\"}", &cap) != 0);
    t_pump();
    REQUIRE(g_calls == 1 && g_surface == BDK_SURFACE_WEB_APP_INSTALLS);
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_complete(g_request, S("grant"), S("{}"), gesture(), &err) == BDK_OK);
    t_pump();
    REQUIRE(cap.results == 1 && !cap.has_error);
    REQUIRE(t_json_string(cap.json, "id", id, n));
}

#define NEEDS_HOOKS()                                                                              \
    do {                                                                                           \
        if (!bdk_harness_web_app_hooks())                                                          \
            return;                                                                                \
    } while (0)

/* ---- Every runtime: the configuration and the core's refusals ---- */

static void test_webapps_every_command_fails_disabled_by_default(void) {
    start_frozen_with("", 1);
    bdk_profile_t p = t_new_profile("Apps");
    static const char* const kCommands[][2] = {
        {"webApps.list", "{}"},
        {"webApps.get", "{\"id\":\"x\"}"},
        {"webApps.installability", "{}"},
        {"webApps.install", "{\"url\":\"" DOC "\"}"},
        {"webApps.uninstall", "{\"id\":\"x\"}"},
        {"webApps.launch", "{\"id\":\"x\"}"},
    };
    for (size_t i = 0; i < sizeof kCommands / sizeof kCommands[0]; ++i) {
        t_capture_t cap;
        call(p, kCommands[i][0], kCommands[i][1], &cap);
        CHECK(failed_with(&cap, "webApps", "disabled"));
    }
    t_capture_t cap;
    t_cap_init(&cap);
    bdk_invoke_options_t o = options(p, 0);
    REQUIRE(bdk_web_app_install(t_runtime, S("{\"url\":\"" DOC "\"}"), &o, gesture(), t_on_result,
                                &cap, t_on_release) != 0);
    t_pump();
    CHECK(cap.results == 1 && failed_with(&cap, "webApps", "disabled"));
}

static void test_webapps_runtime_info_reports_web_apps_and_push(void) {
    t_capture_t cap;
    start_frozen_with("", 1);
    call(0, "runtime.info", "{}", &cap);
    CHECK(!cap.has_error);
    CHECK(strstr(cap.json, "\"webApps\":{\"enabled\":false}") != NULL);
    CHECK(strstr(cap.json, "\"push\":{\"provider\":\"none\"}") != NULL);
    t_stop();
    start_frozen_with(ON ",\"services\":{\"push\":{\"provider\":\"none\"}}", 1);
    call(0, "runtime.info", "{}", &cap);
    CHECK(strstr(cap.json, "\"webApps\":{\"enabled\":true}") != NULL);
    CHECK(strstr(cap.json, "\"push\":{\"provider\":\"none\"}") != NULL);
    CHECK(strstr(cap.json, "\"bdkVersion\":") != NULL); /* the backend's answer is kept */
}

static void test_webapps_invalid_configuration_fails_start(void) {
    bdk_error_t err = 0;
    CHECK(start_with("\"web_apps\":{\"enabled\":\"yes\"}", NULL, &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "config", "resolvedInvalid"));
    CHECK(start_with("\"web_apps\":true", NULL, &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "config", "resolvedInvalid"));
    CHECK(start_with("\"services\":{\"push\":{\"provider\":\"app\"}}", NULL, &err) ==
          BDK_ERR_FAILED);
    CHECK(t_error_is(err, "config", "resolvedInvalid"));
}

static void test_webapps_push_google_without_a_key_fails_start(void) {
    bdk_error_t err = 0;
    CHECK(start_with("\"services\":{\"push\":{\"provider\":\"google\"}}", NULL, &err) ==
          BDK_ERR_FAILED);
    CHECK(t_error_is(err, "config", "keyMissing"));
    /* Chromium takes the key once per process, so only a runtime with the stub's hooks starts
       with one here (tests_providers.c does the same). */
    if (!bdk_harness_web_app_hooks())
        return;
    REQUIRE(start_with("\"services\":{\"push\":{\"provider\":\"google\"}}", "AIza-test-key",
                       &err) == BDK_OK);
    t_freeze();
    t_capture_t cap;
    call(0, "runtime.info", "{}", &cap);
    CHECK(strstr(cap.json, "\"push\":{\"provider\":\"google\"}") != NULL);
}

static void test_webapps_install_surface_cannot_be_disabled(void) {
    bdk_error_t err = 0;
    CHECK(start_with("\"surfaces\":{\"web_app_installs\":\"disabled\"}", NULL, &err) ==
          BDK_ERR_FAILED);
    CHECK(t_error_is(err, "ui", "surfaceMode"));
    REQUIRE(start_with(ON ",\"surfaces\":{\"web_app_installs\":\"custom\"}", NULL, &err) == BDK_OK);
    CHECK(bdk_runtime_freeze_registrations(t_runtime, &err) == BDK_ERR_FAILED);
    CHECK(str_has(bdk_error_message(err), "web_app_installs"));
    CHECK(t_error_is(err, "config", "handlerMissing"));
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_WEB_APP_INSTALLS, handler, NULL, NULL,
                               &err) == BDK_OK);
    CHECK(bdk_runtime_freeze_registrations(t_runtime, &err) == BDK_OK);
}

static void test_webapps_private_profile_refuses(void) {
    start_frozen_with(ON, 0);
    bdk_profile_t p = 0;
    bdk_error_t err = 0;
    REQUIRE(bdk_profile_open_private(t_runtime, &p, &err) == BDK_OK);
    t_capture_t cap;
    call(p, "webApps.list", "{}", &cap);
    CHECK(!cap.has_error && strcmp(cap.json, "[]") == 0);
    call(p, "webApps.install", "{\"url\":\"" DOC "\"}", &cap);
    CHECK(failed_with(&cap, "webApps", "privateProfile"));
    call(p, "webApps.launch", "{\"id\":\"x\"}", &cap);
    CHECK(failed_with(&cap, "webApps", "privateProfile"));
    call(p, "webApps.get", "{\"id\":\"x\"}", &cap);
    CHECK(failed_with(&cap, "webApps", "notFound"));
    CHECK(g_calls == 0); /* nothing was ever asked */
    if (bdk_harness_web_app_hooks()) {
        REQUIRE(bdk_harness_web_app_manifest(t_runtime, DOC, kManifest) == BDK_OK);
        bdk_contents_t c = page_at(p, DOC);
        CHECK(bdk_harness_web_app_prompt(c, 1, &err) == BDK_ERR_FAILED);
        CHECK(t_error_is(err, "webApps", "privateProfile"));
    }
}

static void test_webapps_host_install_needs_its_gesture(void) {
    start_frozen_with(ON, 0);
    bdk_profile_t p = t_new_profile("Apps");
    t_capture_t cap;
    t_cap_init(&cap);
    bdk_invoke_options_t o = options(p, 0);
    REQUIRE(bdk_web_app_install(t_runtime, S("{\"url\":\"" DOC "\"}"), &o, no_gesture(),
                                t_on_result, &cap, t_on_release) != 0);
    t_pump();
    CHECK(cap.results == 1 && cap.releases == 1 && failed_with(&cap, "ui", "gestureRequired"));
    t_cap_init(&cap);
    REQUIRE(bdk_web_app_uninstall(t_runtime, S("{\"id\":\"x\"}"), &o, no_gesture(), t_on_result,
                                  &cap, t_on_release) != 0);
    t_pump();
    CHECK(failed_with(&cap, "ui", "gestureRequired"));
    CHECK(g_calls == 0);
}

/* ---- Where the runtime has the stub's registry ---- */

static void test_webapps_install_with_a_handler(void) {
    NEEDS_HOOKS();
    start_frozen_with(ON, 0);
    REQUIRE(bdk_harness_web_app_manifest(t_runtime, DOC, kManifest) == BDK_OK);
    bdk_profile_t p = t_new_profile("Apps");
    named_events_t installed;
    watch("webApps.installed", &installed);
    char id[64];
    install_granted(p, id, sizeof id);
    CHECK(strstr(g_payload, "\"source\":\"command\"") != NULL);
    CHECK(strstr(g_payload, "\"origin\":\"https://app.example\"") != NULL);
    CHECK(strstr(g_payload, "\"name\":\"Example\"") != NULL);
    CHECK(strstr(g_payload, "\"screenshots\":[{\"url\":\"https://app.example/shot.png\"") != NULL);
    CHECK(installed.count == 1 && strstr(installed.last, id) != NULL);
    t_capture_t cap;
    call(p, "webApps.list", "{}", &cap);
    CHECK(strstr(cap.json, id) != NULL);
    CHECK(strstr(cap.json, "\"startUrl\":\"" START "\"") != NULL);
    CHECK(strstr(cap.json, "\"displayMode\":\"standalone\"") != NULL);
    CHECK(strstr(cap.json, "\"shortcuts\":[{\"name\":\"New\",\"url\":\"" START "new\"") != NULL);
    char get_args[128];
    snprintf(get_args, sizeof get_args, "{\"id\":\"%s\"}", id);
    call(p, "webApps.get", get_args, &cap);
    CHECK(!cap.has_error && strstr(cap.json, "\"themeColor\":\"#336699\"") != NULL);
}

static void test_webapps_install_with_no_handler_installs_nothing(void) {
    NEEDS_HOOKS();
    start_frozen_with(ON, 1);
    REQUIRE(bdk_harness_web_app_manifest(t_runtime, DOC, kManifest) == BDK_OK);
    bdk_profile_t p = t_new_profile("Apps");
    t_capture_t cap;
    call(p, "webApps.install", "{\"url\":\"" DOC "\"}", &cap);
    CHECK(failed_with(&cap, "webApps", "cancelled"));
    bdk_harness_ui_outcome_t outcome;
    REQUIRE(bdk_harness_last_ui_outcome(t_runtime, &outcome) == BDK_OK);
    CHECK(outcome.kind == BDK_HARNESS_UI_DEFAULTED);
    call(p, "webApps.list", "{}", &cap);
    CHECK(strcmp(cap.json, "[]") == 0);
}

static void test_webapps_install_cancelled_or_without_a_gesture(void) {
    NEEDS_HOOKS();
    start_frozen_with(ON, 0);
    REQUIRE(bdk_harness_web_app_manifest(t_runtime, DOC, kManifest) == BDK_OK);
    bdk_profile_t p = t_new_profile("Apps");
    t_capture_t cap;
    REQUIRE(invoke(p, 0, "webApps.install", "{\"url\":\"" DOC "\"}", &cap) != 0);
    t_pump();
    REQUIRE(g_calls == 1);
    bdk_error_t err = 0;
    /* install is a granting completion: refused without the event, and the request stays open. */
    CHECK(bdk_ui_complete(g_request, S("grant"), S("{}"), no_gesture(), &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "ui", "gestureRequired"));
    CHECK(bdk_ui_complete(g_request, S("install"), S("{}"), no_gesture(), &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "ui", "gestureRequired"));
    t_pump();
    CHECK(cap.results == 0);
    REQUIRE(bdk_ui_complete(g_request, S("deny"), S("{}"), no_gesture(), &err) == BDK_OK);
    t_pump();
    CHECK(failed_with(&cap, "webApps", "cancelled"));
    call(p, "webApps.list", "{}", &cap);
    CHECK(strcmp(cap.json, "[]") == 0);
}

static void test_webapps_host_install_raises_no_request(void) {
    NEEDS_HOOKS();
    start_frozen_with(ON, 0);
    REQUIRE(bdk_harness_web_app_manifest(t_runtime, DOC, kManifest) == BDK_OK);
    bdk_profile_t p = t_new_profile("Apps");
    bdk_contents_t c = page_at(p, DOC);
    t_capture_t cap;
    t_cap_init(&cap);
    bdk_invoke_options_t o = options(p, c); /* no url: the page's own manifest */
    REQUIRE(bdk_web_app_install(t_runtime, S("{}"), &o, gesture(), t_on_result, &cap,
                                t_on_release) != 0);
    t_pump();
    CHECK(cap.results == 1 && !cap.has_error && strstr(cap.json, "\"name\":\"Example\"") != NULL);
    CHECK(g_calls == 0);
    char id[64], args[128];
    REQUIRE(t_json_string(cap.json, "id", id, sizeof id));
    snprintf(args, sizeof args, "{\"id\":\"%s\"}", id);
    named_events_t gone;
    watch("webApps.uninstalled", &gone);
    t_cap_init(&cap);
    REQUIRE(bdk_web_app_uninstall(t_runtime, S(args), &o, gesture(), t_on_result, &cap,
                                  t_on_release) != 0);
    t_pump();
    CHECK(cap.results == 1 && !cap.has_error);
    CHECK(gone.count == 1 && strstr(gone.last, id) != NULL);
}

static void test_webapps_installability_reports_errors(void) {
    NEEDS_HOOKS();
    start_frozen_with(ON, 0);
    bdk_profile_t p = t_new_profile("Apps");
    REQUIRE(bdk_harness_web_app_manifest(t_runtime, DOC, kManifest) == BDK_OK);
    REQUIRE(bdk_harness_web_app_manifest(t_runtime, "https://plain.example/",
                                         "{\"name\":\"Plain\",\"display\":\"browser\"}") == BDK_OK);
    t_capture_t cap;
    REQUIRE(invoke(p, 0, "webApps.installability", "{}", &cap) != 0);
    t_pump();
    CHECK(failed_with(&cap, "broker", "invalidArguments")); /* no page */
    bdk_contents_t plain = page_at(p, "https://plain.example/");
    REQUIRE(invoke(p, plain, "webApps.installability", "{}", &cap) != 0);
    t_pump();
    CHECK(strstr(cap.json, "\"isInstallable\":false") != NULL);
    CHECK(strstr(cap.json, "display is browser") != NULL);
    CHECK(strstr(cap.json, "no icon") != NULL);
    call(p, "webApps.install", "{\"url\":\"https://plain.example/\"}", &cap);
    CHECK(failed_with(&cap, "webApps", "notInstallable"));
    CHECK(g_calls == 0);
    bdk_contents_t c = page_at(p, DOC);
    REQUIRE(invoke(p, c, "webApps.installability", "{}", &cap) != 0);
    t_pump();
    CHECK(strstr(cap.json, "\"isInstallable\":true,\"installedId\":null") != NULL);
    char id[64];
    install_granted(p, id, sizeof id);
    REQUIRE(invoke(p, c, "webApps.installability", "{}", &cap) != 0);
    t_pump();
    char installed[64];
    REQUIRE(t_json_string(cap.json, "installedId", installed, sizeof installed));
    CHECK(strcmp(installed, id) == 0);
}

static void test_webapps_page_prompt_raises_the_request(void) {
    NEEDS_HOOKS();
    start_frozen_with(ON, 0);
    REQUIRE(bdk_harness_web_app_manifest(t_runtime, DOC, kManifest) == BDK_OK);
    bdk_profile_t p = t_new_profile("Apps");
    bdk_contents_t c = page_at(p, DOC);
    bdk_error_t err = 0;
    CHECK(bdk_harness_web_app_prompt(c, 0, &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "webApps", "gestureRequired"));
    t_pump();
    CHECK(g_calls == 0);
    named_events_t installed;
    watch("webApps.installed", &installed);
    REQUIRE(bdk_harness_web_app_prompt(c, 1, &err) == BDK_OK);
    t_pump();
    REQUIRE(g_calls == 1 && g_surface == BDK_SURFACE_WEB_APP_INSTALLS);
    CHECK(strstr(g_payload, "\"source\":\"page\"") != NULL);
    REQUIRE(bdk_ui_complete(g_request, S("grant"), S("{}"), gesture(), &err) == BDK_OK);
    t_pump();
    CHECK(installed.count == 1);
}

static void test_webapps_page_prompt_never_asks_while_off(void) {
    NEEDS_HOOKS();
    start_frozen_with("", 0);
    REQUIRE(bdk_harness_web_app_manifest(t_runtime, DOC, kManifest) == BDK_OK);
    bdk_profile_t p = t_new_profile("Apps");
    bdk_contents_t c = page_at(p, DOC);
    bdk_error_t err = 0;
    CHECK(bdk_harness_web_app_prompt(c, 1, &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "webApps", "disabled"));
    t_pump();
    CHECK(g_calls == 0);
}

static void test_webapps_same_app_in_two_profiles_is_two_installs(void) {
    NEEDS_HOOKS();
    start_frozen_with(ON, 0);
    REQUIRE(bdk_harness_web_app_manifest(t_runtime, DOC, kManifest) == BDK_OK);
    bdk_profile_t work = t_new_profile("Work"), home = t_new_profile("Home");
    char a[64], b[64];
    install_granted(work, a, sizeof a);
    install_granted(home, b, sizeof b);
    CHECK(strcmp(a, b) == 0); /* the identifier comes from the manifest */
    t_capture_t cap;
    char args[128];
    snprintf(args, sizeof args, "{\"id\":\"%s\"}", a);
    call(work, "webApps.uninstall", args, &cap);
    CHECK(!cap.has_error);
    call(work, "webApps.list", "{}", &cap);
    CHECK(strcmp(cap.json, "[]") == 0);
    call(home, "webApps.list", "{}", &cap);
    CHECK(strstr(cap.json, b) != NULL);
}

static void test_webapps_launch_raises_a_web_app_window_request(void) {
    NEEDS_HOOKS();
    start_frozen_with(ON, 0);
    REQUIRE(bdk_harness_web_app_manifest(t_runtime, DOC, kManifest) == BDK_OK);
    bdk_profile_t p = t_new_profile("Apps");
    char id[64], args[192];
    install_granted(p, id, sizeof id);
    t_capture_t cap;
    call(p, "webApps.launch", "{\"id\":\"nothing\"}", &cap);
    CHECK(failed_with(&cap, "webApps", "notFound"));
    snprintf(args, sizeof args, "{\"id\":\"%s\",\"shortcutUrl\":\"https://elsewhere.example/\"}",
             id);
    call(p, "webApps.launch", args, &cap);
    CHECK(failed_with(&cap, "broker", "invalidArguments"));
    g_calls = 0;
    snprintf(args, sizeof args, "{\"id\":\"%s\",\"shortcutUrl\":\"" START "new\"}", id);
    call(p, "webApps.launch", args, &cap);
    REQUIRE(!cap.has_error);
    char handle[32];
    REQUIRE(t_json_string(cap.json, "contents", handle, sizeof handle));
    t_pump();
    REQUIRE(g_calls == 1 && g_surface == BDK_SURFACE_NEW_WINDOWS);
    CHECK(strstr(g_payload, "\"kind\":\"webApp\"") != NULL);
    CHECK(strstr(g_payload, "\"targetURL\":\"" START "new\"") != NULL);
    CHECK(strstr(g_payload, "\"webApp\":{\"id\":") != NULL);
    char named[32];
    REQUIRE(t_json_string(g_payload, "newContents", named, sizeof named));
    CHECK(strcmp(named, handle) == 0);
    char app_profile[64], page_profile[64];
    REQUIRE(t_json_string(g_payload, "profile", app_profile, sizeof app_profile));
    bdk_contents_t page = (bdk_contents_t)strtoull(handle, NULL, 10);
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_complete(g_request, S("accept"), S("{}"), no_gesture(), &err) == BDK_OK);
    t_pump();
    /* The page is in the app's profile, loading the shortcut. */
    REQUIRE(invoke(0, page, "contents.get", "{}", &cap) != 0);
    t_pump();
    REQUIRE(!cap.has_error &&
            t_json_string(cap.json, "profile", page_profile, sizeof page_profile));
    CHECK(strcmp(app_profile, page_profile) == 0);
    CHECK(strstr(cap.json, "\"url\":\"" START "new\"") != NULL);
    (void)p;
}

static void test_webapps_a_rejected_launch_destroys_the_page(void) {
    NEEDS_HOOKS();
    start_frozen_with(ON, 0);
    REQUIRE(bdk_harness_web_app_manifest(t_runtime, DOC, kManifest) == BDK_OK);
    bdk_profile_t p = t_new_profile("Apps");
    char id[64], args[128], handle[32];
    install_granted(p, id, sizeof id);
    snprintf(args, sizeof args, "{\"id\":\"%s\"}", id);
    t_capture_t cap;
    call(p, "webApps.launch", args, &cap);
    REQUIRE(t_json_string(cap.json, "contents", handle, sizeof handle));
    t_pump();
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_complete(g_request, S("deny"), S("{}"), no_gesture(), &err) == BDK_OK);
    t_pump();
    bdk_contents_t page = (bdk_contents_t)strtoull(handle, NULL, 10);
    CHECK(bdk_contents_load(page, S(START), &err) == BDK_ERR_STALE_HANDLE);
}

static void test_webapps_leaving_the_scope_is_reported(void) {
    NEEDS_HOOKS();
    start_frozen_with(ON, 0);
    REQUIRE(bdk_harness_web_app_manifest(t_runtime, DOC, kManifest) == BDK_OK);
    bdk_profile_t p = t_new_profile("Apps");
    char id[64], args[128], handle[32];
    install_granted(p, id, sizeof id);
    snprintf(args, sizeof args, "{\"id\":\"%s\"}", id);
    t_capture_t cap;
    call(p, "webApps.launch", args, &cap);
    REQUIRE(t_json_string(cap.json, "contents", handle, sizeof handle));
    t_pump();
    bdk_contents_t page = (bdk_contents_t)strtoull(handle, NULL, 10);
    t_events_t ev;
    t_ev_init(&ev);
    REQUIRE(bdk_contents_subscribe(page, t_on_contents_event, &ev, t_on_event_release) != 0);
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_load(page, S("https://elsewhere.example/"), &err) == BDK_OK);
    t_pump();
    REQUIRE(bdk_contents_load(page, S(START "inbox"), &err) == BDK_OK);
    t_pump();
    int out = -1, back = -1;
    for (int i = 0; i < ev.count; ++i) {
        if (ev.kind[i] != BDK_CONTENTS_EVENT_WEB_APP)
            continue;
        if (strstr(ev.detail[i], "\"isInScope\":false") && strstr(ev.url[i], "elsewhere"))
            out = i;
        if (strstr(ev.detail[i], "\"isInScope\":true") && out >= 0)
            back = i;
    }
    CHECK(out >= 0);
    CHECK(back > out);
    CHECK(out >= 0 && strstr(ev.detail[out], id) != NULL);
}

static void test_webapps_badge_changes_are_events(void) {
    NEEDS_HOOKS();
    start_frozen_with(ON, 0);
    REQUIRE(bdk_harness_web_app_manifest(t_runtime, DOC, kManifest) == BDK_OK);
    bdk_profile_t p = t_new_profile("Apps");
    bdk_contents_t outside = page_at(p, "https://elsewhere.example/");
    bdk_error_t err = 0;
    CHECK(bdk_harness_web_app_badge(outside, "{\"kind\":\"flag\"}", &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "webApps", "notFound"));
    char id[64];
    install_granted(p, id, sizeof id);
    bdk_contents_t c = page_at(p, START);
    named_events_t badge;
    watch("webApps.badgeChanged", &badge);
    REQUIRE(bdk_harness_web_app_badge(c, "{\"kind\":\"count\",\"count\":3}", &err) == BDK_OK);
    t_pump();
    CHECK(badge.count == 1 && strstr(badge.last, "\"count\":3") != NULL);
    t_capture_t cap;
    call(p, "webApps.list", "{}", &cap);
    CHECK(strstr(cap.json, "\"badge\":{\"kind\":\"count\",\"count\":3}") != NULL);
    REQUIRE(bdk_harness_web_app_badge(c, "null", &err) == BDK_OK);
    t_pump();
    CHECK(badge.count == 2 && strstr(badge.last, "\"badge\":null") != NULL);
}

const t_test_t t_web_app_tests[] = {
    {"webapps.every_command_fails_disabled_by_default",
     test_webapps_every_command_fails_disabled_by_default},
    {"webapps.runtime_info_reports_web_apps_and_push",
     test_webapps_runtime_info_reports_web_apps_and_push},
    {"webapps.invalid_configuration_fails_start", test_webapps_invalid_configuration_fails_start},
    {"webapps.push_google_without_a_key_fails_start",
     test_webapps_push_google_without_a_key_fails_start},
    {"webapps.install_surface_cannot_be_disabled", test_webapps_install_surface_cannot_be_disabled},
    {"webapps.private_profile_refuses", test_webapps_private_profile_refuses},
    {"webapps.host_install_needs_its_gesture", test_webapps_host_install_needs_its_gesture},
    {"webapps.install_with_a_handler", test_webapps_install_with_a_handler},
    {"webapps.install_with_no_handler_installs_nothing",
     test_webapps_install_with_no_handler_installs_nothing},
    {"webapps.install_cancelled_or_without_a_gesture",
     test_webapps_install_cancelled_or_without_a_gesture},
    {"webapps.host_install_raises_no_request", test_webapps_host_install_raises_no_request},
    {"webapps.installability_reports_errors", test_webapps_installability_reports_errors},
    {"webapps.page_prompt_raises_the_request", test_webapps_page_prompt_raises_the_request},
    {"webapps.page_prompt_never_asks_while_off", test_webapps_page_prompt_never_asks_while_off},
    {"webapps.same_app_in_two_profiles_is_two_installs",
     test_webapps_same_app_in_two_profiles_is_two_installs},
    {"webapps.launch_raises_a_web_app_window_request",
     test_webapps_launch_raises_a_web_app_window_request},
    {"webapps.a_rejected_launch_destroys_the_page",
     test_webapps_a_rejected_launch_destroys_the_page},
    {"webapps.leaving_the_scope_is_reported", test_webapps_leaving_the_scope_is_reported},
    {"webapps.badge_changes_are_events", test_webapps_badge_changes_are_events},
    {NULL, NULL},
};
