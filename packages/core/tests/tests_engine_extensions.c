/* Scenarios for installing, managing and policing extensions on the engine (T-EXT-05, T-EXT-06,
   T-EXT-09): install prompts and their gestures, the source policy, packed and update-URL
   installs, updates, compatibility checks, uninstall, private profiles and the store. Built only
   into the engine conformance app (BDK_HARNESS_ENGINE_TESTS). Fixture extensions are the
   `install-*` directories of tests/extensions/fixtures (BDK_EXTENSION_FIXTURES); packages are
   made at test time by the engine's `extensions.pack` test hook, and update manifests and
   packages are served by `extensions.serve` (bdk_testing.h). What the default presenter would ask
   is answered by `present.script`. */
#include <stdlib.h>
#include <unistd.h>

#include "bdk_testing.h"
#include "test_support.h"

#ifndef BDK_EXTENSION_FIXTURES
#error "BDK_EXTENSION_FIXTURES names the fixture directory"
#endif

/* From harness_engine.mm: the "extensions" object of the resolved configuration the next runtime
   starts with. */
extern const char* bdk_harness_engine_extensions;

bdk_native_handle_t bdk_harness_engine_gesture(bdk_contents_t contents, int kind);
int bdk_harness_engine_window_open(bdk_runtime_t rt);
void bdk_harness_engine_window_attach(int window, bdk_contents_t contents);
void bdk_harness_engine_window_close(int window);

/* ---- helpers ---- */

static void control(const char* verb, const char* args, char* out, size_t n) {
    REQUIRE(bdk_testing_command(t_runtime, S(verb), S(args), out, n) == BDK_OK);
}

static void script_presenter(const char* surface, const char* answer) {
    char args[512];
    snprintf(args, sizeof args, "{\"surface\":\"%s\",\"answer\":{\"answer\":\"%s\"}}", surface,
             answer);
    control("present.script", args, NULL, 0);
}

static int presenter_uses(const char* surface) {
    char args[96], out[8192];
    snprintf(args, sizeof args, "{\"surface\":\"%s\"}", surface);
    control("present.record", args, out, sizeof out);
    const char* at = strstr(out, "\"shown\":");
    return at ? atoi(at + 8) : -1;
}

/* What the default presenter was last asked to show (its JSON, escapes included). */
static void presenter_record(const char* surface, char* out, size_t n) {
    char args[96];
    snprintf(args, sizeof args, "{\"surface\":\"%s\"}", surface);
    control("present.record", args, out, n);
}

static int g_window = -1;

static void close_window(void) {
    if (g_window >= 0)
        bdk_harness_engine_window_close(g_window);
    g_window = -1;
}

/* A page in a window of the application: a prompt needs a page of the profile to be shown for, and
   a gesture needs a window. */
static bdk_contents_t new_active_page(bdk_profile_t profile) {
    bdk_contents_t c = t_new_contents(profile);
    int window = bdk_harness_engine_window_open(t_runtime);
    REQUIRE(window >= 0);
    g_window = window;
    bdk_harness_engine_window_attach(window, c);
    REQUIRE(bdk_contents_focus(c) == BDK_OK);
    t_pump();
    return c;
}

static void pump_for(double seconds) {
    t_pump();
    bdk_testing_run_for(seconds);
}

/* Starts a built-in command on the profile; the result arrives in `cap` while the test pumps. */
static void start(bdk_profile_t profile, const char* command, const char* args, t_capture_t* cap) {
    bdk_invoke_options_t o;
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.profile = profile;
    t_cap_init(cap);
    REQUIRE(bdk_invoke_named(t_runtime, S(command), S(args), &o, t_on_result, NULL, cap,
                             t_on_release) != 0);
}

/* Pumps, in real time, until the command answers (unpacking and the extension system take real
   time). */
static void finish(t_capture_t* cap) {
    for (int i = 0; i < 400 && cap->results == 0; ++i)
        pump_for(0.05);
    REQUIRE(cap->results == 1);
}

static void invoke(bdk_profile_t profile, const char* command, const char* args, t_capture_t* cap) {
    start(profile, command, args, cap);
    finish(cap);
}

static void invoke_ok(bdk_profile_t profile, const char* command, const char* args,
                      t_capture_t* cap) {
    invoke(profile, command, args, cap);
    if (cap->has_error)
        printf("    %s failed: %s.%s: %s\n", command, cap->domain, cap->code, cap->message);
    REQUIRE(!cap->has_error);
}

static int failed_with(const t_capture_t* cap, const char* code) {
    if (!cap->has_error || strcmp(cap->domain, "extension") != 0 || strcmp(cap->code, code) != 0) {
        printf("    wanted extension.%s, got %s: %s.%s: %s\n", code,
               cap->has_error ? "an error" : "a result", cap->domain, cap->code, cap->message);
        return 0;
    }
    return 1;
}

