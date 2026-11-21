/* Extensions and registered windows (M5): the rules of extensions.md and conventions.md
 * "Extensions (M5)" that the ABI can express. The configuration of the extension surfaces, the
 * gesture and surface checks of bdk_extensions.h, open requests (bdk_ui_update) and the window
 * calls run on every runtime. The install, update and action flows, which need packages and
 * windows a test can make up, run where the runtime has the stub's hooks (harness_extensions.h);
 * the engine runs its own scenarios against real extensions (tests_engine_ext*.c). */
#include <stdlib.h>

#include "harness_extensions.h"
#include "test_support.h"

#define ID_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define ID_B "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"

/* ---- helpers ---- */

static bdk_request_t g_request;
static uint32_t g_surface;
static int g_calls, g_withdrawn, g_updates;
static char g_payload[1024], g_update[256];
static bdk_ui_response_t g_answer = BDK_UI_HANDLED;

static void on_update(void* ctx, bdk_request_t request, bdk_string_t update) {
    (void)ctx, (void)request;
    ++g_updates;
    snprintf(g_update, sizeof g_update, "%.*s", (int)update.length, update.data);
}

static void on_withdrawn(void* ctx) {
    (void)ctx;
    ++g_withdrawn;
}

static bdk_ui_response_t handler(void* ctx, const bdk_ui_request_info_t* info) {
    (void)ctx;
    ++g_calls;
    g_request = info->request;
    g_surface = info->surface;
    snprintf(g_payload, sizeof g_payload, "%.*s", (int)info->payload_json.length,
             info->payload_json.data);
    bdk_ui_set_update_handler(info->request, on_update, NULL, NULL);
    bdk_ui_set_withdrawn_handler(info->request, on_withdrawn, NULL, NULL);
    return g_answer;
}

static void reset(void) {
    g_request = 0;
    g_surface = 0;
    g_calls = g_withdrawn = g_updates = 0;
    g_payload[0] = g_update[0] = '\0';
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

/* A fake native window, for the stub only: the engine would dereference it. */
static bdk_native_handle_t fake_window(uint64_t id) {
    bdk_native_handle_t w;
    memset(&w, 0, sizeof w);
    w.kind = BDK_NATIVE_NSWINDOW;
    w.pointer = (void*)(uintptr_t)(0x1000 + id);
    w.id = id;
    return w;
}

/* Starts with the harness's resolved configuration plus the given members (JSON text without
   braces, may be empty). */
static bdk_status_t start_with(const char* members, bdk_error_t* err) {
    bdk_start_config_t c;
    t_fill_start_config(&c);
    const bdk_string_t base = c.resolved_config_json;
    REQUIRE(base.length > 1 && base.data[base.length - 1] == '}');
    static char resolved[4096];
    snprintf(resolved, sizeof resolved, "%.*s%s%s}", (int)(base.length - 1), base.data,
             base.length > 2 && members[0] ? "," : "", members);
    c.resolved_config_json = S(resolved);
    return bdk_runtime_start(&c, &t_runtime, err);
}

static void start_frozen_with(const char* members, const uint32_t* surfaces, size_t n) {
    reset();
    bdk_error_t err = 0;
    REQUIRE(start_with(members, &err) == BDK_OK);
    for (size_t i = 0; i < n; ++i)
        REQUIRE(bdk_ui_set_handler(t_runtime, surfaces[i], handler, NULL, NULL, &err) == BDK_OK);
    t_freeze();
}

static const uint32_t kInstallSurfaces[] = {
    BDK_SURFACE_EXTENSION_INSTALLS, BDK_SURFACE_EXTENSION_UNINSTALLS, BDK_SURFACE_EXTENSION_POPUPS};

static bdk_call_t invoke(bdk_profile_t profile, const char* command, const char* args,
                         t_capture_t* cap) {
    t_cap_init(cap);
    bdk_invoke_options_t o;
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.profile = profile;
    return bdk_invoke_named(t_runtime, S(command), S(args), &o, t_on_result, t_on_progress, cap,
                            t_on_release);
}

/* Invokes and pumps; the result is in cap. */
static void call(bdk_profile_t profile, const char* command, const char* args, t_capture_t* cap) {
    REQUIRE(invoke(profile, command, args, cap) != 0);
    t_pump();
    REQUIRE(cap->results == 1);
}

static int failed_with(const t_capture_t* cap, const char* domain, const char* code) {
    return cap->has_error && strcmp(cap->domain, domain) == 0 && strcmp(cap->code, code) == 0;
}

/* Events by name, counted with the last payload. */
typedef struct named_events {
    int count;
    char last_name[64], last[2048];
} named_events_t;

static void on_event(void* ctx, uint32_t id, bdk_string_t name, bdk_string_t payload) {
    named_events_t* e = (named_events_t*)ctx;
    (void)id;
    ++e->count;
    snprintf(e->last_name, sizeof e->last_name, "%.*s", (int)name.length, name.data);
    snprintf(e->last, sizeof e->last, "%.*s", (int)payload.length, payload.data);
}

static void watch(const char* name, named_events_t* e) {
    memset(e, 0, sizeof *e);
    REQUIRE(bdk_subscribe_named(t_runtime, S(name), S(""), NULL, on_event, e, NULL) != 0);
}

static const char* kPlainManifest =
    "{\"manifest_version\":3,\"name\":\"Plain\",\"version\":\"1.0\",\"description\":\"d\","
    "\"permissions\":[\"storage\"],\"host_permissions\":[\"https://example.com/*\"]}";

static const char* kPopupManifest =
    "{\"manifest_version\":3,\"name\":\"Popup\",\"version\":\"1.0\","
    "\"action\":{\"default_title\":\"Open\",\"default_popup\":\"popup.html\"},"
    "\"commands\":{\"_execute_action\":{\"suggested_key\":{\"default\":\"Ctrl+Shift+Y\"}},"
    "\"toggle\":{\"description\":\"Toggle\",\"suggested_key\":{\"mac\":\"Command+Shift+U\"}}}}";

static const char* kClickManifest = "{\"manifest_version\":3,\"name\":\"Click\",\"version\":\"1."
                                    "0\",\"permissions\":[\"activeTab\"],"
                                    "\"action\":{\"default_title\":\"Run\"}}";

static const char* kSidePanelManifest = "{\"manifest_version\":3,\"name\":\"Panel\",\"version\":"
                                        "\"1.0\",\"permissions\":[\"sidePanel\"],"
                                        "\"side_panel\":{\"default_path\":\"panel.html\"}}";

/* Installs a package registered under the unpacked key, granting the prompt with a gesture. */
static void install_unpacked(bdk_profile_t p, const char* path, const char* manifest,
                             t_capture_t* cap) {
    REQUIRE(bdk_harness_extension_package(t_runtime, "unpacked", path, manifest) == BDK_OK);
    char args[256];
    snprintf(args, sizeof args, "{\"source\":{\"kind\":\"unpacked\",\"path\":\"%s\"}}", path);
    g_calls = 0;
    REQUIRE(invoke(p, "extensions.install", args, cap) != 0);
    t_pump();
    REQUIRE(g_calls == 1 && g_surface == BDK_SURFACE_EXTENSION_INSTALLS);
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_complete(g_request, S("grant"), S("{}"), gesture(), &err) == BDK_OK);
    t_pump();
    REQUIRE(cap->results == 1 && !cap->has_error);
}

/* ---- Configuration of the extension surfaces (every runtime) ---- */

