/* Internal pages (T-WUI-02 to T-WUI-06) and page commands (T-SWF-06): the route table and its
 * registrations, what a branded or internal URL becomes, the branded form the application sees,
 * native routes, the broker as an internal page reaches it, and the page controls. */
#include <stdlib.h>

#include "harness_broker.h"
#include "test_support.h"

/* settings and agents are bundled pages, history is Chromium's, downloads is native, flags is
   disabled, prefs is an alias of settings. Bundled paths are inside the bundle's resources. */
static const char* kRoutes =
    "{\"browser\":{\"name\":\"BDK Conformance\",\"internal_scheme\":\"conf\"},"
    "\"routes\":{"
    "\"agents\":{\"mode\":\"bundled\",\"path\":\"Pages/Agents\",\"host\":\"bdk-agents\","
    "\"capabilities\":[]},"
    "\"downloads\":{\"mode\":\"native\"},"
    "\"flags\":{\"mode\":\"disabled\"},"
    "\"history\":{\"mode\":\"chromium\"},"
    "\"settings\":{\"mode\":\"bundled\",\"path\":\"Pages/Settings\",\"host\":\"bdk-settings\","
    "\"capabilities\":[\"settings.read\",\"settings.write\",\"app.appearance.*\"]}},"
    "\"aliases\":{\"prefs\":\"settings\"}}";

/* A native page handler that records the request and answers as the test chose. */
typedef struct native_seen {
    int calls, releases;
    uint32_t answer; /* 0: keep the context in held */
    bdk_context_t held;
    bdk_contents_t source;
    bdk_profile_t profile;
    char request[256];
} native_seen_t;

static native_seen_t g_native;

static void on_native_page(void* ctx, bdk_context_t call, bdk_string_t request) {
    native_seen_t* n = (native_seen_t*)ctx;
    ++n->calls;
    snprintf(n->request, sizeof n->request, "%.*s", (int)request.length, request.data);
    n->source = bdk_native_page_source(call);
    n->profile = bdk_app_command_profile(call);
    if (n->answer)
        bdk_native_page_complete(call, n->answer);
    else
        n->held = call;
}

static void on_native_release(void* ctx) {
    ++((native_seen_t*)ctx)->releases;
}

/* Starts with kRoutes and a handler for downloads that answers `answer`, then freezes. */
static bdk_profile_t start_routes(uint32_t answer) {
    memset(&g_native, 0, sizeof g_native);
    g_native.answer = answer;
    t_start_with(kRoutes);
    bdk_error_t err = 0;
    REQUIRE(bdk_page_handler_register(t_runtime, S("downloads"), BDK_PAGE_MODE_NATIVE,
                                      on_native_page, &g_native, on_native_release,
                                      &err) == BDK_OK);
    t_freeze();
    return t_new_profile("Pages");
}

/* Loads url in a new page of profile and pumps; the events go to ev. */
static bdk_contents_t load_in_new_page(bdk_profile_t profile, const char* url, t_events_t* ev) {
    bdk_contents_t c = t_new_contents(profile);
    t_ev_init(ev);
    REQUIRE(bdk_contents_subscribe(c, t_on_contents_event, ev, t_on_event_release) != 0);
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_load(c, S(url), &err) == BDK_OK);
    t_pump();
    return c;
}

/* The index of the first event of kind, or -1. */
static int find_event(const t_events_t* ev, uint32_t kind) {
    for (int i = 0; i < ev->count; ++i) {
        if (ev->kind[i] == kind)
            return i;
    }
    return -1;
}

/* No event carried a chrome:// URL. */
static int no_internal_url(const t_events_t* ev) {
    for (int i = 0; i < ev->count; ++i) {
        if (strncmp(ev->url[i], "chrome:", 7) == 0)
            return 0;
    }
    return 1;
}

static void current_url(bdk_contents_t c, char* out, size_t n) {
    bdk_contents_state_t st;
    memset(&st, 0, sizeof st);
    st.struct_size = sizeof st;
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_state(c, &st, &err) == BDK_OK);
    snprintf(out, n, "%.*s", (int)st.url.length, st.url.data ? st.url.data : "");
}

/* ---- The route table (T-WUI-02, T-WUI-05) ---- */