typedef struct {
    int calls;
    bdk_request_t request;
    char payload[65536]; /* a request carries the extension's icon as a data URL */
} seen_t;

static seen_t g_seen;

/* Takes the request and holds it open, for the test to complete. */
static bdk_ui_response_t hold(void* ctx, const bdk_ui_request_info_t* info) {
    seen_t* seen = (seen_t*)ctx;
    ++seen->calls;
    seen->request = info->request;
    snprintf(seen->payload, sizeof seen->payload, "%.*s", (int)info->payload_json.length,
             info->payload_json.data ? info->payload_json.data : "");
    return BDK_UI_HANDLED;
}

static bdk_native_handle_t gesture(bdk_contents_t c, int kind) {
    return bdk_harness_engine_gesture(c, kind);
}

static void wait_for_request(void) {
    for (int i = 0; i < 400 && g_seen.calls == 0; ++i)
        pump_for(0.05);
    REQUIRE(g_seen.calls == 1);
}

/* The events of one name a subscription received. */
typedef struct {
    int count;
    char payloads[16384];
} event_log_t;

static void on_event(void* ctx, uint32_t id, bdk_string_t name, bdk_string_t payload) {
    event_log_t* log = (event_log_t*)ctx;
    (void)id;
    (void)name;
    size_t used = strlen(log->payloads);
    ++log->count;
    snprintf(log->payloads + used, sizeof log->payloads - used, "%.*s\n", (int)payload.length,
             payload.data);
}

static void subscribe_to(const char* event, bdk_profile_t profile, event_log_t* log) {
    bdk_invoke_options_t o;
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.profile = profile;
    memset(log, 0, sizeof *log);
    REQUIRE(bdk_subscribe_named(t_runtime, S(event), S(""), &o, on_event, log, NULL) != 0);
}

static int wait_for_payload(const event_log_t* log, const char* text) {
    for (int i = 0; i < 200 && !strstr(log->payloads, text); ++i)
        pump_for(0.05);
    if (!strstr(log->payloads, text))
        printf("    no event with \"%s\"; got:\n%s", text, log->payloads);
    return strstr(log->payloads, text) != NULL;
}

/* An unpacked install of a fixture, as the argument of extensions.install. */
static void unpacked(const char* fixture, char* out, size_t n) {
    snprintf(out, n, "{\"source\":{\"kind\":\"unpacked\",\"path\":\"%s/%s\"}}",
             BDK_EXTENSION_FIXTURES, fixture);
}

/* Installs a fixture with the default presenter answering "install"; returns the id. */
static void install_fixture(bdk_profile_t p, const char* fixture, char* id, size_t n) {
    char args[1024];
    t_capture_t cap;
    script_presenter("extension_installs", "install");
    unpacked(fixture, args, sizeof args);
    invoke_ok(p, "extensions.install", args, &cap);
    REQUIRE(t_json_string(cap.json, "id", id, n));
}

static void start_with_extensions(const char* config) {
    bdk_harness_engine_extensions = config;
    t_start();
}

/* ---- packages made at test time ---- */

static char g_work[64];

static const char* work_dir(void) {
    if (!g_work[0]) {
        snprintf(g_work, sizeof g_work, "/tmp/bdk-ext-XXXXXX");
        REQUIRE(mkdtemp(g_work) != NULL);
    }
    return g_work;
}

/* Packs fixture `dir` into <work>/<crx> signed with <work>/key.pem (made on first use); returns
   the extension's id. */
static void pack(const char* dir, const char* crx, char* id, size_t n) {
    char args[1024], out[256];
    snprintf(args, sizeof args, "{\"dir\":\"%s/%s\",\"crx\":\"%s/%s\",\"key\":\"%s/key.pem\"}",
             BDK_EXTENSION_FIXTURES, dir, work_dir(), crx, work_dir());
    control("extensions.pack", args, out, sizeof out);
    REQUIRE(t_json_string(out, "id", id, n));
}

static void serve_file(const char* url, const char* crx) {
    char args[1024];
    snprintf(args, sizeof args, "{\"url\":\"%s\",\"file\":\"%s/%s\"}", url, work_dir(), crx);
    control("extensions.serve", args, NULL, 0);
}

/* An update manifest that offers `version` of `id` at `codebase`. */
static void serve_update_manifest(const char* url, const char* id, const char* codebase,
                                  const char* version) {
    char body[1024], args[2048];
    snprintf(body, sizeof body,
             "<?xml version='1.0' encoding='UTF-8'?>"
             "<gupdate xmlns='http://www.google.com/update2/response' protocol='2.0'>"
             "<app appid='%s'><updatecheck codebase='%s' version='%s'/></app></gupdate>",
             id, codebase, version);
    snprintf(args, sizeof args, "{\"url\":\"%s\",\"body\":\"%s\"}", url, body);
    control("extensions.serve", args, NULL, 0);
}