static void test_ext_custom_popups_need_a_handler_at_the_freeze(void) {
    bdk_error_t err = 0;
    REQUIRE(start_with("\"extensions\":{\"ui\":{\"popups\":\"custom\",\"actions\":\"custom\"}}",
                       &err) == BDK_OK);
    CHECK(bdk_runtime_freeze_registrations(t_runtime, &err) == BDK_ERR_FAILED);
    bdk_string_t m = bdk_error_message(err);
    CHECK(str_has(m, "extension_popups"));
    CHECK(!str_has(m, "extension_actions")); /* actions are data: no handler needed */
    CHECK(t_error_is(err, "config", "handlerMissing"));
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_EXTENSION_POPUPS, handler, NULL, NULL,
                               &err) == BDK_OK);
    CHECK(bdk_runtime_freeze_registrations(t_runtime, &err) == BDK_OK);
}

static void test_ext_unknown_extension_surface_fails_start(void) {
    bdk_error_t err = 0;
    CHECK(start_with("\"extensions\":{\"ui\":{\"toolbar\":\"default\"}}", &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "ui", "surfaceUnknown"));
    /* The router's names for extension surfaces are not [ui.surfaces] keys. */
    CHECK(start_with("\"surfaces\":{\"extension_popups\":\"default\"}", &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "ui", "surfaceUnknown"));
    CHECK(start_with("\"extensions\":{\"ui\":{\"popups\":\"sometimes\"}}", &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "config", "resolvedInvalid"));
    CHECK(start_with("\"extensions\":{\"install\":{\"stores\":[\"elsewhere\"]}}", &err) ==
          BDK_ERR_FAILED);
    CHECK(t_error_is(err, "config", "resolvedInvalid"));
}

static void test_ext_handlers_accept_every_extension_surface(void) {
    t_start();
    bdk_error_t err = 0;
    for (uint32_t s = BDK_SURFACE_EXTENSION_ACTIONS; s <= BDK_SURFACE_EXTENSION_UNINSTALLS; ++s)
        CHECK(bdk_ui_set_handler(t_runtime, s, handler, NULL, NULL, &err) == BDK_OK);
    CHECK(bdk_ui_set_handler(t_runtime, BDK_SURFACE_EXTENSION_UNINSTALLS + 1, handler, NULL, NULL,
                             &err) == BDK_ERR_INVALID_ARGUMENT);
}

static void test_ext_disabled_extensions_disable_every_extension_surface(void) {
    reset();
    bdk_error_t err = 0;
    REQUIRE(start_with("\"extensions\":{\"enabled\":false}", &err) == BDK_OK);
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_EXTENSION_POPUPS, handler, NULL, NULL,
                               &err) == BDK_OK);
    t_freeze();
    bdk_contents_t c = t_new_contents(t_new_profile("Ext"));
    REQUIRE(bdk_harness_raise_ui_request(t_runtime, BDK_SURFACE_EXTENSION_POPUPS, c, "{}") ==
            BDK_OK);
    t_pump();
    CHECK(g_calls == 0); /* disabled: no handler is called */
    bdk_harness_ui_outcome_t o;
    memset(&o, 0, sizeof o);
    REQUIRE(bdk_harness_last_ui_outcome(t_runtime, &o) == BDK_OK);
    CHECK(o.kind == BDK_HARNESS_UI_DISABLED);
    if (!bdk_harness_extension_hooks())
        return;
    /* And nothing installs. */
    t_capture_t cap;
    call(t_new_profile("Other"), "extensions.install",
         "{\"source\":{\"kind\":\"unpacked\",\"path\":\"/x\"}}", &cap);
    CHECK(failed_with(&cap, "extension", "sourceNotAllowed"));
}

/* ---- Activation and the default menu: the checks before the backend (every runtime) ---- */

static void test_ext_activation_needs_a_gesture_and_an_extension(void) {
    t_start_frozen();
    bdk_contents_t c = t_new_contents(t_new_profile("Ext"));
    bdk_buffer_t out = 1;
    bdk_error_t err = 0;
    CHECK(bdk_extension_action_activate(c, S(ID_A), no_gesture(), &out, &err) == BDK_ERR_FAILED);
    CHECK(out == 0);
    CHECK(t_error_is(err, "ui", "gestureRequired"));
    CHECK(bdk_extension_action_activate(c, S(ID_A), gesture(), &out, &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "extension", "notFound"));
    CHECK(bdk_extension_command_perform(c, S(ID_A), S("toggle"), gesture(), &out, &err) ==
          BDK_ERR_FAILED);
    CHECK(t_error_is(err, "extension", "notFound"));
    CHECK(bdk_extension_action_activate(c, S(""), gesture(), &out, &err) ==
          BDK_ERR_INVALID_ARGUMENT);
    CHECK(bdk_extension_action_activate(c, S(ID_A), gesture(), NULL, &err) ==
          BDK_ERR_INVALID_ARGUMENT);
    CHECK(bdk_extension_command_perform(c, S(ID_A), S(""), gesture(), &out, &err) ==
          BDK_ERR_INVALID_ARGUMENT);
    REQUIRE(bdk_contents_close(c, &err) == BDK_OK);
    t_pump();
    CHECK(bdk_extension_action_activate(c, S(ID_A), gesture(), &out, &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "contents", "stale"));
    REQUIRE(bdk_contents_destroy(c) == BDK_OK);
    CHECK(bdk_extension_action_activate(c, S(ID_A), gesture(), &out, &err) == BDK_ERR_STALE_HANDLE);
}

static void test_ext_activation_refused_when_actions_are_disabled(void) {
    reset();
    bdk_error_t err = 0;
    REQUIRE(start_with("\"extensions\":{\"ui\":{\"actions\":\"disabled\"}}", &err) == BDK_OK);
    t_freeze();
    bdk_contents_t c = t_new_contents(t_new_profile("Ext"));
    bdk_buffer_t out = 0;
    CHECK(bdk_extension_action_activate(c, S(ID_A), gesture(), &out, &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "extension", "surfaceDisabled"));
}

static void test_ext_activation_is_refused_off_the_ui_thread_ctx(void* ctx) {
    bdk_contents_t c = *(bdk_contents_t*)ctx;
    bdk_buffer_t out = 0;
    bdk_error_t err = 0;
    CHECK(bdk_extension_action_activate(c, S(ID_A), gesture(), &out, &err) == BDK_ERR_WRONG_THREAD);
}

static void test_ext_activation_is_refused_off_the_ui_thread(void) {
    t_start_frozen();
    bdk_contents_t c = t_new_contents(t_new_profile("Ext"));
    bdk_harness_run_on_other_thread(test_ext_activation_is_refused_off_the_ui_thread_ctx, &c);
}

static void test_ext_window_kind_is_checked(void) {
    t_start();
    bdk_window_t w = 0;
    bdk_error_t err = 0;
    CHECK(bdk_window_register(t_runtime, fake_window(1), 7, &w, &err) == BDK_ERR_INVALID_ARGUMENT);
    CHECK(w == 0);
}

/* ---- Open requests (every runtime) ---- */

static void test_ext_open_request_updates_until_it_finishes(void) {
    const uint32_t surfaces[] = {BDK_SURFACE_EXTENSION_POPUPS};
    start_frozen_with("", surfaces, 1);
    bdk_contents_t c = t_new_contents(t_new_profile("Ext"));
    REQUIRE(bdk_harness_raise_ui_request(t_runtime, BDK_SURFACE_EXTENSION_POPUPS, c,
                                         "{\"extensionId\":\"" ID_A "\"}") == BDK_OK);
    t_pump();
    REQUIRE(g_calls == 1);
    bdk_error_t err = 0;
    CHECK(bdk_ui_update(g_request, S("{\"placed\":true}"), &err) == BDK_OK);
    CHECK(bdk_ui_update(g_request, S("[1]"), &err) == BDK_ERR_INVALID_ARGUMENT);
    CHECK(bdk_ui_update(g_request, S("{bad"), &err) == BDK_ERR_INVALID_ARGUMENT);
    t_pump();
    /* The application closes its container: the request ends. */
    CHECK(bdk_ui_complete(g_request, S("accept"), S("{\"action\":\"close\"}"), no_gesture(),
                          &err) == BDK_OK);
    CHECK(bdk_ui_update(g_request, S("{\"placed\":true}"), &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "ui", "requestCompleted"));
    t_pump();
    bdk_ui_request_release(g_request);
    CHECK(bdk_ui_update(g_request, S("{}"), &err) == BDK_ERR_STALE_HANDLE);
}