static void test_a_bundled_page_is_reported_by_its_branded_url(void) {
    bdk_profile_t p = start_routes(BDK_NATIVE_PAGE_HANDLED);
    t_events_t ev;
    bdk_contents_t c = load_in_new_page(p, "conf://settings/privacy?x=1", &ev);
    int committed = find_event(&ev, BDK_CONTENTS_EVENT_NAVIGATION_COMMITTED);
    REQUIRE(committed >= 0);
    CHECK(strcmp(ev.url[committed], "conf://settings/privacy?x=1") == 0);
    CHECK(no_internal_url(&ev));
    char url[128];
    current_url(c, url, sizeof url);
    CHECK(strcmp(url, "conf://settings/privacy?x=1") == 0);

    bdk_buffer_t entries = 0;
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_entries(c, &entries, &err) == BDK_OK);
    char json[512];
    t_buffer_text(entries, json, sizeof json);
    CHECK(strstr(json, "conf://settings/privacy?x=1") != NULL);
    CHECK(strstr(json, "chrome:") == NULL);
}

static void test_an_alias_shares_its_targets_page(void) {
    bdk_profile_t p = start_routes(BDK_NATIVE_PAGE_HANDLED);
    t_events_t ev;
    bdk_contents_t c = load_in_new_page(p, "conf://prefs", &ev);
    int committed = find_event(&ev, BDK_CONTENTS_EVENT_NAVIGATION_COMMITTED);
    REQUIRE(committed >= 0);
    /* The page is the target's: the alias is shown as the route it serves. */
    CHECK(strcmp(ev.url[committed], "conf://settings/") == 0);
    char url[128];
    current_url(c, url, sizeof url);
    CHECK(strcmp(url, "conf://settings/") == 0);
}

static void test_a_built_in_page_is_reported_by_its_branded_url(void) {
    bdk_profile_t p = start_routes(BDK_NATIVE_PAGE_HANDLED);
    t_events_t ev;
    (void)load_in_new_page(p, "conf://history", &ev);
    int committed = find_event(&ev, BDK_CONTENTS_EVENT_NAVIGATION_COMMITTED);
    REQUIRE(committed >= 0);
    CHECK(strcmp(ev.url[committed], "conf://history/") == 0);
    CHECK(no_internal_url(&ev));
}

static void expect_error_page(const t_events_t* ev, const char* url, int32_t net_error) {
    int failed = find_event(ev, BDK_CONTENTS_EVENT_NAVIGATION_FAILED);
    REQUIRE(failed >= 0);
    CHECK(ev->net_error[failed] == net_error);
    CHECK((ev->flags[failed] & BDK_EVENT_FLAG_ERROR_PAGE) != 0);
    CHECK(strcmp(ev->url[failed], url) == 0);
    CHECK(find_event(ev, BDK_CONTENTS_EVENT_NAVIGATION_COMMITTED) < 0);
}

static void test_a_disabled_page_shows_an_error_page(void) {
    bdk_profile_t p = start_routes(BDK_NATIVE_PAGE_HANDLED);
    t_events_t ev;
    (void)load_in_new_page(p, "conf://flags", &ev);
    expect_error_page(&ev, "conf://flags/", -20); /* net::ERR_BLOCKED_BY_CLIENT */
}

static void test_an_unknown_route_shows_an_error_page(void) {
    bdk_profile_t p = start_routes(BDK_NATIVE_PAGE_HANDLED);
    t_events_t ev;
    (void)load_in_new_page(p, "conf://nothing-here", &ev);
    expect_error_page(&ev, "conf://nothing-here/", -300); /* net::ERR_INVALID_URL */
}

static void test_an_overridden_or_disabled_built_in_has_no_internal_way_in(void) {
    bdk_profile_t p = start_routes(BDK_NATIVE_PAGE_HANDLED);
    /* settings is a bundled page, so Chromium's own settings page is not reachable. */
    const char* blocked[] = {"chrome://settings/", "chrome://flags/", "chrome://bdk-downloads/",
                             "chrome://policy/"};
    for (size_t i = 0; i < sizeof blocked / sizeof blocked[0]; ++i) {
        t_events_t ev;
        (void)load_in_new_page(p, blocked[i], &ev);
        expect_error_page(&ev, blocked[i], -20);
    }
    /* A route's own internal URL loads, and is reported in its branded form. */
    t_events_t ev;
    (void)load_in_new_page(p, "chrome://bdk-settings/", &ev);
    int committed = find_event(&ev, BDK_CONTENTS_EVENT_NAVIGATION_COMMITTED);
    REQUIRE(committed >= 0);
    CHECK(strcmp(ev.url[committed], "conf://settings/") == 0);
}