#define UPDATE_URL "https://updates.example/extensions.xml"

/* ---- install prompts and gestures ---- */

static void test_engine_ext_a_handler_grants_an_unpacked_install_with_a_gesture(void) {
    memset(&g_seen, 0, sizeof g_seen);
    t_start();
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_EXTENSION_INSTALLS, hold, &g_seen, NULL,
                               &err) == BDK_OK);
    t_freeze();
    bdk_profile_t p = t_new_profile("Install granted");
    bdk_contents_t c = new_active_page(p);
    event_log_t installed;
    subscribe_to("extensions.installed", p, &installed);

    char args[1024];
    t_capture_t cap;
    unpacked("install-basic", args, sizeof args);
    start(p, "extensions.install", args, &cap);
    wait_for_request();
    CHECK(strstr(g_seen.payload, "\"kind\":\"install\"") != NULL);
    CHECK(strstr(g_seen.payload, "\"extensionName\":\"Install Basic\"") != NULL);
    CHECK(strstr(g_seen.payload, "\"source\":\"unpacked\"") != NULL);
    CHECK(strstr(g_seen.payload, "\"warnings\":[") != NULL);
    CHECK(strstr(g_seen.payload, "\"verdict\":\"compatible\"") != NULL);
    /* Host warnings are lines, never Chromium's empty per-permission details. */
    const char* hosts = strstr(g_seen.payload, "\"hostWarnings\":[");
    REQUIRE(hosts != NULL);
    const char* hosts_end = strchr(hosts, ']');
    REQUIRE(hosts_end != NULL);
    const char* empty = strstr(hosts, "\"\"");
    CHECK(empty == NULL || empty > hosts_end);

    /* The handle alone is not a grant. */
    bdk_native_handle_t none;
    memset(&none, 0, sizeof none);
    err = 0;
    CHECK(bdk_ui_complete(g_seen.request, S("grant"), S("{}"), none, &err) != BDK_OK);
    CHECK(bdk_ui_complete(g_seen.request, S("grant"), S("{}"), gesture(c, 0), &err) == BDK_OK);
    finish(&cap);
    REQUIRE(!cap.has_error);
    char id[64];
    REQUIRE(t_json_string(cap.json, "id", id, sizeof id));
    CHECK(strstr(cap.json, "\"name\":\"Install Basic\"") != NULL);
    CHECK(strstr(cap.json, "\"version\":\"1.0\"") != NULL);
    CHECK(strstr(cap.json, "\"manifestVersion\":3") != NULL);
    CHECK(strstr(cap.json, "\"source\":\"unpacked\"") != NULL);
    CHECK(strstr(cap.json, "\"isEnabled\":true") != NULL);
    CHECK(strstr(cap.json, "\"disableReasons\":[]") != NULL);
    CHECK(strstr(cap.json, "\"apiPermissions\":[") != NULL && strstr(cap.json, "storage") != NULL);
    CHECK(strstr(cap.json, "\"verdict\":\"compatible\"") != NULL); /* keys are sorted */
    CHECK(wait_for_payload(&installed, "\"name\":\"Install Basic\""));
    bdk_ui_request_release(g_seen.request);

    t_capture_t list;
    invoke_ok(p, "extensions.list", "{}", &list);
    CHECK(strstr(list.json, id) != NULL);
    char get_args[128];
    snprintf(get_args, sizeof get_args, "{\"id\":\"%s\"}", id);
    invoke_ok(p, "extensions.get", get_args, &list);
    CHECK(strstr(list.json, "\"errors\":[") != NULL);
    close_window();
}

static void test_engine_ext_a_grant_without_a_recent_gesture_installs_nothing(void) {
    memset(&g_seen, 0, sizeof g_seen);
    t_start();
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_EXTENSION_INSTALLS, hold, &g_seen, NULL,
                               &err) == BDK_OK);
    t_freeze();
    bdk_profile_t p = t_new_profile("Install stale gesture");
    bdk_contents_t c = new_active_page(p);
    char args[1024];
    t_capture_t cap;
    unpacked("install-basic", args, sizeof args);
    start(p, "extensions.install", args, &cap);
    wait_for_request();
    /* A mouse-up a minute old is not recent: the engine turns the grant into a rejection. */
    err = 0;
    (void)bdk_ui_complete(g_seen.request, S("grant"), S("{}"), gesture(c, 2), &err);
    finish(&cap);
    CHECK(failed_with(&cap, "cancelled"));
    bdk_ui_request_release(g_seen.request);
    t_capture_t list;
    invoke_ok(p, "extensions.list", "{}", &list);
    CHECK(strcmp(list.json, "[]") == 0);
    close_window();
}