/* ---- With the stub's hooks: updates both ways, windows, installs and actions ---- */

#define NEEDS_HOOKS()                                                                              \
    do {                                                                                           \
        if (!bdk_harness_extension_hooks())                                                        \
            return;                                                                                \
    } while (0)

static void test_ext_open_request_updates_reach_both_sides(void) {
    NEEDS_HOOKS();
    const uint32_t surfaces[] = {BDK_SURFACE_EXTENSION_SIDE_PANELS};
    start_frozen_with("", surfaces, 1);
    bdk_contents_t c = t_new_contents(t_new_profile("Ext"));
    REQUIRE(bdk_harness_raise_ui_request(t_runtime, BDK_SURFACE_EXTENSION_SIDE_PANELS, c,
                                         "{\"extensionId\":\"" ID_A
                                         "\",\"scope\":\"page\"}") == BDK_OK);
    t_pump();
    REQUIRE(g_calls == 1);
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_update(g_request, S("{\"placed\":true,\"side\":\"leading\"}"), &err) == BDK_OK);
    char last[128] = "";
    REQUIRE(bdk_harness_last_ui_update(t_runtime, last, sizeof last) == BDK_OK);
    CHECK(last[0] == '\0'); /* never synchronous */
    t_pump();
    REQUIRE(bdk_harness_last_ui_update(t_runtime, last, sizeof last) == BDK_OK);
    CHECK(strstr(last, "\"side\":\"leading\"") != NULL);

    REQUIRE(bdk_harness_send_ui_update(t_runtime, "{\"visible\":false}") == BDK_OK);
    CHECK(g_updates == 0);
    t_pump();
    CHECK(g_updates == 1 && strcmp(g_update, "{\"visible\":false}") == 0);

    /* The extension closes its panel: a withdrawal, and no update after it. */
    REQUIRE(bdk_harness_withdraw_ui_requests(t_runtime) == BDK_OK);
    REQUIRE(bdk_harness_send_ui_update(t_runtime, "{\"visible\":true}") == BDK_OK);
    t_pump();
    CHECK(g_withdrawn == 1);
    CHECK(g_updates == 1);
}

static int g_anchor_calls, g_anchor_released, g_request_calls, g_request_released;
static bdk_ui_response_t g_window_answer;
static char g_window_request[256];

static uint8_t anchor(void* ctx, bdk_window_t w, uint32_t role, double rect[4]) {
    (void)ctx, (void)w;
    ++g_anchor_calls;
    if (role != BDK_ANCHOR_EXTENSIONS)
        return 0;
    rect[0] = 10, rect[1] = 20, rect[2] = 30, rect[3] = 40;
    return 1;
}
static void anchor_released(void* ctx) {
    (void)ctx;
    ++g_anchor_released;
}
static bdk_ui_response_t window_request(void* ctx, bdk_window_t w, bdk_string_t json) {
    (void)ctx, (void)w;
    ++g_request_calls;
    snprintf(g_window_request, sizeof g_window_request, "%.*s", (int)json.length, json.data);
    return g_window_answer;
}
static void request_released(void* ctx) {
    (void)ctx;
    ++g_request_released;
}

static void test_ext_windows_report_their_pages(void) {
    NEEDS_HOOKS();
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Ext");
    bdk_contents_t a = t_new_contents(p), b = t_new_contents(p);
    bdk_window_t w1 = 0, w2 = 0, found = 99;
    bdk_error_t err = 0;
    REQUIRE(bdk_window_register(t_runtime, fake_window(1), BDK_WINDOW_KIND_NORMAL, &w1, &err) ==
            BDK_OK);
    REQUIRE(bdk_window_register(t_runtime, fake_window(2), BDK_WINDOW_KIND_POPUP, &w2, &err) ==
            BDK_OK);
    int32_t id = 0;
    CHECK(bdk_window_extension_id(w1, &id, &err) == BDK_OK && id == -1);
    CHECK(bdk_window_for_contents(a, &found, &err) == BDK_OK && found == 0);

    REQUIRE(bdk_harness_attach_to_window(a, w1) == BDK_OK);
    REQUIRE(bdk_harness_attach_to_window(b, w1) == BDK_OK);
    CHECK(bdk_window_for_contents(b, &found, &err) == BDK_OK && found == w1);
    bdk_contents_t active = 0;
    CHECK(bdk_window_active_contents(w1, &active, &err) == BDK_OK && active == a);
    REQUIRE(bdk_contents_focus(b) == BDK_OK);
    CHECK(bdk_window_active_contents(w1, &active, &err) == BDK_OK && active == b);
    bdk_buffer_t list = 0;
    REQUIRE(bdk_window_contents(w1, &list, &err) == BDK_OK);
    char text[128], expected[128];
    t_buffer_text(list, text, sizeof text);
    snprintf(expected, sizeof expected, "[\"%llu\",\"%llu\"]", (unsigned long long)a,
             (unsigned long long)b);
    CHECK(strcmp(text, expected) == 0);
    CHECK(bdk_window_extension_id(w1, &id, &err) == BDK_OK && id == 1);

    /* A move keeps the page and changes its window. */
    REQUIRE(bdk_harness_attach_to_window(a, w2) == BDK_OK);
    CHECK(bdk_window_for_contents(a, &found, &err) == BDK_OK && found == w2);
    CHECK(bdk_window_extension_id(w2, &id, &err) == BDK_OK && id == 2);
    /* A destroyed page leaves its window. */
    bdk_contents_destroy(b);
    t_pump();
    REQUIRE(bdk_window_contents(w1, &list, &err) == BDK_OK);
    t_buffer_text(list, text, sizeof text);
    CHECK(strcmp(text, "[]") == 0);
    CHECK(bdk_window_active_contents(w1, &active, &err) == BDK_OK && active == 0);

    REQUIRE(bdk_window_unregister(w1, &err) == BDK_OK);
    CHECK(bdk_window_contents(w1, &list, &err) == BDK_ERR_STALE_HANDLE);
    CHECK(bdk_window_active_contents(w1, NULL, &err) == BDK_ERR_STALE_HANDLE);
    CHECK(bdk_window_active_contents(w2, NULL, &err) == BDK_ERR_INVALID_ARGUMENT);
}