static void test_a_web_page_cannot_navigate_to_a_branded_url(void) {
    if (!bdk_harness_page_hooks()) {
        printf("    (skipped: the runtime has no page hooks)\n");
        return;
    }
    bdk_profile_t p = start_routes(BDK_NATIVE_PAGE_HANDLED);
    REQUIRE(bdk_harness_script_navigation(t_runtime, "https://example.test/", 0, "Web") == BDK_OK);
    t_events_t ev;
    bdk_contents_t c = load_in_new_page(p, "https://example.test/", &ev);
    int before = ev.count;
    REQUIRE(bdk_harness_page_navigate(c, "conf://settings") == BDK_OK);
    t_pump();
    CHECK(ev.count == before); /* dropped, as Chromium drops a web page's chrome:// link */
    char url[128];
    current_url(c, url, sizeof url);
    CHECK(strcmp(url, "https://example.test/") == 0);

    /* An internal page may link to another one by its branded URL. */
    t_events_t ev2;
    bdk_contents_t internal = load_in_new_page(p, "conf://settings", &ev2);
    REQUIRE(bdk_harness_page_navigate(internal, "conf://history") == BDK_OK);
    t_pump();
    current_url(internal, url, sizeof url);
    CHECK(strcmp(url, "conf://history/") == 0);
}

/* ---- Native routes ---- */

static void test_a_native_route_calls_its_handler_and_loads_nothing(void) {
    bdk_profile_t p = start_routes(BDK_NATIVE_PAGE_HANDLED);
    t_events_t ev;
    bdk_contents_t c = load_in_new_page(p, "conf://downloads/recent?x=1", &ev);
    CHECK(g_native.calls == 1);
    CHECK(str_has(S(g_native.request), "\"route\":\"downloads\""));
    CHECK(str_has(S(g_native.request), "\"url\":\"conf://downloads/recent?x=1\""));
    CHECK(str_has(S(g_native.request), "\"path\":\"/recent\""));
    CHECK(str_has(S(g_native.request), "\"query\":\"x=1\""));
    CHECK(g_native.source == c);
    CHECK(g_native.profile == p);
    CHECK(find_event(&ev, BDK_CONTENTS_EVENT_NAVIGATION_COMMITTED) < 0);
    char url[128];
    current_url(c, url, sizeof url);
    CHECK(strcmp(url, "about:blank") == 0);
}

static void test_a_native_handler_can_block_with_the_error_page(void) {
    bdk_profile_t p = start_routes(BDK_NATIVE_PAGE_BLOCKED);
    t_events_t ev;
    (void)load_in_new_page(p, "conf://downloads", &ev);
    CHECK(g_native.calls == 1);
    expect_error_page(&ev, "conf://downloads/", -20);
}

static void test_a_native_handler_may_answer_later_and_only_once(void) {
    bdk_profile_t p = start_routes(0);
    t_events_t ev;
    (void)load_in_new_page(p, "conf://downloads", &ev);
    REQUIRE(g_native.calls == 1 && g_native.held != 0);
    CHECK(find_event(&ev, BDK_CONTENTS_EVENT_NAVIGATION_FAILED) < 0);
    bdk_native_page_complete(g_native.held, BDK_NATIVE_PAGE_BLOCKED);
    bdk_native_page_complete(g_native.held, BDK_NATIVE_PAGE_HANDLED); /* stale: ignored */
    t_pump();
    expect_error_page(&ev, "conf://downloads/", -20);
}

static void test_a_native_route_needs_a_handler_before_the_freeze(void) {
    t_start_with(kRoutes);
    bdk_error_t err = 0;
    CHECK(bdk_runtime_freeze_registrations(t_runtime, &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "config", "handlerMissing"));
    /* The runtime stays unfrozen, so the handler can still be added. */
    CHECK(bdk_page_handler_register(t_runtime, S("downloads"), BDK_PAGE_MODE_NATIVE, on_native_page,
                                    &g_native, NULL, &err) == BDK_OK);
    t_freeze();
}

/* ---- Registrations from Swift (architecture.md 8.2) ---- */

static void expect_register_error(const char* route, uint32_t mode, const char* code) {
    bdk_error_t err = 0;
    CHECK(bdk_page_handler_register(t_runtime, S(route), mode, on_native_page, &g_native, NULL,
                                    &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "config", code));
}

static void test_registrations_are_checked(void) {
    t_start_with(kRoutes);
    expect_register_error("agents", BDK_PAGE_MODE_CHROMIUM, "noBuiltin");
    expect_register_error("history", BDK_PAGE_MODE_BUNDLED, "routeUnknown");
    expect_register_error("prefs", BDK_PAGE_MODE_DISABLED, "routeInvalid");
    expect_register_error("print", BDK_PAGE_MODE_NATIVE, "routeInvalid");
    expect_register_error("bdk-thing", BDK_PAGE_MODE_NATIVE, "routeInvalid");
    expect_register_error("Not_A_Route", BDK_PAGE_MODE_NATIVE, "routeInvalid");
    expect_register_error("credits", BDK_PAGE_MODE_DISABLED, "routeMode");
    bdk_error_t err = 0;
    CHECK(bdk_page_handler_register(t_runtime, S("agents"), 99, NULL, NULL, NULL, &err) ==
          BDK_ERR_INVALID_ARGUMENT);
    CHECK(bdk_page_handler_register(t_runtime, S("agents"), BDK_PAGE_MODE_NATIVE, NULL, NULL, NULL,
                                    &err) == BDK_ERR_INVALID_ARGUMENT);
    CHECK(bdk_page_handler_register(t_runtime, S("downloads"), BDK_PAGE_MODE_NATIVE, on_native_page,
                                    &g_native, NULL, &err) == BDK_OK);
    CHECK(bdk_page_handler_register(t_runtime, S("downloads"), BDK_PAGE_MODE_NATIVE, on_native_page,
                                    &g_native, NULL, &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "config", "duplicateName"));
    t_freeze();
    CHECK(bdk_page_handler_register(t_runtime, S("agents"), BDK_PAGE_MODE_DISABLED, NULL, NULL,
                                    NULL, &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "config", "registrationAfterStart"));
}