static void test_engine_ext_denying_the_install_cancels_it(void) {
    memset(&g_seen, 0, sizeof g_seen);
    t_start();
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_EXTENSION_INSTALLS, hold, &g_seen, NULL,
                               &err) == BDK_OK);
    t_freeze();
    bdk_profile_t p = t_new_profile("Install denied");
    bdk_contents_t c = new_active_page(p);
    char args[1024];
    t_capture_t cap;
    unpacked("install-basic", args, sizeof args);
    start(p, "extensions.install", args, &cap);
    wait_for_request();
    err = 0;
    CHECK(bdk_ui_complete(g_seen.request, S("deny"), S("{}"), gesture(c, 0), &err) == BDK_OK);
    finish(&cap);
    CHECK(failed_with(&cap, "cancelled"));
    bdk_ui_request_release(g_seen.request);
    t_capture_t list;
    invoke_ok(p, "extensions.list", "{}", &list);
    CHECK(strcmp(list.json, "[]") == 0);
    close_window();
}

static void test_engine_ext_with_no_handler_the_default_alert_lists_the_warnings(void) {
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Install default alert");
    bdk_contents_t c = new_active_page(p);
    (void)c;
    script_presenter("extension_installs", "cancel");
    char args[1024];
    t_capture_t cap;
    unpacked("install-basic", args, sizeof args);
    invoke(p, "extensions.install", args, &cap);
    CHECK(failed_with(&cap, "cancelled"));
    CHECK(presenter_uses("extension_installs") == 1);
    char record[65536];
    presenter_record("extension_installs", record, sizeof record);
    CHECK(strstr(record, "Install Basic") != NULL);
    CHECK(strstr(record, "It can:") != NULL || strstr(record, "no special permissions") != NULL);
    /* The payload is a JSON string inside the record, so its quotes are escaped. */
    CHECK(strstr(record, "\\\"install\\\"") != NULL && strstr(record, "\\\"cancel\\\"") != NULL);

    script_presenter("extension_installs", "install");
    invoke_ok(p, "extensions.install", args, &cap);
    CHECK(strstr(cap.json, "\"isEnabled\":true") != NULL);
    close_window();
}

static void test_engine_ext_an_install_with_no_page_to_ask_in_is_refused(void) {
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Install no page");
    script_presenter("extension_installs", "install");
    char args[1024];
    t_capture_t cap;
    unpacked("install-basic", args, sizeof args);
    invoke(p, "extensions.install", args, &cap);
    CHECK(failed_with(&cap, "cancelled")); /* never installed silently */
    CHECK(presenter_uses("extension_installs") == 0);
}

/* ---- the source policy ---- */

static void refused_source(const char* config, const char* source, const char* key) {
    start_with_extensions(config);
    t_freeze();
    bdk_profile_t p = t_new_profile("Source refused");
    t_capture_t cap;
    char args[1024];
    snprintf(args, sizeof args, "{\"source\":%s}", source);
    invoke(p, "extensions.install", args, &cap);
    CHECK(failed_with(&cap, "sourceNotAllowed"));
    CHECK(strstr(cap.message, key) != NULL);
}

static void test_engine_ext_unpacked_off_is_refused_naming_the_key(void) {
    char source[512];
    snprintf(source, sizeof source, "{\"kind\":\"unpacked\",\"path\":\"%s/install-basic\"}",
             BDK_EXTENSION_FIXTURES);
    refused_source("{\"install\":{\"unpacked\":false}}", source, "unpacked");
}

static void test_engine_ext_crx_off_is_refused_naming_the_key(void) {
    refused_source("{\"install\":{\"crx\":false}}", "{\"kind\":\"crx\",\"path\":\"/tmp/none.crx\"}",
                   "crx");
}

static void test_engine_ext_an_update_url_outside_the_patterns_is_refused(void) {
    refused_source("{\"install\":{\"update_urls\":[\"https://allowed.example/*\"]}}",
                   "{\"kind\":\"updateUrl\",\"id\":\"abcdefghijklmnopabcdefghijklmnop\","
                   "\"updateUrl\":\"https://other.example/u.xml\"}",
                   "update_urls");
}

static void test_engine_ext_a_store_that_is_not_configured_is_refused(void) {
    refused_source("{}",
                   "{\"kind\":\"store\",\"store\":\"chrome-web-store\","
                   "\"id\":\"abcdefghijklmnopabcdefghijklmnop\"}",
                   "stores");
}

/* ---- packages ---- */