static void test_ext_window_anchor_and_request_handlers(void) {
    NEEDS_HOOKS();
    t_start_frozen();
    g_anchor_calls = g_anchor_released = g_request_calls = g_request_released = 0;
    bdk_window_t w = 0;
    bdk_error_t err = 0;
    REQUIRE(bdk_window_register(t_runtime, fake_window(3), BDK_WINDOW_KIND_NORMAL, &w, &err) ==
            BDK_OK);
    double rect[4] = {0, 0, 0, 0};
    uint8_t found = 9;
    REQUIRE(bdk_harness_window_anchor(w, BDK_ANCHOR_EXTENSIONS, rect, &found) == BDK_OK);
    CHECK(found == 0); /* no provider */
    REQUIRE(bdk_window_set_anchor_provider(w, anchor, NULL, anchor_released, &err) == BDK_OK);
    REQUIRE(bdk_harness_window_anchor(w, BDK_ANCHOR_EXTENSIONS, rect, &found) == BDK_OK);
    CHECK(found == 1 && rect[0] == 10 && rect[3] == 40);
    REQUIRE(bdk_harness_window_anchor(w, BDK_ANCHOR_ADDRESS_BAR, rect, &found) == BDK_OK);
    CHECK(found == 0 && g_anchor_calls == 2);

    /* No handler: the default applies it. */
    uint32_t answer = 9;
    REQUIRE(bdk_harness_window_request(w, "{\"kind\":\"minimize\"}") == BDK_OK);
    REQUIRE(bdk_harness_last_window_answer(&answer) == BDK_OK);
    CHECK(answer == 0);
    t_pump();
    REQUIRE(bdk_harness_last_window_answer(&answer) == BDK_OK);
    CHECK(answer == BDK_UI_USE_DEFAULT);

    g_window_answer = BDK_UI_REJECT;
    REQUIRE(bdk_window_set_request_handler(w, window_request, NULL, request_released, &err) ==
            BDK_OK);
    REQUIRE(bdk_harness_window_request(
                w, "{\"kind\":\"setFrame\",\"frame\":{\"x\":1,\"y\":2,\"width\":3,\"height\":4},"
                   "\"initiator\":{\"extensionId\":\"" ID_A "\"}}") == BDK_OK);
    CHECK(g_request_calls == 0); /* later */
    t_pump();
    CHECK(g_request_calls == 1 && strstr(g_window_request, "setFrame") != NULL);
    REQUIRE(bdk_harness_last_window_answer(&answer) == BDK_OK);
    CHECK(answer == BDK_UI_REJECT);
    g_window_answer = (bdk_ui_response_t)42; /* not an answer: a rejection */
    REQUIRE(bdk_harness_window_request(w, "{\"kind\":\"close\"}") == BDK_OK);
    t_pump();
    REQUIRE(bdk_harness_last_window_answer(&answer) == BDK_OK);
    CHECK(answer == BDK_UI_REJECT);

    /* Replacing releases the old context; unregistering releases the rest. */
    REQUIRE(bdk_window_set_anchor_provider(w, anchor, NULL, anchor_released, &err) == BDK_OK);
    t_pump();
    CHECK(g_anchor_released == 1);
    REQUIRE(bdk_window_unregister(w, &err) == BDK_OK);
    t_pump();
    CHECK(g_anchor_released == 2 && g_request_released == 1);
}

static void test_ext_window_handlers_are_released_at_shutdown(void) {
    NEEDS_HOOKS();
    t_start_frozen();
    g_anchor_released = g_request_released = 0;
    bdk_window_t w = 0;
    bdk_error_t err = 0;
    REQUIRE(bdk_window_register(t_runtime, fake_window(4), BDK_WINDOW_KIND_POPUP, &w, &err) ==
            BDK_OK);
    REQUIRE(bdk_window_set_anchor_provider(w, anchor, NULL, anchor_released, &err) == BDK_OK);
    REQUIRE(bdk_window_set_request_handler(w, window_request, NULL, request_released, &err) ==
            BDK_OK);
    t_stop();
    CHECK(g_anchor_released == 1 && g_request_released == 1);
}

static void test_ext_default_menu_needs_default_actions(void) {
    NEEDS_HOOKS();
    reset();
    bdk_error_t err = 0;
    REQUIRE(start_with("\"extensions\":{\"ui\":{\"actions\":\"custom\"}}", &err) == BDK_OK);
    t_freeze();
    bdk_window_t w = 0;
    REQUIRE(bdk_window_register(t_runtime, fake_window(5), BDK_WINDOW_KIND_NORMAL, &w, &err) ==
            BDK_OK);
    CHECK(bdk_extension_menu_present(w, gesture(), &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "extension", "surfaceDisabled"));
    t_stop();

    t_start_frozen();
    REQUIRE(bdk_window_register(t_runtime, fake_window(6), BDK_WINDOW_KIND_NORMAL, &w, &err) ==
            BDK_OK);
    CHECK(bdk_extension_menu_present(w, no_gesture(), &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "ui", "gestureRequired"));
    CHECK(bdk_extension_menu_present(w, gesture(), &err) == BDK_OK);
    uint32_t shown = 0;
    REQUIRE(bdk_harness_menus_presented(&shown) == BDK_OK);
    CHECK(shown == 1);
}

static void test_ext_install_asks_and_needs_a_gesture(void) {
    NEEDS_HOOKS();
    start_frozen_with("", kInstallSurfaces, 3);
    bdk_profile_t p = t_new_profile("Ext");
    named_events_t installed, loaded;
    watch("extensions.installed", &installed);
    watch("extensions.loaded", &loaded);
    REQUIRE(bdk_harness_extension_package(t_runtime, "unpacked", "/fixtures/plain",
                                          kPlainManifest) == BDK_OK);
    t_capture_t cap;
    REQUIRE(invoke(p, "extensions.install",
                   "{\"source\":{\"kind\":\"unpacked\",\"path\":\"/fixtures/plain\"}}", &cap) != 0);
    t_pump();
    REQUIRE(g_calls == 1 && g_surface == BDK_SURFACE_EXTENSION_INSTALLS);
    CHECK(strstr(g_payload, "\"kind\":\"install\"") != NULL);
    CHECK(strstr(g_payload, "\"extensionName\":\"Plain\"") != NULL);
    CHECK(strstr(g_payload, "\"source\":\"unpacked\"") != NULL);
    CHECK(strstr(g_payload, "\"warnings\":[\"Uses storage\"]") != NULL);
    CHECK(strstr(g_payload, "\"hostWarnings\":[\"Read and change your data on "
                            "https://example.com/*\"]") != NULL);
    CHECK(strstr(g_payload, "\"verdict\":\"compatible\"") != NULL);
    CHECK(cap.results == 0); /* waiting for the user */

    bdk_error_t err = 0;
    CHECK(bdk_ui_complete(g_request, S("grant"), S("{}"), no_gesture(), &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "ui", "gestureRequired"));
    REQUIRE(bdk_ui_complete(g_request, S("grant"), S("{}"), gesture(), &err) == BDK_OK);
    t_pump();
    REQUIRE(cap.results == 1 && !cap.has_error);
    char id[40] = "";
    CHECK(t_json_string(cap.json, "id", id, sizeof id) && strlen(id) == 32);
    CHECK(strstr(cap.json, "\"source\":\"unpacked\"") != NULL);
    CHECK(strstr(cap.json, "\"siteAccess\":\"onSpecificSites\"") != NULL);
    CHECK(installed.count == 1 && strstr(installed.last, id) != NULL);
    CHECK(loaded.count == 1);

    t_capture_t list;
    call(p, "extensions.list", "{}", &list);
    CHECK(!list.has_error && strstr(list.json, id) != NULL);
    char args[96];
    snprintf(args, sizeof args, "{\"id\":\"%s\"}", id);
    t_capture_t got;
    call(p, "extensions.get", args, &got);
    CHECK(!got.has_error && strstr(got.json, "\"errors\":[]") != NULL);
    /* Profiles keep their own extensions. */
    t_capture_t other;
    call(t_new_profile("Other"), "extensions.list", "{}", &other);
    CHECK(!other.has_error && strcmp(other.json, "[]") == 0);
}