static void test_a_registration_outranks_the_resolved_table(void) {
    memset(&g_native, 0, sizeof g_native);
    t_start_with(kRoutes);
    bdk_error_t err = 0;
    /* Serve Chromium's settings although a bundled page exists; disable agents; native flags. */
    REQUIRE(bdk_page_handler_register(t_runtime, S("settings"), BDK_PAGE_MODE_CHROMIUM, NULL, NULL,
                                      NULL, &err) == BDK_OK);
    REQUIRE(bdk_page_handler_register(t_runtime, S("agents"), BDK_PAGE_MODE_DISABLED, NULL, NULL,
                                      NULL, &err) == BDK_OK);
    REQUIRE(bdk_page_handler_register(t_runtime, S("downloads"), BDK_PAGE_MODE_NATIVE,
                                      on_native_page, &g_native, NULL, &err) == BDK_OK);
    t_freeze();
    bdk_profile_t p = t_new_profile("Registrations");

    t_events_t ev;
    (void)load_in_new_page(p, "conf://settings", &ev);
    int committed = find_event(&ev, BDK_CONTENTS_EVENT_NAVIGATION_COMMITTED);
    REQUIRE(committed >= 0);
    CHECK(strcmp(ev.url[committed], "conf://settings/") == 0);
    t_events_t ev2;
    (void)load_in_new_page(p, "chrome://bdk-settings/", &ev2); /* the bundled page is out */
    expect_error_page(&ev2, "chrome://bdk-settings/", -20);
    t_events_t ev3;
    (void)load_in_new_page(p, "conf://agents", &ev3);
    expect_error_page(&ev3, "conf://agents/", -20);
}

static void test_registered_contexts_are_released_once(void) {
    memset(&g_native, 0, sizeof g_native);
    t_start_with(kRoutes);
    bdk_error_t err = 0;
    REQUIRE(bdk_page_handler_register(t_runtime, S("downloads"), BDK_PAGE_MODE_NATIVE,
                                      on_native_page, &g_native, on_native_release,
                                      &err) == BDK_OK);
    t_freeze();
    t_stop();
    CHECK(g_native.releases == 1);
}

static void test_a_malformed_route_table_fails_the_start(void) {
    const char* bad[] = {
        "{\"routes\":{\"credits\":{\"mode\":\"disabled\"}}}",
        "{\"routes\":{\"agents\":{\"mode\":\"chromium\"}}}",
        "{\"routes\":{\"agents\":{\"mode\":\"bundled\",\"path\":\"../"
        "x\",\"host\":\"bdk-agents\"}}}",
        "{\"routes\":{\"agents\":{\"mode\":\"bundled\",\"path\":\"Pages/A\",\"host\":\"agents\"}}}",
        ("{\"routes\":{\"agents\":{\"mode\":\"bundled\",\"path\":\"Pages/"
         "A\",\"host\":\"bdk-agents\","
         "\"capabilities\":[\"contents.read\"]}}}"),
        "{\"routes\":{\"agents\":{\"mode\":\"somewhere\"}}}",
        "{\"aliases\":{\"x\":\"y\"}}",
        "{\"browser\":{\"internal_scheme\":\"https\"}}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        bdk_start_config_t c;
        t_fill_start_config(&c);
        c.resolved_config_json = S(bad[i]);
        bdk_runtime_t rt = 0;
        bdk_error_t err = 0;
        CHECK(bdk_runtime_start(&c, &rt, &err) == BDK_ERR_FAILED);
        CHECK(rt == 0);
        CHECK(t_error_is(err, "config", "resolvedInvalid"));
    }
}