static void test_engine_ext_a_crx_built_at_test_time_installs_with_the_prompt(void) {
    t_start_frozen();
    bdk_profile_t p = t_new_profile("CRX install");
    bdk_contents_t c = new_active_page(p);
    (void)c;
    char id[64];
    pack("install-basic", "basic.crx", id, sizeof id);
    script_presenter("extension_installs", "install");
    char args[1024];
    t_capture_t cap;
    snprintf(args, sizeof args, "{\"source\":{\"kind\":\"crx\",\"path\":\"%s/basic.crx\"}}",
             work_dir());
    invoke_ok(p, "extensions.install", args, &cap);
    CHECK(strstr(cap.json, id) != NULL);
    CHECK(strstr(cap.json, "\"source\":\"crx\"") != NULL);
    CHECK(strstr(cap.json, "\"isEnabled\":true") != NULL);
    CHECK(presenter_uses("extension_installs") == 1); /* the prompt stays for every install */
    close_window();
}

static void test_engine_ext_a_self_hosted_update_url_installs_and_then_updates(void) {
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Update URL");
    bdk_contents_t c = new_active_page(p);
    (void)c;
    char id[64], id2[64];
    pack("install-update-v1", "u1.crx", id, sizeof id);
    pack("install-update-v2", "u2.crx", id2, sizeof id2);
    CHECK(strcmp(id, id2) == 0); /* the same key: an update of the same extension */
    serve_file("https://updates.example/u1.crx", "u1.crx");
    serve_file("https://updates.example/u2.crx", "u2.crx");
    serve_update_manifest(UPDATE_URL, id, "https://updates.example/u1.crx", "1.0");
    event_log_t updated;
    subscribe_to("extensions.updated", p, &updated);

    script_presenter("extension_installs", "install");
    char args[1024];
    t_capture_t cap;
    snprintf(args, sizeof args,
             "{\"source\":{\"kind\":\"updateUrl\",\"id\":\"%s\",\"updateUrl\":\"" UPDATE_URL "\"}}",
             id);
    invoke_ok(p, "extensions.install", args, &cap);
    CHECK(strstr(cap.json, "\"version\":\"1.0\"") != NULL);
    CHECK(strstr(cap.json, "\"source\":\"updateUrl\"") != NULL);

    serve_update_manifest(UPDATE_URL, id, "https://updates.example/u2.crx", "2.0");
    invoke_ok(p, "extensions.checkForUpdates", "{}", &cap);
    CHECK(strstr(cap.json, id) != NULL && strstr(cap.json, "\"refused\":[]") != NULL);
    char get_args[128];
    snprintf(get_args, sizeof get_args, "{\"id\":\"%s\"}", id);
    invoke_ok(p, "extensions.get", get_args, &cap);
    CHECK(strstr(cap.json, "\"version\":\"2.0\"") != NULL);
    CHECK(strstr(cap.json, "\"isEnabled\":true") != NULL);
    CHECK(wait_for_payload(&updated, "\"previousVersion\":\"1.0\""));
    close_window();
}

/* Installs update-v1 from the update URL, leaving the manifest ready to offer `crx`. */
static void install_v1_from_update_url(bdk_profile_t p, char* id, size_t n) {
    char other[64];
    pack("install-update-v1", "u1.crx", id, n);
    pack("install-update-bad", "ubad.crx", other, sizeof other);
    pack("install-update-perm", "uperm.crx", other, sizeof other);
    serve_file("https://updates.example/u1.crx", "u1.crx");
    serve_file("https://updates.example/ubad.crx", "ubad.crx");
    serve_file("https://updates.example/uperm.crx", "uperm.crx");
    serve_update_manifest(UPDATE_URL, id, "https://updates.example/u1.crx", "1.0");
    script_presenter("extension_installs", "install");
    char args[1024];
    t_capture_t cap;
    snprintf(args, sizeof args,
             "{\"source\":{\"kind\":\"updateUrl\",\"id\":\"%s\",\"updateUrl\":\"" UPDATE_URL "\"}}",
             id);
    invoke_ok(p, "extensions.install", args, &cap);
}

static void test_engine_ext_an_update_that_becomes_incompatible_is_refused(void) {
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Update refused");
    bdk_contents_t c = new_active_page(p);
    (void)c;
    char id[64];
    install_v1_from_update_url(p, id, sizeof id);
    serve_update_manifest(UPDATE_URL, id, "https://updates.example/ubad.crx", "3.0");
    t_capture_t cap;
    invoke_ok(p, "extensions.checkForUpdates", "{}", &cap);
    CHECK(strstr(cap.json, "\"updated\":[]") != NULL);
    CHECK(strstr(cap.json, "api.loginState.unavailable") != NULL);
    /* The installed version keeps running. */
    char get_args[128];
    snprintf(get_args, sizeof get_args, "{\"id\":\"%s\"}", id);
    invoke_ok(p, "extensions.get", get_args, &cap);
    CHECK(strstr(cap.json, "\"version\":\"1.0\"") != NULL);
    CHECK(strstr(cap.json, "\"isEnabled\":true") != NULL);
    close_window();
}