static void test_ext_install_without_confirmation_installs_nothing(void) {
    NEEDS_HOOKS();
    start_frozen_with("", kInstallSurfaces, 3);
    bdk_profile_t p = t_new_profile("Ext");
    REQUIRE(bdk_harness_extension_package(t_runtime, "unpacked", "/fixtures/plain",
                                          kPlainManifest) == BDK_OK);
    const char* args = "{\"source\":{\"kind\":\"unpacked\",\"path\":\"/fixtures/plain\"}}";
    bdk_error_t err = 0;
    t_capture_t cap;
    /* Denied. */
    REQUIRE(invoke(p, "extensions.install", args, &cap) != 0);
    t_pump();
    REQUIRE(bdk_ui_complete(g_request, S("deny"), S("{}"), no_gesture(), &err) == BDK_OK);
    t_pump();
    CHECK(failed_with(&cap, "extension", "cancelled"));
    /* Rejected by the handler. */
    g_answer = BDK_UI_REJECT;
    call(p, "extensions.install", args, &cap);
    CHECK(failed_with(&cap, "extension", "cancelled"));
    /* Default: the stub has no person to ask. */
    g_answer = BDK_UI_USE_DEFAULT;
    call(p, "extensions.install", args, &cap);
    CHECK(failed_with(&cap, "extension", "cancelled"));
    /* Dropped. */
    g_answer = BDK_UI_HANDLED;
    REQUIRE(invoke(p, "extensions.install", args, &cap) != 0);
    t_pump();
    bdk_ui_request_release(g_request);
    t_pump();
    CHECK(failed_with(&cap, "extension", "cancelled"));
    /* Cancelled by the caller while the prompt is up: the prompt is withdrawn. */
    bdk_call_t pending = invoke(p, "extensions.install", args, &cap);
    REQUIRE(pending != 0);
    t_pump();
    REQUIRE(g_calls >= 1);
    g_withdrawn = 0;
    REQUIRE(bdk_call_cancel(pending) == BDK_OK);
    t_pump();
    CHECK(failed_with(&cap, "broker", "cancelled"));
    CHECK(g_withdrawn == 1);

    t_capture_t list;
    call(p, "extensions.list", "{}", &list);
    CHECK(strcmp(list.json, "[]") == 0);
}

static void test_ext_sources_follow_the_configuration(void) {
    NEEDS_HOOKS();
    start_frozen_with("\"extensions\":{\"install\":{\"unpacked\":false,\"crx\":false,"
                      "\"update_urls\":[\"https://updates.example.com/*\"],\"stores\":[]}}",
                      kInstallSurfaces, 3);
    bdk_profile_t p = t_new_profile("Ext");
    t_capture_t cap;
    call(p, "extensions.install", "{\"source\":{\"kind\":\"unpacked\",\"path\":\"/x\"}}", &cap);
    CHECK(failed_with(&cap, "extension", "sourceNotAllowed"));
    CHECK(strstr(cap.message, "[extensions.install] unpacked") != NULL);
    call(p, "extensions.install", "{\"source\":{\"kind\":\"crx\",\"path\":\"/x.crx\"}}", &cap);
    CHECK(failed_with(&cap, "extension", "sourceNotAllowed"));
    CHECK(strstr(cap.message, "crx") != NULL);
    call(p, "extensions.install",
         "{\"source\":{\"kind\":\"updateUrl\",\"id\":\"" ID_A
         "\",\"updateUrl\":\"https://elsewhere.example.com/u.xml\"}}",
         &cap);
    CHECK(failed_with(&cap, "extension", "sourceNotAllowed"));
    CHECK(strstr(cap.message, "update_urls") != NULL);
    call(p, "extensions.install",
         "{\"source\":{\"kind\":\"store\",\"store\":\"chrome-web-store\",\"id\":\"" ID_B "\"}}",
         &cap);
    CHECK(failed_with(&cap, "extension", "sourceNotAllowed"));
    CHECK(strstr(cap.message, "stores") != NULL);
    call(p, "extensions.install",
         "{\"source\":{\"kind\":\"store\",\"store\":\"elsewhere\",\"id\":\"" ID_B "\"}}", &cap);
    CHECK(failed_with(&cap, "extension", "sourceNotAllowed"));
    /* A matching update URL reaches the prompt. */
    REQUIRE(bdk_harness_extension_package(t_runtime, "updateUrl", ID_A, kPlainManifest) == BDK_OK);
    REQUIRE(invoke(p, "extensions.install",
                   "{\"source\":{\"kind\":\"updateUrl\",\"id\":\"" ID_A
                   "\",\"updateUrl\":\"https://updates.example.com/plain.xml\"}}",
                   &cap) != 0);
    t_pump();
    CHECK(g_calls == 1 && strstr(g_payload, "\"source\":\"updateUrl\"") != NULL);
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_complete(g_request, S("deny"), S("{}"), no_gesture(), &err) == BDK_OK);
    t_pump();
    CHECK(failed_with(&cap, "extension", "cancelled"));
    /* Malformed sources are the caller's mistake. */
    call(p, "extensions.install", "{\"source\":{\"kind\":\"updateUrl\",\"id\":\"short\"}}", &cap);
    CHECK(failed_with(&cap, "broker", "invalidArguments"));
    call(p, "extensions.install", "{\"source\":{\"kind\":\"bundled\"}}", &cap);
    CHECK(failed_with(&cap, "broker", "invalidArguments"));
}

static void test_ext_store_source_when_enabled(void) {
    NEEDS_HOOKS();
    start_frozen_with("\"extensions\":{\"install\":{\"stores\":[\"chrome-web-store\"]}}",
                      kInstallSurfaces, 3);
    bdk_profile_t p = t_new_profile("Ext");
    REQUIRE(bdk_harness_extension_package(t_runtime, "store", "chrome-web-store/" ID_B,
                                          kPlainManifest) == BDK_OK);
    t_capture_t cap;
    REQUIRE(invoke(p, "extensions.install",
                   "{\"source\":{\"kind\":\"store\",\"store\":\"chrome-web-store\",\"id\":\"" ID_B
                   "\"}}",
                   &cap) != 0);
    t_pump();
    REQUIRE(g_calls == 1);
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_complete(g_request, S("grant"), S("{}"), gesture(), &err) == BDK_OK);
    t_pump();
    CHECK(!cap.has_error && strstr(cap.json, "\"id\":\"" ID_B "\"") != NULL);
    CHECK(strstr(cap.json, "\"store\":\"chrome-web-store\"") != NULL);
}

static void test_ext_incompatible_manifests_are_refused_with_the_finding(void) {
    NEEDS_HOOKS();
    start_frozen_with("\"extensions\":{\"ui\":{\"side_panels\":\"disabled\"}}", kInstallSurfaces,
                      3);
    bdk_profile_t p = t_new_profile("Ext");
    REQUIRE(bdk_harness_extension_package(t_runtime, "unpacked", "/fixtures/panel",
                                          kSidePanelManifest) == BDK_OK);
    t_capture_t cap;
    call(p, "extensions.install",
         "{\"source\":{\"kind\":\"unpacked\",\"path\":\"/fixtures/panel\"}}", &cap);
    CHECK(failed_with(&cap, "extension", "incompatible"));
    CHECK(strstr(cap.message, "surface.sidePanel.disabled") != NULL);
    CHECK(g_calls == 0); /* refused before any prompt */

    t_capture_t report;
    call(p, "extensions.inspect",
         "{\"source\":{\"kind\":\"unpacked\",\"path\":\"/fixtures/panel\"}}", &report);
    CHECK(!report.has_error && strstr(report.json, "\"verdict\":\"incompatible\"") != NULL);
    CHECK(strstr(report.json, "\"subject\":\"side_panel\"") != NULL);

    REQUIRE(bdk_harness_extension_package(
                t_runtime, "unpacked", "/fixtures/old",
                "{\"manifest_version\":2,\"name\":\"Old\",\"version\":\"1\"}") == BDK_OK);
    call(p, "extensions.inspect", "{\"source\":{\"kind\":\"unpacked\",\"path\":\"/fixtures/old\"}}",
         &report);
    CHECK(strstr(report.json, "manifest.version.unsupported") != NULL);
    REQUIRE(bdk_harness_extension_package(
                t_runtime, "unpacked", "/fixtures/new",
                "{\"manifest_version\":3,\"name\":\"New\",\"version\":\"1\","
                "\"minimum_chrome_version\":\"999\",\"permissions\":[\"identity\"]}") == BDK_OK);
    call(p, "extensions.inspect", "{\"source\":{\"kind\":\"unpacked\",\"path\":\"/fixtures/new\"}}",
         &report);
    CHECK(strstr(report.json, "manifest.minimumChromeVersion") != NULL);
    CHECK(strstr(report.json, "api.identity.getAuthToken.unavailable") != NULL);
    call(p, "extensions.inspect", "{}", &report);
    CHECK(failed_with(&report, "broker", "invalidArguments"));
    call(p, "extensions.inspect",
         "{\"source\":{\"kind\":\"unpacked\",\"path\":\"/fixtures/none\"}}", &report);
    CHECK(failed_with(&report, "extension", "manifestInvalid"));
}