static void test_without_a_scheme_urls_are_reported_as_they_are(void) {
    t_start_frozen(); /* the suite's minimal configuration names no scheme */
    bdk_profile_t p = t_new_profile("No scheme");
    t_events_t ev;
    (void)load_in_new_page(p, "chrome://history/", &ev);
    int committed = find_event(&ev, BDK_CONTENTS_EVENT_NAVIGATION_COMMITTED);
    REQUIRE(committed >= 0);
    CHECK(strcmp(ev.url[committed], "chrome://history/") == 0);
}

/* ---- The broker as an internal page reaches it (T-WUI-04) ---- */

#define NEED_PAGE_HOOKS()                                                                          \
    do {                                                                                           \
        if (!bdk_harness_page_hooks() || !bdk_harness_broker_hooks()) {                            \
            printf("    (skipped: the runtime has no page hooks)\n");                              \
            return;                                                                                \
        }                                                                                          \
    } while (0)

static void call_as(bdk_harness_caller_t caller, const char* command, const char* args,
                    t_capture_t* cap) {
    t_cap_init(cap);
    REQUIRE(bdk_harness_invoke_as(t_runtime, caller, command, args, NULL, 0, t_on_result, NULL, cap,
                                  t_on_release) != 0);
    t_pump();
    REQUIRE(cap->results == 1);
}

static void test_a_bundled_page_gets_its_routes_capabilities(void) {
    NEED_PAGE_HOOKS();
    bdk_profile_t p = start_routes(BDK_NATIVE_PAGE_HANDLED);
    t_events_t ev;
    bdk_contents_t c = load_in_new_page(p, "conf://settings", &ev);
    bdk_harness_caller_t caller = 0;
    bdk_error_t err = 0;
    REQUIRE(bdk_harness_page_bind(t_runtime, c, 0, &caller, &err) == BDK_OK);

    t_capture_t cap;
    call_as(caller, "runtime.caller", "{}", &cap);
    CHECK(!cap.has_error);
    CHECK(str_has(S(cap.json), "\"kind\":\"page\""));
    CHECK(str_has(S(cap.json), "\"name\":\"settings\""));
    CHECK(str_has(S(cap.json), "\"settings.read\""));
    CHECK(str_has(S(cap.json), "\"internalScheme\":\"conf\""));

    call_as(caller, "settings.set", "{\"key\":\"browser.homePage\",\"value\":\"https://a.test/\"}",
            &cap);
    CHECK(!cap.has_error);
    /* The host sees the value the page wrote: there is one settings store. */
    bdk_invoke_options_t o;
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.profile = p;
    t_cap_init(&cap);
    REQUIRE(bdk_invoke_named(t_runtime, S("settings.get"), S("{\"key\":\"browser.homePage\"}"), &o,
                             t_on_result, NULL, &cap, t_on_release) != 0);
    t_pump();
    CHECK(!cap.has_error && str_has(S(cap.json), "https://a.test/"));

    call_as(caller, "history.search", "{}", &cap);
    CHECK(cap.has_error && strcmp(cap.code, "denied") == 0);
}

static void test_an_alias_carries_its_targets_grants(void) {
    NEED_PAGE_HOOKS();
    bdk_profile_t p = start_routes(BDK_NATIVE_PAGE_HANDLED);
    t_events_t ev;
    bdk_contents_t c = load_in_new_page(p, "conf://prefs", &ev);
    bdk_harness_caller_t caller = 0;
    bdk_error_t err = 0;
    REQUIRE(bdk_harness_page_bind(t_runtime, c, 0, &caller, &err) == BDK_OK);
    t_capture_t cap;
    call_as(caller, "runtime.caller", "{}", &cap);
    CHECK(str_has(S(cap.json), "\"name\":\"settings\""));
    CHECK(str_has(S(cap.json), "\"settings.write\""));
}

static void expect_bind_refused(bdk_contents_t c, int subframe, t_events_t* ev) {
    int before = ev->count;
    bdk_harness_caller_t caller = 0;
    bdk_error_t err = 0;
    CHECK(bdk_harness_page_bind(t_runtime, c, subframe, &caller, &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "capability", "denied"));
    t_pump();
    int gone = -1;
    for (int i = before; i < ev->count; ++i) {
        if (ev->kind[i] == BDK_CONTENTS_EVENT_RENDERER_GONE)
            gone = i;
    }
    CHECK(gone >= 0); /* the renderer that asked is terminated */
}

static void test_the_broker_is_refused_to_every_other_frame(void) {
    NEED_PAGE_HOOKS();
    bdk_profile_t p = start_routes(BDK_NATIVE_PAGE_HANDLED);
    REQUIRE(bdk_harness_script_navigation(t_runtime, "https://example.test/", 0, "Web") == BDK_OK);
    t_events_t web, builtin, sub;
    expect_bind_refused(load_in_new_page(p, "https://example.test/", &web), 0, &web);
    expect_bind_refused(load_in_new_page(p, "conf://history", &builtin), 0, &builtin);
    expect_bind_refused(load_in_new_page(p, "conf://settings", &sub), 1, &sub);
}