static void test_engine_ext_an_update_that_adds_permissions_is_disabled_until_approved(void) {
    memset(&g_seen, 0, sizeof g_seen);
    t_start();
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_EXTENSION_INSTALLS, hold, &g_seen, NULL,
                               &err) == BDK_OK);
    t_freeze();
    bdk_profile_t p = t_new_profile("Update permissions");
    bdk_contents_t c = new_active_page(p);
    char id[64], other[64];
    pack("install-update-v1", "u1.crx", id, sizeof id);
    pack("install-update-perm", "uperm.crx", other, sizeof other);
    serve_file("https://updates.example/u1.crx", "u1.crx");
    serve_file("https://updates.example/uperm.crx", "uperm.crx");
    serve_update_manifest(UPDATE_URL, id, "https://updates.example/u1.crx", "1.0");
    char args[1024];
    t_capture_t cap;
    snprintf(args, sizeof args,
             "{\"source\":{\"kind\":\"updateUrl\",\"id\":\"%s\",\"updateUrl\":\"" UPDATE_URL "\"}}",
             id);
    start(p, "extensions.install", args, &cap);
    wait_for_request();
    CHECK(bdk_ui_complete(g_seen.request, S("grant"), S("{}"), gesture(c, 0), &err) == BDK_OK);
    finish(&cap);
    REQUIRE(!cap.has_error);
    bdk_ui_request_release(g_seen.request);

    serve_update_manifest(UPDATE_URL, id, "https://updates.example/uperm.crx", "2.0");
    invoke_ok(p, "extensions.checkForUpdates", "{}", &cap);
    CHECK(strstr(cap.json, id) != NULL);
    char get_args[128];
    snprintf(get_args, sizeof get_args, "{\"id\":\"%s\"}", id);
    invoke_ok(p, "extensions.get", get_args, &cap);
    CHECK(strstr(cap.json, "\"version\":\"2.0\"") != NULL);
    CHECK(strstr(cap.json, "\"isEnabled\":false") != NULL);
    CHECK(strstr(cap.json, "permissionsIncreased") != NULL);

    /* Enabling it asks again, with the new permissions. */
    memset(&g_seen, 0, sizeof g_seen);
    char enable[160];
    snprintf(enable, sizeof enable, "{\"id\":\"%s\",\"enabled\":true}", id);
    start(p, "extensions.setEnabled", enable, &cap);
    wait_for_request();
    CHECK(strstr(g_seen.payload, "\"kind\":\"permissionsIncrease\"") != NULL);
    CHECK(bdk_ui_complete(g_seen.request, S("grant"), S("{}"), gesture(c, 0), &err) == BDK_OK);
    finish(&cap);
    CHECK(!cap.has_error);
    bdk_ui_request_release(g_seen.request);
    invoke_ok(p, "extensions.get", get_args, &cap);
    CHECK(strstr(cap.json, "\"isEnabled\":true") != NULL);
    close_window();
}

/* ---- compatibility ---- */

static void test_engine_ext_an_incompatible_manifest_is_refused_with_the_finding(void) {
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Incompatible");
    bdk_contents_t c = new_active_page(p);
    (void)c;
    script_presenter("extension_installs", "install");
    char args[1024];
    t_capture_t cap;
    unpacked("install-mv2", args, sizeof args);
    invoke(p, "extensions.install", args, &cap);
    CHECK(failed_with(&cap, "incompatible"));
    CHECK(strstr(cap.message, "manifest.version.unsupported") != NULL);
    CHECK(presenter_uses("extension_installs") == 0); /* refused before anyone is asked */

    /* extensions.inspect reports it without installing. */
    char inspect[1024];
    snprintf(inspect, sizeof inspect,
             "{\"source\":{\"kind\":\"unpacked\",\"path\":\"%s/install-mv2\"}}",
             BDK_EXTENSION_FIXTURES);
    invoke_ok(p, "extensions.inspect", inspect, &cap);
    CHECK(strstr(cap.json, "\"verdict\":\"incompatible\"") != NULL);
    CHECK(strstr(cap.json, "manifest.version.unsupported") != NULL);
    close_window();
}