static void test_ext_allow_all_installs_with_the_findings_in_the_prompt(void) {
    NEEDS_HOOKS();
    start_frozen_with("\"extensions\":{\"install_policy\":\"allow_all\",\"ui\":{\"side_panels\":"
                      "\"disabled\"}}",
                      kInstallSurfaces, 3);
    bdk_profile_t p = t_new_profile("Ext");
    REQUIRE(bdk_harness_extension_package(t_runtime, "unpacked", "/fixtures/panel",
                                          kSidePanelManifest) == BDK_OK);
    t_capture_t cap;
    REQUIRE(invoke(p, "extensions.install",
                   "{\"source\":{\"kind\":\"unpacked\",\"path\":\"/fixtures/panel\"}}", &cap) != 0);
    t_pump();
    REQUIRE(g_calls == 1);
    CHECK(strstr(g_payload, "\"verdict\":\"incompatible\"") != NULL);
    CHECK(strstr(g_payload, "surface.sidePanel.disabled") != NULL);
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_complete(g_request, S("grant"), S("{}"), gesture(), &err) == BDK_OK);
    t_pump();
    CHECK(!cap.has_error);
}

static void test_ext_updates_apply_refuse_and_disable(void) {
    NEEDS_HOOKS();
    start_frozen_with("\"extensions\":{\"ui\":{\"context_menus\":\"disabled\"}}", kInstallSurfaces,
                      3);
    bdk_profile_t p = t_new_profile("Ext");
    REQUIRE(bdk_harness_extension_package(t_runtime, "updateUrl", ID_A, kPlainManifest) == BDK_OK);
    t_capture_t cap;
    REQUIRE(invoke(p, "extensions.install",
                   "{\"source\":{\"kind\":\"updateUrl\",\"id\":\"" ID_A
                   "\",\"updateUrl\":\"https://updates.example.com/u.xml\"}}",
                   &cap) != 0);
    t_pump();
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_complete(g_request, S("grant"), S("{}"), gesture(), &err) == BDK_OK);
    t_pump();
    REQUIRE(!cap.has_error);

    named_events_t updated, enabled;
    watch("extensions.updated", &updated);
    watch("extensions.enabledChanged", &enabled);
    /* Nothing newer: nothing changes. */
    call(p, "extensions.checkForUpdates", "{}", &cap);
    CHECK(!cap.has_error && strcmp(cap.json, "{\"updated\":[],\"refused\":[]}") == 0);
    CHECK(cap.progress >= 1);

    /* A version that needs a disabled surface is refused; the installed one keeps running. */
    REQUIRE(bdk_harness_extension_package(
                t_runtime, "updateUrl", ID_A,
                "{\"manifest_version\":3,\"name\":\"Plain\",\"version\":\"2.0\",\"permissions\":"
                "[\"storage\",\"contextMenus\"]}") == BDK_OK);
    call(p, "extensions.checkForUpdates", "{\"id\":\"" ID_A "\"}", &cap);
    CHECK(!cap.has_error && strstr(cap.json, "\"updated\":[]") != NULL);
    CHECK(strstr(cap.json, "surface.contextMenus.disabled") != NULL);
    t_capture_t got;
    call(p, "extensions.get", "{\"id\":\"" ID_A "\"}", &got);
    CHECK(strstr(got.json, "\"version\":\"1.0\"") != NULL);
    CHECK(strstr(got.json, "\"isEnabled\":true") != NULL);
    CHECK(updated.count == 0);

    /* A version that asks for more applies and is disabled until the user agrees again. */
    REQUIRE(bdk_harness_extension_package(
                t_runtime, "updateUrl", ID_A,
                "{\"manifest_version\":3,\"name\":\"Plain\",\"version\":\"3.0\",\"permissions\":"
                "[\"storage\",\"tabs\"],\"host_permissions\":[\"https://example.com/*\"]}") ==
            BDK_OK);
    call(p, "extensions.checkForUpdates", "{}", &cap);
    CHECK(!cap.has_error && strstr(cap.json, "\"updated\":[\"" ID_A "\"]") != NULL);
    CHECK(updated.count == 1 && strstr(updated.last, "\"previousVersion\":\"1.0\"") != NULL);
    CHECK(enabled.count == 1 && strstr(enabled.last, "permissionsIncreased") != NULL);
    call(p, "extensions.get", "{\"id\":\"" ID_A "\"}", &got);
    CHECK(strstr(got.json, "\"isEnabled\":false") != NULL);

    /* Re-enabling asks again. */
    g_calls = 0;
    REQUIRE(invoke(p, "extensions.setEnabled", "{\"id\":\"" ID_A "\",\"enabled\":true}", &cap) !=
            0);
    t_pump();
    REQUIRE(g_calls == 1);
    CHECK(strstr(g_payload, "\"kind\":\"reEnable\"") != NULL);
    REQUIRE(bdk_ui_complete(g_request, S("grant"), S("{}"), gesture(), &err) == BDK_OK);
    t_pump();
    CHECK(!cap.has_error);
    CHECK(enabled.count == 2 && strstr(enabled.last, "\"isEnabled\":true") != NULL);

    call(p, "extensions.checkForUpdates", "{\"id\":\"" ID_B "\"}", &cap);
    CHECK(failed_with(&cap, "extension", "notFound"));
}

static void test_ext_reload_only_unpacked(void) {
    NEEDS_HOOKS();
    start_frozen_with("", kInstallSurfaces, 3);
    bdk_profile_t p = t_new_profile("Ext");
    t_capture_t cap;
    install_unpacked(p, "/fixtures/plain", kPlainManifest, &cap);
    char id[40] = "", args[96];
    REQUIRE(t_json_string(cap.json, "id", id, sizeof id));
    snprintf(args, sizeof args, "{\"id\":\"%s\"}", id);
    REQUIRE(bdk_harness_extension_package(
                t_runtime, "unpacked", "/fixtures/plain",
                "{\"manifest_version\":3,\"name\":\"Plain\",\"version\":\"1.1\"}") == BDK_OK);
    call(p, "extensions.reload", args, &cap);
    CHECK(!cap.has_error);
    t_capture_t got;
    call(p, "extensions.get", args, &got);
    CHECK(strstr(got.json, "\"version\":\"1.1\"") != NULL);

    t_stop();
    start_frozen_with("\"extensions\":{\"install\":{\"stores\":[\"chrome-web-store\"]}}",
                      kInstallSurfaces, 3);
    p = t_new_profile("Ext");
    REQUIRE(bdk_harness_extension_package(t_runtime, "store", "chrome-web-store/" ID_B,
                                          kPlainManifest) == BDK_OK);
    REQUIRE(invoke(p, "extensions.install",
                   "{\"source\":{\"kind\":\"store\",\"store\":\"chrome-web-store\",\"id\":\"" ID_B
                   "\"}}",
                   &cap) != 0);
    t_pump();
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_complete(g_request, S("grant"), S("{}"), gesture(), &err) == BDK_OK);
    t_pump();
    call(p, "extensions.reload", "{\"id\":\"" ID_B "\"}", &cap);
    CHECK(failed_with(&cap, "extension", "notUnpacked"));
}