static void test_privilege_ends_when_the_page_navigates(void) {
    NEED_PAGE_HOOKS();
    bdk_profile_t p = start_routes(BDK_NATIVE_PAGE_HANDLED);
    t_events_t ev;
    bdk_contents_t c = load_in_new_page(p, "conf://settings", &ev);
    bdk_harness_caller_t caller = 0;
    bdk_error_t err = 0;
    REQUIRE(bdk_harness_page_bind(t_runtime, c, 0, &caller, &err) == BDK_OK);
    REQUIRE(bdk_harness_script_navigation(t_runtime, "https://example.test/", 0, "Web") == BDK_OK);
    REQUIRE(bdk_contents_load(c, S("https://example.test/"), &err) == BDK_OK);
    t_pump();
    t_capture_t cap;
    call_as(caller, "settings.get", "{\"key\":\"browser.homePage\"}", &cap);
    CHECK(cap.has_error && strcmp(cap.code, "denied") == 0);
}

typedef struct seen_events {
    int count, releases;
} seen_events_t;

static void on_event(void* ctx, uint32_t id, bdk_string_t name, bdk_string_t payload) {
    (void)id, (void)name, (void)payload;
    ++((seen_events_t*)ctx)->count;
}

static void on_event_release(void* ctx) {
    ++((seen_events_t*)ctx)->releases;
}

static void test_a_page_subscribes_only_to_what_it_holds(void) {
    NEED_PAGE_HOOKS();
    memset(&g_native, 0, sizeof g_native);
    t_start_with(kRoutes);
    bdk_error_t err = 0;
    REQUIRE(bdk_app_event_declare(t_runtime, S("app.appearance.changed"), S("{}"), &err) == BDK_OK);
    REQUIRE(bdk_app_event_declare(t_runtime, S("app.tabs.changed"), S("{}"), &err) == BDK_OK);
    REQUIRE(bdk_page_handler_register(t_runtime, S("downloads"), BDK_PAGE_MODE_NATIVE,
                                      on_native_page, &g_native, NULL, &err) == BDK_OK);
    t_freeze();
    bdk_profile_t p = t_new_profile("Subscriptions");
    t_events_t ev;
    bdk_contents_t c = load_in_new_page(p, "conf://settings", &ev);
    bdk_harness_caller_t caller = 0;
    REQUIRE(bdk_harness_page_bind(t_runtime, c, 0, &caller, &err) == BDK_OK);

    seen_events_t settings_seen = {0, 0}, app_seen = {0, 0};
    CHECK(bdk_harness_subscribe_as(t_runtime, caller, "settings.changed", on_event, &settings_seen,
                                   on_event_release, &err) != 0);
    CHECK(bdk_harness_subscribe_as(t_runtime, caller, "app.appearance.changed", on_event, &app_seen,
                                   on_event_release, &err) != 0);
    CHECK(bdk_harness_subscribe_as(t_runtime, caller, "history.visited", on_event, NULL, NULL,
                                   &err) == 0);
    CHECK(t_error_is(err, "capability", "denied"));
    CHECK(bdk_harness_subscribe_as(t_runtime, caller, "app.tabs.changed", on_event, NULL, NULL,
                                   &err) == 0);
    CHECK(t_error_is(err, "capability", "denied"));
    CHECK(bdk_harness_subscribe_as(t_runtime, caller, "no.such", on_event, NULL, NULL, &err) == 0);
    CHECK(t_error_is(err, "broker", "unknownEvent"));

    REQUIRE(bdk_app_event_emit(t_runtime, S("app.appearance.changed"), S("{}"), S(""), &err) ==
            BDK_OK);
    t_pump();
    CHECK(app_seen.count == 1);

    /* Navigating away ends the page's subscriptions. */
    REQUIRE(bdk_contents_load(c, S("conf://history"), &err) == BDK_OK);
    t_pump();
    CHECK(settings_seen.releases == 1 && app_seen.releases == 1);
    REQUIRE(bdk_app_event_emit(t_runtime, S("app.appearance.changed"), S("{}"), S(""), &err) ==
            BDK_OK);
    t_pump();
    CHECK(app_seen.count == 1);
}

/* ---- Page commands and controls (T-SWF-06) ---- */

static int can_perform(bdk_contents_t c, uint32_t command) {
    uint8_t enabled = 2;
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_can_perform_command(c, command, &enabled, &err) == BDK_OK);
    return enabled;
}