static void test_engine_ext_allow_all_installs_it_and_the_disabled_apis_are_absent(void) {
    start_with_extensions("{\"install_policy\":\"allow_all\","
                          "\"ui\":{\"side_panels\":\"disabled\",\"context_menus\":\"disabled\"}}");
    t_freeze();
    bdk_profile_t p = t_new_profile("Allow all");
    bdk_contents_t c = new_active_page(p);
    char id[64];
    install_fixture(p, "install-sidepanel", id, sizeof id);
    char record[65536];
    presenter_record("extension_installs", record, sizeof record);
    CHECK(strstr(record, "Compatibility") != NULL); /* the finding is in the prompt */

    /* The extension's own page sees which APIs exist: neither is on its chrome object. */
    char url[128];
    snprintf(url, sizeof url, "chrome-extension://%s/probe.html", id);
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_load(c, S(url), &err) == BDK_OK);
    char title[128] = "";
    for (int i = 0; i < 100 && strcmp(title, "undefined,undefined") != 0; ++i) {
        pump_for(0.1);
        bdk_contents_state_t st;
        memset(&st, 0, sizeof st);
        st.struct_size = sizeof st;
        REQUIRE(bdk_contents_state(c, &st, &err) == BDK_OK);
        snprintf(title, sizeof title, "%.*s", (int)st.title.length,
                 st.title.data ? st.title.data : "");
    }
    CHECK(strcmp(title, "undefined,undefined") == 0);

    t_capture_t cap;
    char get_args[128];
    snprintf(get_args, sizeof get_args, "{\"id\":\"%s\"}", id);
    invoke_ok(p, "extensions.get", get_args, &cap);
    CHECK(strstr(cap.json, "\"kind\":\"unavailableApi\"") != NULL);
    CHECK(strstr(cap.json, "\"verdict\":\"incompatible\"") != NULL);
    close_window();
}

/* ---- uninstall, private profiles, the store ---- */

static void test_engine_ext_uninstall_raises_the_uninstall_request(void) {
    memset(&g_seen, 0, sizeof g_seen);
    t_start();
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_EXTENSION_UNINSTALLS, hold, &g_seen, NULL,
                               &err) == BDK_OK);
    t_freeze();
    bdk_profile_t p = t_new_profile("Uninstall");
    bdk_contents_t c = new_active_page(p);
    char id[64];
    install_fixture(p, "install-basic", id, sizeof id);
    event_log_t removed;
    subscribe_to("extensions.uninstalled", p, &removed);

    char args[128];
    t_capture_t cap;
    snprintf(args, sizeof args, "{\"id\":\"%s\"}", id);
    start(p, "extensions.uninstall", args, &cap);
    wait_for_request();
    CHECK(strstr(g_seen.payload, "\"extensionName\":\"Install Basic\"") != NULL);
    CHECK(strstr(g_seen.payload, "\"requestedBy\":{\"kind\":\"user\"}") != NULL);
    CHECK(bdk_ui_complete(g_seen.request, S("grant"), S("{}"), gesture(c, 0), &err) == BDK_OK);
    finish(&cap);
    CHECK(!cap.has_error);
    bdk_ui_request_release(g_seen.request);
    CHECK(wait_for_payload(&removed, id));
    t_capture_t list;
    invoke_ok(p, "extensions.list", "{}", &list);
    CHECK(strcmp(list.json, "[]") == 0);
    close_window();
}

static void test_engine_ext_allowed_in_private_is_refused_when_private_is_never(void) {
    start_with_extensions("{\"allow_in_private\":\"never\"}");
    t_freeze();
    bdk_profile_t p = t_new_profile("Private never");
    bdk_contents_t c = new_active_page(p);
    (void)c;
    char id[64];
    install_fixture(p, "install-basic", id, sizeof id);
    char args[160];
    t_capture_t cap;
    snprintf(args, sizeof args, "{\"id\":\"%s\",\"allowed\":true}", id);
    invoke(p, "extensions.setAllowedInPrivate", args, &cap);
    CHECK(failed_with(&cap, "privateNotAllowed"));
    snprintf(args, sizeof args, "{\"id\":\"%s\"}", id);
    invoke_ok(p, "extensions.get", args, &cap);
    CHECK(strstr(cap.json, "\"allowedInPrivate\":false") != NULL);
    close_window();
}

#define STORE_URL "https://chromewebstore.google.com/detail/fixture"
#define STORE_PAGE                                                                                 \
    "<!doctype html><title>store</title><button id=b>Add to Chrome</button>"                       \
    "<script>setInterval(function(){document.title=document.getElementById('b').textContent},"     \
    "100)</script>"

static void script_html(const char* url, const char* html) {
    char args[2048];
    snprintf(args, sizeof args,
             "{\"url\":\"%s\",\"contentType\":\"text/html\",\"headers\":\"\",\"body\":\"", url);
    size_t n = strlen(args);
    for (const char* c = html; *c && n + 4 < sizeof args; ++c) {
        if (*c == '"' || *c == '\\')
            args[n++] = '\\';
        args[n++] = *c;
    }
    snprintf(args + n, sizeof args - n, "\"}");
    control("script_page", args, NULL, 0);
}