static void test_ext_uninstall_asks(void) {
    NEEDS_HOOKS();
    start_frozen_with("", kInstallSurfaces, 3);
    bdk_profile_t p = t_new_profile("Ext");
    t_capture_t cap;
    install_unpacked(p, "/fixtures/plain", kPlainManifest, &cap);
    char id[40] = "", args[96];
    REQUIRE(t_json_string(cap.json, "id", id, sizeof id));
    snprintf(args, sizeof args, "{\"id\":\"%s\"}", id);
    named_events_t uninstalled;
    watch("extensions.uninstalled", &uninstalled);

    bdk_error_t err = 0;
    g_calls = 0;
    REQUIRE(invoke(p, "extensions.uninstall", args, &cap) != 0);
    t_pump();
    REQUIRE(g_calls == 1 && g_surface == BDK_SURFACE_EXTENSION_UNINSTALLS);
    CHECK(strstr(g_payload, "\"requestedBy\":{\"kind\":\"user\"}") != NULL);
    REQUIRE(bdk_ui_complete(g_request, S("deny"), S("{}"), no_gesture(), &err) == BDK_OK);
    t_pump();
    CHECK(failed_with(&cap, "extension", "cancelled"));
    CHECK(uninstalled.count == 0);

    REQUIRE(invoke(p, "extensions.uninstall", args, &cap) != 0);
    t_pump();
    CHECK(bdk_ui_complete(g_request, S("grant"), S("{}"), no_gesture(), &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "ui", "gestureRequired"));
    REQUIRE(bdk_ui_complete(g_request, S("grant"), S("{}"), gesture(), &err) == BDK_OK);
    t_pump();
    CHECK(!cap.has_error && uninstalled.count == 1);
    call(p, "extensions.get", args, &cap);
    CHECK(failed_with(&cap, "extension", "notFound"));
    call(p, "extensions.uninstall", args, &cap);
    CHECK(failed_with(&cap, "extension", "notFound"));
}

static void test_ext_settings_of_one_extension(void) {
    NEEDS_HOOKS();
    start_frozen_with("\"extensions\":{\"allow_in_private\":\"never\"}", kInstallSurfaces, 3);
    bdk_profile_t p = t_new_profile("Ext");
    t_capture_t cap;
    install_unpacked(p, "/fixtures/popup", kPopupManifest, &cap);
    char id[40] = "", args[160];
    REQUIRE(t_json_string(cap.json, "id", id, sizeof id));

    snprintf(args, sizeof args, "{\"id\":\"%s\",\"allowed\":true}", id);
    call(p, "extensions.setAllowedInPrivate", args, &cap);
    CHECK(failed_with(&cap, "extension", "privateNotAllowed"));
    call(p, "extensions.setAllowedFileAccess", args, &cap);
    CHECK(!cap.has_error);
    snprintf(args, sizeof args, "{\"id\":\"%s\",\"access\":\"onSpecificSites\"}", id);
    call(p, "extensions.setSiteAccess", args, &cap);
    CHECK(failed_with(&cap, "broker", "invalidArguments"));
    snprintf(args, sizeof args,
             "{\"id\":\"%s\",\"access\":\"onSpecificSites\",\"patterns\":[\"https://a.test/*\"]}",
             id);
    call(p, "extensions.setSiteAccess", args, &cap);
    CHECK(!cap.has_error);
    named_events_t changed;
    watch("extensions.actionChanged", &changed);
    snprintf(args, sizeof args, "{\"id\":\"%s\",\"pinned\":true}", id);
    call(p, "extensions.setPinned", args, &cap);
    CHECK(!cap.has_error && changed.count == 1);
    snprintf(args, sizeof args, "{\"id\":\"%s\"}", id);
    t_capture_t got;
    call(p, "extensions.get", args, &got);
    CHECK(strstr(got.json, "\"allowedFileAccess\":true") != NULL);
    CHECK(strstr(got.json, "\"isPinned\":true") != NULL);
    CHECK(strstr(got.json, "\"siteAccessPatterns\":[\"https://a.test/*\"]") != NULL);
    CHECK(strstr(got.json, "\"surfaces\":[\"action\",\"popup\"]") != NULL);
    call(p, "extensions.clearErrors", args, &cap);
    CHECK(!cap.has_error);

    /* Declared keyboard commands and the user's binding. */
    call(p, "extensions.commands", "{}", &cap);
    CHECK(strstr(cap.json, "\"name\":\"toggle\"") != NULL);
    CHECK(strstr(cap.json, "\"shortcut\":\"Command+Shift+U\"") != NULL);
    CHECK(strstr(cap.json, "\"isAction\":true") != NULL);
    snprintf(args, sizeof args,
             "{\"extensionId\":\"%s\",\"name\":\"toggle\",\"shortcut\":\"Command+Shift+K\"}", id);
    call(p, "extensions.setCommandShortcut", args, &cap);
    CHECK(!cap.has_error);
    call(p, "extensions.commands", "{}", &cap);
    CHECK(strstr(cap.json, "Command+Shift+K") != NULL);
    snprintf(args, sizeof args, "{\"extensionId\":\"%s\",\"name\":\"missing\"}", id);
    call(p, "extensions.setCommandShortcut", args, &cap);
    CHECK(failed_with(&cap, "extension", "notFound"));
    call(p, "extensions.setPinned", "{\"id\":\"" ID_B "\",\"pinned\":true}", &cap);
    CHECK(failed_with(&cap, "extension", "notFound"));
}