static void test_page_commands_follow_the_page(void) {
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Commands");
    REQUIRE(bdk_harness_script_navigation(t_runtime, "https://a.test/", 0, "A") == BDK_OK);
    REQUIRE(bdk_harness_script_navigation(t_runtime, "https://b.test/", 0, "B") == BDK_OK);
    t_events_t ev;
    bdk_contents_t c = load_in_new_page(p, "https://a.test/", &ev);
    CHECK(!can_perform(c, BDK_PAGE_COMMAND_BACK));
    CHECK(!can_perform(c, BDK_PAGE_COMMAND_FORWARD));
    CHECK(can_perform(c, BDK_PAGE_COMMAND_RELOAD));
    CHECK(!can_perform(c, BDK_PAGE_COMMAND_STOP));
    CHECK(!can_perform(c, BDK_PAGE_COMMAND_ZOOM_RESET));
    CHECK(can_perform(c, BDK_PAGE_COMMAND_ZOOM_IN));

    bdk_error_t err = 0;
    CHECK(bdk_contents_perform_command(c, BDK_PAGE_COMMAND_BACK, &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "contents", "commandDisabled"));

    REQUIRE(bdk_contents_load(c, S("https://b.test/"), &err) == BDK_OK);
    t_pump();
    CHECK(can_perform(c, BDK_PAGE_COMMAND_BACK));
    REQUIRE(bdk_contents_perform_command(c, BDK_PAGE_COMMAND_BACK, &err) == BDK_OK);
    t_pump();
    char url[128];
    current_url(c, url, sizeof url);
    CHECK(strcmp(url, "https://a.test/") == 0);
    CHECK(can_perform(c, BDK_PAGE_COMMAND_FORWARD));

    REQUIRE(bdk_contents_perform_command(c, BDK_PAGE_COMMAND_ZOOM_IN, &err) == BDK_OK);
    bdk_contents_state_t st;
    memset(&st, 0, sizeof st);
    st.struct_size = sizeof st;
    REQUIRE(bdk_contents_state(c, &st, &err) == BDK_OK);
    CHECK(st.zoom > 1.0);
    CHECK(can_perform(c, BDK_PAGE_COMMAND_ZOOM_RESET));
    REQUIRE(bdk_contents_perform_command(c, BDK_PAGE_COMMAND_ZOOM_RESET, &err) == BDK_OK);
    REQUIRE(bdk_contents_state(c, &st, &err) == BDK_OK);
    CHECK(st.zoom == 1.0);

    REQUIRE(bdk_contents_perform_command(c, BDK_PAGE_COMMAND_TOGGLE_MUTE, &err) == BDK_OK);
    REQUIRE(bdk_contents_state(c, &st, &err) == BDK_OK);
    CHECK((st.audio & BDK_AUDIO_MUTED) != 0);

    uint8_t enabled = 0;
    CHECK(bdk_contents_can_perform_command(c, 0, &enabled, &err) == BDK_ERR_INVALID_ARGUMENT);
    CHECK(bdk_contents_can_perform_command(c, 999, &enabled, &err) == BDK_ERR_INVALID_ARGUMENT);
    CHECK(bdk_contents_can_perform_command(c, BDK_PAGE_COMMAND_BACK, NULL, &err) ==
          BDK_ERR_INVALID_ARGUMENT);
    CHECK(bdk_contents_perform_command(c, 999, &err) == BDK_ERR_INVALID_ARGUMENT);
}

static void test_internal_pages_cannot_be_saved_or_viewed_as_source(void) {
    bdk_profile_t p = start_routes(BDK_NATIVE_PAGE_HANDLED);
    t_events_t ev;
    bdk_contents_t c = load_in_new_page(p, "conf://settings", &ev);
    CHECK(!can_perform(c, BDK_PAGE_COMMAND_SAVE_PAGE_AS));
    CHECK(!can_perform(c, BDK_PAGE_COMMAND_VIEW_SOURCE));
}

static int g_surface_calls[32];

static bdk_ui_response_t count_surface(void* ctx, const bdk_ui_request_info_t* info) {
    (void)ctx;
    if (info->surface < 32)
        ++g_surface_calls[info->surface];
    return BDK_UI_REJECT;
}

static void test_the_find_and_print_commands_raise_their_requests(void) {
    memset(g_surface_calls, 0, sizeof g_surface_calls);
    t_start();
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_FIND, count_surface, NULL, NULL, &err) ==
            BDK_OK);
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_PRINTING, count_surface, NULL, NULL, &err) ==
            BDK_OK);
    t_freeze();
    bdk_profile_t p = t_new_profile("Find");
    REQUIRE(bdk_harness_script_navigation(t_runtime, "https://a.test/", 0, "A") == BDK_OK);
    t_events_t ev;
    bdk_contents_t c = load_in_new_page(p, "https://a.test/", &ev);
    REQUIRE(bdk_contents_perform_command(c, BDK_PAGE_COMMAND_FIND, &err) == BDK_OK);
    REQUIRE(bdk_contents_perform_command(c, BDK_PAGE_COMMAND_PRINT, &err) == BDK_OK);
    t_pump();
    CHECK(g_surface_calls[BDK_SURFACE_FIND] == 1);
    CHECK(g_surface_calls[BDK_SURFACE_PRINTING] == 1);
}