static int title_becomes(bdk_contents_t c, const char* want) {
    char title[128] = "";
    for (int i = 0; i < 60; ++i) {
        pump_for(0.1);
        bdk_contents_state_t st;
        memset(&st, 0, sizeof st);
        st.struct_size = sizeof st;
        bdk_error_t err = 0;
        REQUIRE(bdk_contents_state(c, &st, &err) == BDK_OK);
        snprintf(title, sizeof title, "%.*s", (int)st.title.length,
                 st.title.data ? st.title.data : "");
        if (strcmp(title, want) == 0)
            return 1;
    }
    printf("    title is \"%s\", not \"%s\"\n", title, want);
    return 0;
}

static void test_engine_ext_the_store_button_says_add_to_the_browser_name(void) {
    start_with_extensions("{\"install\":{\"stores\":[\"chrome-web-store\"]}}");
    t_freeze();
    bdk_profile_t p = t_new_profile("Store label");
    bdk_contents_t c = new_active_page(p);
    script_html(STORE_URL, STORE_PAGE);
    script_html("https://unrelated.example/", STORE_PAGE);
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_load(c, S(STORE_URL), &err) == BDK_OK);
    CHECK(title_becomes(c, "Add to BDK Conformance")); /* the browser name of the suite */
    /* Another origin with the same markup is left alone. */
    REQUIRE(bdk_contents_load(c, S("https://unrelated.example/"), &err) == BDK_OK);
    CHECK(title_becomes(c, "Add to Chrome"));
    close_window();
}

static void test_engine_ext_the_store_button_is_left_alone_when_the_store_is_off(void) {
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Store off");
    bdk_contents_t c = new_active_page(p);
    script_html(STORE_URL, STORE_PAGE);
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_load(c, S(STORE_URL), &err) == BDK_OK);
    CHECK(title_becomes(c, "Add to Chrome"));
    close_window();
}

const t_test_t t_engine_extension_tests[] = {
    {"engine_ext_install_a_handler_grants_an_unpacked_install_with_a_gesture",
     test_engine_ext_a_handler_grants_an_unpacked_install_with_a_gesture},
    {"engine_ext_install_a_grant_without_a_recent_gesture_installs_nothing",
     test_engine_ext_a_grant_without_a_recent_gesture_installs_nothing},
    {"engine_ext_install_denying_the_install_cancels_it",
     test_engine_ext_denying_the_install_cancels_it},
    {"engine_ext_install_with_no_handler_the_default_alert_lists_the_warnings",
     test_engine_ext_with_no_handler_the_default_alert_lists_the_warnings},
    {"engine_ext_install_an_install_with_no_page_to_ask_in_is_refused",
     test_engine_ext_an_install_with_no_page_to_ask_in_is_refused},
    {"engine_ext_install_unpacked_off_is_refused_naming_the_key",
     test_engine_ext_unpacked_off_is_refused_naming_the_key},
    {"engine_ext_install_crx_off_is_refused_naming_the_key",
     test_engine_ext_crx_off_is_refused_naming_the_key},
    {"engine_ext_install_an_update_url_outside_the_patterns_is_refused",
     test_engine_ext_an_update_url_outside_the_patterns_is_refused},
    {"engine_ext_install_a_store_that_is_not_configured_is_refused",
     test_engine_ext_a_store_that_is_not_configured_is_refused},
    {"engine_ext_install_a_crx_built_at_test_time_installs_with_the_prompt",
     test_engine_ext_a_crx_built_at_test_time_installs_with_the_prompt},
    {"engine_ext_install_a_self_hosted_update_url_installs_and_then_updates",
     test_engine_ext_a_self_hosted_update_url_installs_and_then_updates},
    {"engine_ext_install_an_update_that_becomes_incompatible_is_refused",
     test_engine_ext_an_update_that_becomes_incompatible_is_refused},
    {"engine_ext_install_an_update_that_adds_permissions_is_disabled_until_approved",
     test_engine_ext_an_update_that_adds_permissions_is_disabled_until_approved},
    {"engine_ext_install_an_incompatible_manifest_is_refused_with_the_finding",
     test_engine_ext_an_incompatible_manifest_is_refused_with_the_finding},
    {"engine_ext_install_allow_all_installs_it_and_the_disabled_apis_are_absent",
     test_engine_ext_allow_all_installs_it_and_the_disabled_apis_are_absent},
    {"engine_ext_install_uninstall_raises_the_uninstall_request",
     test_engine_ext_uninstall_raises_the_uninstall_request},
    {"engine_ext_install_allowed_in_private_is_refused_when_private_is_never",
     test_engine_ext_allowed_in_private_is_refused_when_private_is_never},
    {"engine_ext_install_the_store_button_says_add_to_the_browser_name",
     test_engine_ext_the_store_button_says_add_to_the_browser_name},
    {"engine_ext_install_the_store_button_is_left_alone_when_the_store_is_off",
     test_engine_ext_the_store_button_is_left_alone_when_the_store_is_off},
    {NULL, NULL},
};