static void test_ext_actions_and_activation(void) {
    NEEDS_HOOKS();
    start_frozen_with("", kInstallSurfaces, 3);
    bdk_profile_t p = t_new_profile("Ext");
    bdk_contents_t page = t_new_contents(p);
    t_capture_t cap;
    install_unpacked(p, "/fixtures/popup", kPopupManifest, &cap);
    char popup[40] = "", click[40] = "";
    REQUIRE(t_json_string(cap.json, "id", popup, sizeof popup));
    install_unpacked(p, "/fixtures/click", kClickManifest, &cap);
    REQUIRE(t_json_string(cap.json, "id", click, sizeof click));
    install_unpacked(p, "/fixtures/plain", kPlainManifest, &cap);

    call(p, "extensions.actions", "{}", &cap);
    CHECK(strstr(cap.json, "\"title\":\"Open\"") != NULL);
    CHECK(strstr(cap.json, "\"hasPopup\":true") != NULL);
    CHECK(strstr(cap.json, "\"title\":\"Run\"") != NULL);
    CHECK(strstr(cap.json, "Plain") == NULL); /* no action, no entry */

    bdk_buffer_t out = 0;
    bdk_error_t err = 0;
    char text[160];
    REQUIRE(bdk_extension_action_activate(page, S(click), gesture(), &out, &err) == BDK_OK);
    t_buffer_text(out, text, sizeof text);
    CHECK(strcmp(text, "{\"outcome\":\"dispatchedClick\"}") == 0);

    g_calls = 0;
    REQUIRE(bdk_extension_action_activate(page, S(popup), gesture(), &out, &err) == BDK_OK);
    t_buffer_text(out, text, sizeof text);
    CHECK(strcmp(text, "{\"outcome\":\"popupRequested\"}") == 0);
    CHECK(g_calls == 0); /* the popup follows as a request */
    t_pump();
    REQUIRE(g_calls == 1 && g_surface == BDK_SURFACE_EXTENSION_POPUPS);
    CHECK(strstr(g_payload, "\"trigger\":\"userClick\"") != NULL);
    CHECK(strstr(g_payload, "popup.html") != NULL);
    REQUIRE(bdk_ui_update(g_request, S("{\"placed\":true}"), &err) == BDK_OK);
    REQUIRE(bdk_harness_send_ui_update(
                t_runtime, "{\"preferredSize\":{\"width\":320,\"height\":200}}") == BDK_OK);
    t_pump();
    CHECK(g_updates == 1 && strstr(g_update, "320") != NULL);
    char last[96];
    REQUIRE(bdk_harness_last_ui_update(t_runtime, last, sizeof last) == BDK_OK);
    CHECK(strcmp(last, "{\"placed\":true}") == 0);
    REQUIRE(bdk_ui_complete(g_request, S("accept"), S("{\"action\":\"close\"}"), no_gesture(),
                            &err) == BDK_OK);
    t_pump();

    /* The action's keyboard command opens the popup too; other commands are dispatched. */
    g_calls = 0;
    REQUIRE(bdk_extension_command_perform(page, S(popup), S("_execute_action"), gesture(), &out,
                                          &err) == BDK_OK);
    t_pump();
    CHECK(g_calls == 1 && strstr(g_payload, "\"trigger\":\"command\"") != NULL);
    REQUIRE(bdk_extension_command_perform(page, S(popup), S("toggle"), gesture(), &out, &err) ==
            BDK_OK);
    t_buffer_text(out, text, sizeof text);
    CHECK(strcmp(text, "{\"outcome\":\"dispatchedClick\"}") == 0);

    /* A disabled extension cannot be activated. */
    char args[96];
    snprintf(args, sizeof args, "{\"id\":\"%s\",\"enabled\":false}", click);
    call(p, "extensions.setEnabled", args, &cap);
    CHECK(bdk_extension_action_activate(page, S(click), gesture(), &out, &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "extension", "notFound"));
    call(p, "extensions.actions", "{}", &cap);
    CHECK(strstr(cap.json, "\"title\":\"Run\"") == NULL);
}

static void test_ext_popup_only_action_is_blocked_when_popups_are_disabled(void) {
    NEEDS_HOOKS();
    /* With allow_all it installs; a click is then blocked rather than silently lost. */
    start_frozen_with("\"extensions\":{\"install_policy\":\"allow_all\",\"ui\":{\"popups\":"
                      "\"disabled\"}}",
                      kInstallSurfaces, 3);
    bdk_profile_t p = t_new_profile("Ext");
    bdk_contents_t page = t_new_contents(p);
    t_capture_t cap;
    install_unpacked(p, "/fixtures/popup", kPopupManifest, &cap);
    char id[40] = "";
    REQUIRE(t_json_string(cap.json, "id", id, sizeof id));
    CHECK(strstr(cap.json, "surface.popup.disabled") != NULL);
    bdk_buffer_t out = 0;
    bdk_error_t err = 0;
    REQUIRE(bdk_extension_action_activate(page, S(id), gesture(), &out, &err) == BDK_OK);
    char text[160];
    t_buffer_text(out, text, sizeof text);
    CHECK(strstr(text, "\"outcome\":\"blocked\"") != NULL);
    CHECK(strstr(text, "popups") != NULL);
}

static void test_ext_options_pages(void) {
    NEEDS_HOOKS();
    start_frozen_with("\"extensions\":{\"ui\":{\"options_pages\":\"disabled\"}}", kInstallSurfaces,
                      3);
    bdk_profile_t p = t_new_profile("Ext");
    t_capture_t cap;
    install_unpacked(p, "/fixtures/options",
                     "{\"manifest_version\":3,\"name\":\"Options\",\"version\":\"1\","
                     "\"options_ui\":{\"page\":\"options.html\"}}",
                     &cap);
    char id[40] = "", args[96];
    REQUIRE(t_json_string(cap.json, "id", id, sizeof id));
    CHECK(strstr(cap.json, "surface.optionsPage.disabled") != NULL); /* degrading, not refused */
    snprintf(args, sizeof args, "{\"id\":\"%s\"}", id);
    call(p, "extensions.openOptions", args, &cap);
    CHECK(failed_with(&cap, "extension", "surfaceDisabled"));
}

const t_test_t t_extension_tests[] = {
    {"ext_custom_popups_need_a_handler_at_the_freeze",
     test_ext_custom_popups_need_a_handler_at_the_freeze},
    {"ext_unknown_extension_surface_fails_start", test_ext_unknown_extension_surface_fails_start},
    {"ext_handlers_accept_every_extension_surface",
     test_ext_handlers_accept_every_extension_surface},
    {"ext_disabled_extensions_disable_every_extension_surface",
     test_ext_disabled_extensions_disable_every_extension_surface},
    {"ext_activation_needs_a_gesture_and_an_extension",
     test_ext_activation_needs_a_gesture_and_an_extension},
    {"ext_activation_refused_when_actions_are_disabled",
     test_ext_activation_refused_when_actions_are_disabled},
    {"ext_activation_is_refused_off_the_ui_thread",
     test_ext_activation_is_refused_off_the_ui_thread},
    {"ext_window_kind_is_checked", test_ext_window_kind_is_checked},
    {"ext_open_request_updates_until_it_finishes", test_ext_open_request_updates_until_it_finishes},
    {"ext_open_request_updates_reach_both_sides", test_ext_open_request_updates_reach_both_sides},
    {"ext_windows_report_their_pages", test_ext_windows_report_their_pages},
    {"ext_window_anchor_and_request_handlers", test_ext_window_anchor_and_request_handlers},
    {"ext_window_handlers_are_released_at_shutdown",
     test_ext_window_handlers_are_released_at_shutdown},
    {"ext_default_menu_needs_default_actions", test_ext_default_menu_needs_default_actions},
    {"ext_install_asks_and_needs_a_gesture", test_ext_install_asks_and_needs_a_gesture},
    {"ext_install_without_confirmation_installs_nothing",
     test_ext_install_without_confirmation_installs_nothing},
    {"ext_sources_follow_the_configuration", test_ext_sources_follow_the_configuration},
    {"ext_store_source_when_enabled", test_ext_store_source_when_enabled},
    {"ext_incompatible_manifests_are_refused_with_the_finding",
     test_ext_incompatible_manifests_are_refused_with_the_finding},
    {"ext_allow_all_installs_with_the_findings_in_the_prompt",
     test_ext_allow_all_installs_with_the_findings_in_the_prompt},
    {"ext_updates_apply_refuse_and_disable", test_ext_updates_apply_refuse_and_disable},
    {"ext_reload_only_unpacked", test_ext_reload_only_unpacked},
    {"ext_uninstall_asks", test_ext_uninstall_asks},
    {"ext_settings_of_one_extension", test_ext_settings_of_one_extension},
    {"ext_actions_and_activation", test_ext_actions_and_activation},
    {"ext_popup_only_action_is_blocked_when_popups_are_disabled",
     test_ext_popup_only_action_is_blocked_when_popups_are_disabled},
    {"ext_options_pages", test_ext_options_pages},
    {NULL, NULL},
};