static void test_zoom_has_a_page_and_a_site_scope(void) {
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Zoom");
    REQUIRE(bdk_harness_script_navigation(t_runtime, "https://a.test/one", 0, "1") == BDK_OK);
    REQUIRE(bdk_harness_script_navigation(t_runtime, "https://a.test/two", 0, "2") == BDK_OK);
    t_events_t e1, e2;
    bdk_contents_t one = load_in_new_page(p, "https://a.test/one", &e1);
    bdk_contents_t two = load_in_new_page(p, "https://a.test/two", &e2);
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_set_zoom_scoped(one, 1.5, BDK_ZOOM_SCOPE_PAGE, &err) == BDK_OK);
    bdk_contents_state_t st;
    memset(&st, 0, sizeof st);
    st.struct_size = sizeof st;
    REQUIRE(bdk_contents_state(two, &st, &err) == BDK_OK);
    CHECK(st.zoom == 1.0);
    REQUIRE(bdk_contents_set_zoom_scoped(one, 1.25, BDK_ZOOM_SCOPE_SITE, &err) == BDK_OK);
    t_pump();
    REQUIRE(bdk_contents_state(two, &st, &err) == BDK_OK);
    CHECK(st.zoom == 1.25);
    CHECK(bdk_contents_set_zoom_scoped(one, 1.0, 7, &err) == BDK_ERR_INVALID_ARGUMENT);
    CHECK(bdk_contents_set_zoom_scoped(one, 0.0, BDK_ZOOM_SCOPE_PAGE, &err) ==
          BDK_ERR_INVALID_ARGUMENT);
    CHECK(bdk_contents_exit_picture_in_picture(one, &err) == BDK_OK);
}

const t_test_t t_pages_tests[] = {
    {"pages.bundled_page_has_its_branded_url", test_a_bundled_page_is_reported_by_its_branded_url},
    {"pages.alias_shares_its_targets_page", test_an_alias_shares_its_targets_page},
    {"pages.built_in_page_has_its_branded_url",
     test_a_built_in_page_is_reported_by_its_branded_url},
    {"pages.disabled_page_shows_an_error", test_a_disabled_page_shows_an_error_page},
    {"pages.unknown_route_shows_an_error", test_an_unknown_route_shows_an_error_page},
    {"pages.no_internal_way_into_an_overridden_page",
     test_an_overridden_or_disabled_built_in_has_no_internal_way_in},
    {"pages.web_page_cannot_reach_a_branded_url", test_a_web_page_cannot_navigate_to_a_branded_url},
    {"pages.native_route_calls_its_handler",
     test_a_native_route_calls_its_handler_and_loads_nothing},
    {"pages.native_handler_can_block", test_a_native_handler_can_block_with_the_error_page},
    {"pages.native_handler_answers_once", test_a_native_handler_may_answer_later_and_only_once},
    {"pages.native_route_needs_a_handler", test_a_native_route_needs_a_handler_before_the_freeze},
    {"pages.registrations_are_checked", test_registrations_are_checked},
    {"pages.registration_outranks_the_table", test_a_registration_outranks_the_resolved_table},
    {"pages.registered_contexts_released_once", test_registered_contexts_are_released_once},
    {"pages.malformed_route_table_fails_start", test_a_malformed_route_table_fails_the_start},
    {"pages.no_scheme_reports_urls_as_they_are",
     test_without_a_scheme_urls_are_reported_as_they_are},
    {"pages.bundled_page_gets_its_capabilities", test_a_bundled_page_gets_its_routes_capabilities},
    {"pages.alias_carries_its_targets_grants", test_an_alias_carries_its_targets_grants},
    {"pages.broker_refused_to_other_frames", test_the_broker_is_refused_to_every_other_frame},
    {"pages.privilege_ends_on_navigation", test_privilege_ends_when_the_page_navigates},
    {"pages.page_subscribes_to_what_it_holds", test_a_page_subscribes_only_to_what_it_holds},
    {"pages.commands_follow_the_page", test_page_commands_follow_the_page},
    {"pages.internal_pages_not_savable", test_internal_pages_cannot_be_saved_or_viewed_as_source},
    {"pages.find_and_print_raise_requests", test_the_find_and_print_commands_raise_their_requests},
    {"pages.zoom_page_and_site_scope", test_zoom_has_a_page_and_a_site_scope},
    {NULL, NULL},
};
