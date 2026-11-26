/* Providers on Chromium (T-PRV-02 to T-PRV-05, T-PRV-09): a real page translated through the
   application's provider and through the default presenter, translation preferences kept in
   Chromium's translate preferences, Chromium's Safe Browsing interstitial raised by BDK's lists
   and by the application's reputation provider, and a page's position from the application's
   geolocation provider. Built only into the engine conformance app (BDK_HARNESS_ENGINE_TESTS).
   Pages are served by the engine's test interceptor; default presenters are scripted through
   bdk_testing_command. The provider contract itself is in tests_providers.c, which runs here too.
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bdk_testing.h"
#include "test_support.h"

/* From harness_engine.mm. */
int bdk_harness_engine_window_open(bdk_runtime_t rt);
void bdk_harness_engine_window_attach(int window, bdk_contents_t contents);
void bdk_harness_engine_window_close(int window);

/* ---- helpers ---- */

static void control(const char* verb, const char* args, char* out, size_t n) {
    REQUIRE(bdk_testing_command(t_runtime, S(verb), S(args), out, n) == BDK_OK);
}

static int g_window = -1;

static void close_window(void) {
    if (g_window >= 0)
        bdk_harness_engine_window_close(g_window);
    g_window = -1;
}

/* A page in a window, focused, as a person's page would be. */
static bdk_contents_t new_active_page(bdk_profile_t profile) {
    bdk_contents_t c = t_new_contents(profile);
    g_window = bdk_harness_engine_window_open(t_runtime);
    REQUIRE(g_window >= 0);
    bdk_harness_engine_window_attach(g_window, c);
    REQUIRE(bdk_contents_focus(c) == BDK_OK);
    t_pump();
    return c;
}

static void script_page(const char* url, const char* html) {
    static char args[4096];
    size_t n = (size_t)snprintf(args, sizeof args,
                                "{\"url\":\"%s\",\"contentType\":\"text/html\",\"headers\":\"\","
                                "\"body\":\"",
                                url);
    for (const char* c = html; *c && n + 8 < sizeof args; ++c) {
        if (*c == '"' || *c == '\\')
            args[n++] = '\\';
        args[n++] = *c;
    }
    snprintf(args + n, sizeof args - n, "\"}");
    control("script_page", args, NULL, 0);
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
    char title[256] = "";
    for (int i = 0; i < 80; ++i) {
        title_of(c, title, sizeof title);
        if (strcmp(title, want) == 0)
            return 1;
        t_pump();
        bdk_testing_run_for(0.1);
    }
    printf("    title is \"%s\", not \"%s\"\n", title, want);
    return 0;
}

static void load(bdk_contents_t c, const char* url) {
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_load(c, S(url), &err) == BDK_OK);
    t_pump();
}

/* Writes the paragraphs' text into the title, from the page's own world: what a person sees. */
static void show_paragraphs(bdk_contents_t c) {
    REQUIRE(bdk_testing_execute_script(c,
                                       S("document.title = [...document.querySelectorAll('p')]"
                                         ".map(p => p.textContent).join('|')"),
                                       0) == BDK_OK);
    t_pump();
}

static void invoke(const char* command, const char* args, bdk_profile_t profile,
                   bdk_contents_t contents, t_capture_t* cap) {
    bdk_invoke_options_t o;
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.profile = profile;
    o.contents = contents;
    t_cap_init(cap);
    REQUIRE(bdk_invoke_named(t_runtime, S(command), S(args), &o, t_on_result, NULL, cap,
                             t_on_release) != 0);
    for (int i = 0; i < 100 && cap->results == 0; ++i) {
        t_pump();
        bdk_testing_run_for(0.05);
    }
    REQUIRE(cap->results == 1);
}

/* Waits until translation.state reports the phase. */
static int wait_for_phase(bdk_contents_t c, const char* phase) {
    char want[48];
    snprintf(want, sizeof want, "\"phase\":\"%s\"", phase);
    t_capture_t cap;
    /* When it misses (T-ADP-10), how long it waited, start-up's state and the page's. */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < 60; ++i) {
        invoke("translation.state", "{}", 0, c, &cap);
        if (strstr(cap.json, want))
            return 1;
        bdk_testing_run_for(0.1);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("    translation state is %s after %.2fs\n", cap.json,
           (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9);
    char out[512];
    control("startup.state", "{}", out, sizeof out);
    bdk_contents_state_t st;
    memset(&st, 0, sizeof st);
    st.struct_size = sizeof st;
    bdk_error_t err = 0;
    bdk_contents_state(c, &st, &err);
    printf("    start-up %s; page %.*s \"%.*s\" loading %d visibility %u\n", out,
           (int)st.url.length, st.url.data ? st.url.data : "", (int)st.title.length,
           st.title.data ? st.title.data : "", st.loading, st.visibility);
    return 0;
}

/* Starts with the harness's resolved configuration plus `extra` members. */
static bdk_status_t start_with(const char* extra, bdk_error_t* err) {
    bdk_start_config_t c;
    t_fill_start_config(&c);
    const bdk_string_t base = c.resolved_config_json;
    REQUIRE(base.length > 1 && base.data[base.length - 1] == '}');
    static char resolved[2048];
    snprintf(resolved, sizeof resolved, "%.*s%s%s}", (int)(base.length - 1), base.data,
             extra[0] ? "," : "", extra);
    c.resolved_config_json = S(resolved);
    return bdk_runtime_start(&c, &t_runtime, err);
}

/* ---- a fake provider for every service ---- */

static int g_translates, g_checks, g_starts;
static const char* g_risky_host;

static void fake_pairs(void* ctx, bdk_context_t call, bdk_string_t input) {
    (void)ctx, (void)input;
    bdk_provider_resolve(call, S("{\"pairs\":[{\"from\":\"fr\",\"to\":\"en\"}]}"));
}

static void fake_translate(void* ctx, bdk_context_t call, bdk_string_t input) {
    (void)ctx;
    ++g_translates;
    char text[4096];
    snprintf(text, sizeof text, "%.*s", (int)input.length, input.data);
    char* start = strstr(text, "\"texts\":[");
    char* end = start ? strchr(start, ']') : NULL;
    REQUIRE(start && end);
    start += 8;
    end[1] = '\0';
    for (char* p = start; *p; ++p)
        *p = (char)toupper((unsigned char)*p);
    char out[4200];
    snprintf(out, sizeof out, "{\"texts\":%s}", start);
    bdk_provider_resolve(call, S(out));
}

static void fake_check_url(void* ctx, bdk_context_t call, bdk_string_t input) {
    (void)ctx;
    ++g_checks;
    bdk_provider_resolve(call, S(g_risky_host && str_has(input, g_risky_host)
                                     ? "{\"verdict\":\"malware\"}"
                                     : "{\"verdict\":\"safe\"}"));
}

static void fake_start(void* ctx, bdk_context_t call, bdk_string_t input) {
    (void)ctx, (void)input;
    ++g_starts;
    bdk_provider_report(call, S("{\"latitude\":51.5,\"longitude\":-0.125,\"accuracy\":10}"));
}

static void register_fake(const char* service, const char* descriptor) {
    bdk_provider_vtable_t v;
    memset(&v, 0, sizeof v);
    v.struct_size = sizeof v;
    v.descriptor_json = S(descriptor ? descriptor
                                     : "{\"id\":\"app.fake\",\"name\":\"Fake\",\"privacy\":"
                                       "\"onDevice\"}");
    v.supported_pairs = fake_pairs;
    v.translate = fake_translate;
    v.check_url = fake_check_url;
    v.start = fake_start;
    bdk_error_t err = 0;
    REQUIRE(bdk_provider_register(t_runtime, S(service), &v, NULL, NULL, &err) == BDK_OK);
}

typedef struct {
    int calls;
    bdk_request_t request;
    char payload[1024];
} seen_t;

static bdk_ui_response_t hold(void* ctx, const bdk_ui_request_info_t* info) {
    seen_t* seen = (seen_t*)ctx;
    ++seen->calls;
    seen->request = info->request;
    snprintf(seen->payload, sizeof seen->payload, "%.*s", (int)info->payload_json.length,
             info->payload_json.data ? info->payload_json.data : "");
    return BDK_UI_HANDLED;
}

static int wait_for_request(seen_t* seen, int calls) {
    for (int i = 0; i < 60 && seen->calls < calls; ++i) {
        t_pump();
        bdk_testing_run_for(0.1);
    }
    return seen->calls >= calls;
}

#define FRENCH_PAGE                                                                                \
    "<html lang=\"fr\"><head><title>page</title></head><body><p>bonjour</p>"                       \
    "<p translate=\"no\">garde</p><p>le monde</p></body></html>"

/* ---- translation (T-PRV-02, T-PRV-03) ---- */

static void test_engine_a_real_page_is_translated_through_the_app_provider(void) {
    g_translates = 0;
    bdk_error_t err = 0;
    REQUIRE(start_with("\"services\":{\"translation\":{\"provider\":\"app\"}}", &err) == BDK_OK);
    register_fake("translation", NULL);
    t_freeze();
    bdk_contents_t c = new_active_page(t_new_profile("Translation"));
    script_page("https://fr.example/", FRENCH_PAGE);
    load(c, "https://fr.example/");
    REQUIRE(wait_for_phase(c, "available"));
    t_capture_t cap;
    invoke("translation.translate", "{}", 0, c, &cap);
    CHECK(!cap.has_error && strstr(cap.json, "\"phase\":\"translated\""));
    CHECK(g_translates == 1);
    show_paragraphs(c);
    /* Text marked translate="no" is left alone, and no markup is added. */
    CHECK(wait_for_title(c, "BONJOUR|garde|LE MONDE"));
    invoke("translation.revert", "{}", 0, c, &cap);
    show_paragraphs(c);
    CHECK(wait_for_title(c, "bonjour|garde|le monde"));
    /* A new page has no translation. */
    script_page("https://fr.example/next", FRENCH_PAGE);
    load(c, "https://fr.example/next");
    invoke("translation.state", "{}", 0, c, &cap);
    CHECK(!strstr(cap.json, "\"phase\":\"translated\""));
    close_window();
}

static void test_engine_the_offer_with_no_handler_is_the_default_sheet(void) {
    bdk_error_t err = 0;
    REQUIRE(start_with("\"services\":{\"translation\":{\"provider\":\"app\"}}", &err) == BDK_OK);
    register_fake("translation", NULL);
    t_freeze();
    control("present.script", "{\"surface\":\"translation\",\"answer\":{\"answer\":\"translate\"}}",
            NULL, 0);
    bdk_contents_t c = new_active_page(t_new_profile("Translation"));
    script_page("https://fr.example/", FRENCH_PAGE);
    load(c, "https://fr.example/");
    REQUIRE(wait_for_phase(c, "translated"));
    char out[2048];
    control("present.record", "{\"surface\":\"translation\"}", out, sizeof out);
    CHECK(strstr(out, "\"shown\":1") != NULL);
    show_paragraphs(c);
    CHECK(wait_for_title(c, "BONJOUR|garde|LE MONDE"));
    control("present.clear", "{}", NULL, 0);
    close_window();
}

static void test_engine_never_from_the_request_persists_in_chromium_prefs(void) {
    bdk_error_t err = 0;
    REQUIRE(start_with("\"services\":{\"translation\":{\"provider\":\"app\"}}", &err) == BDK_OK);
    register_fake("translation", NULL);
    seen_t seen;
    memset(&seen, 0, sizeof seen);
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_TRANSLATION, hold, &seen, NULL, &err) ==
            BDK_OK);
    t_freeze();
    bdk_profile_t p = t_new_profile("Translation");
    bdk_contents_t c = new_active_page(p);
    script_page("https://fr.example/", FRENCH_PAGE);
    load(c, "https://fr.example/");
    REQUIRE(wait_for_request(&seen, 1));
    CHECK(strstr(seen.payload, "\"phase\":\"offer\"") &&
          strstr(seen.payload, "\"detected\":\"fr\""));
    bdk_native_handle_t none;
    memset(&none, 0, sizeof none);
    CHECK(bdk_ui_complete(seen.request, S("never"), S("{}"), none, &err) == BDK_OK);
    t_pump();
    t_capture_t cap;
    invoke("translation.preferences", "{}", p, 0, &cap);
    CHECK(strstr(cap.json, "\"fr\"") != NULL);
    char out[512];
    char args[128];
    snprintf(args, sizeof args, "{\"contents\":\"%llu\",\"name\":\"translate_blocked_languages\"}",
             (unsigned long long)c);
    control("pref.json", args, out, sizeof out);
    CHECK(strstr(out, "fr") != NULL);
    /* French is not offered again. */
    script_page("https://fr.example/again", FRENCH_PAGE);
    load(c, "https://fr.example/again");
    bdk_testing_run_for(1);
    t_pump();
    CHECK(seen.calls == 1);
    close_window();
}

/* ---- reputation (T-PRV-05) ---- */

static void test_engine_the_default_lists_raise_chromium_s_interstitial(void) {
    bdk_error_t err = 0;
    REQUIRE(start_with("", &err) == BDK_OK);
    seen_t seen;
    memset(&seen, 0, sizeof seen);
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_REPUTATION_WARNINGS, hold, &seen, NULL,
                               &err) == BDK_OK);
    t_freeze();
    char out[256];
    control("reputation.list",
            "{\"json\":\"{\\\"format\\\":\\\"bdk-reputation-list\\\",\\\"version\\\":1,"
            "\\\"sequence\\\":1,\\\"generated\\\":1700000000000,\\\"expires\\\":4102444800000,"
            "\\\"source\\\":\\\"test\\\",\\\"entries\\\":[{\\\"expression\\\":"
            "\\\"phish.example/\\\",\\\"verdict\\\":\\\"phishing\\\"}]}\"}",
            out, sizeof out);
    REQUIRE(strstr(out, "\"loaded\":true") != NULL);
    bdk_contents_t c = new_active_page(t_new_profile("Reputation"));
    script_page("https://safe.example/", "<title>safe</title>");
    load(c, "https://safe.example/");
    CHECK(wait_for_title(c, "safe"));
    CHECK(seen.calls == 0);
    script_page("https://www.phish.example/login", "<title>phish</title>");
    load(c, "https://www.phish.example/login");
    REQUIRE(wait_for_request(&seen, 1));
    CHECK(strstr(seen.payload, "\"verdict\":\"phishing\"") &&
          strstr(seen.payload, "\"protection\":\"reduced\"") &&
          strstr(seen.payload, "reduced protection"));
    close_window();
}

static void test_engine_the_app_reputation_provider_raises_the_interstitial(void) {
    g_checks = 0;
    g_risky_host = "malware.example";
    bdk_error_t err = 0;
    REQUIRE(start_with("\"services\":{\"reputation\":{\"provider\":\"app\"}}", &err) == BDK_OK);
    register_fake("reputation", "{\"id\":\"app.rep\",\"name\":\"Rep\",\"privacy\":\"onDevice\"}");
    seen_t seen;
    memset(&seen, 0, sizeof seen);
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_REPUTATION_WARNINGS, hold, &seen, NULL,
                               &err) == BDK_OK);
    t_freeze();
    bdk_contents_t c = new_active_page(t_new_profile("Reputation"));
    script_page("https://malware.example/", "<title>bad</title>");
    load(c, "https://malware.example/");
    REQUIRE(wait_for_request(&seen, 1));
    CHECK(g_checks >= 1);
    CHECK(strstr(seen.payload, "\"verdict\":\"malware\"") &&
          strstr(seen.payload, "\"id\":\"app.rep\""));
    g_risky_host = NULL;
    close_window();
}

/* ---- geolocation (T-PRV-04) ---- */

static void test_engine_a_page_gets_its_position_from_the_app_provider(void) {
    g_starts = 0;
    bdk_error_t err = 0;
    REQUIRE(start_with("\"services\":{\"geolocation\":{\"provider\":\"app\"}}", &err) == BDK_OK);
    register_fake("geolocation", NULL);
    t_freeze();
    control("present.script", "{\"surface\":\"permissions\",\"answer\":{\"choice\":\"allow\"}}",
            NULL, 0);
    bdk_contents_t c = new_active_page(t_new_profile("Geolocation"));
    script_page("https://geo.example/", "<title>geo</title>");
    load(c, "https://geo.example/");
    REQUIRE(bdk_testing_execute_script(c,
                                       S("navigator.geolocation.getCurrentPosition("
                                         "p => { document.title = String(p.coords.latitude); },"
                                         "e => { document.title = 'error ' + e.code; })"),
                                       1) == BDK_OK);
    CHECK(wait_for_title(c, "51.5"));
    CHECK(g_starts == 1);
    control("present.clear", "{}", NULL, 0);
    close_window();
}

/* ---- the Google key (T-PRV-09) ---- */

static void test_engine_no_key_reaches_chromium_without_a_google_provider(void) {
    t_start_frozen();
    char out[512];
    control("providers.settings", "{}", out, sizeof out);
    CHECK(strstr(out, "\"googleKey\":false") != NULL);
}

/* ---- the application's Google key (a launch of its own) ---- */

#define KEY_LAUNCH "engine_launch_chromium_starts_with_the_application_s_google_key"
#define KEY_CONFIG "\"services\":{\"geolocation\":{\"provider\":\"google\"}}"
#define KEY_VALUE "AIza-bdk-test-key"

static int g_key_prepared;

/* Called by the harness before its first start: Chromium takes a key once per process, from the
   start that opens it, so the scenario runs alone (scripts/test-engine.sh). */
void bdk_engine_google_key_launch_prepare(const char* name) {
    if (strcmp(name, KEY_LAUNCH) != 0)
        return;
    t_config_extra = KEY_CONFIG;
    t_first_start_google_key = KEY_VALUE;
    g_key_prepared = 1;
}

/* A start with a Google provider gives Chromium the application's key once Chromium's features
   exist; before them, Chromium's key cache stops the process (it reads a feature). */
static void test_launch_chromium_starts_with_the_application_s_google_key(void) {
    if (!g_key_prepared) {
        printf("    nothing prepared: run alone (scripts/test-engine.sh does)\n");
        return;
    }
    bdk_error_t err = 0;
    t_config_extra = KEY_CONFIG;
    bdk_start_config_t config;
    t_fill_start_config(&config);
    t_config_extra = NULL;
    config.google_api_key = S(KEY_VALUE);
    REQUIRE(bdk_runtime_start(&config, &t_runtime, &err) == BDK_OK);
    t_freeze();
    char out[512];
    control("providers.settings", "{}", out, sizeof out);
    CHECK(strstr(out, "\"googleKey\":true") != NULL);
    CHECK(strstr(out, "\"geolocation\":\"google\"") != NULL);
}

const t_test_t t_engine_provider_tests[] = {
    {KEY_LAUNCH, test_launch_chromium_starts_with_the_application_s_google_key},
    {"engine_a_real_page_is_translated_through_the_app_provider",
     test_engine_a_real_page_is_translated_through_the_app_provider},
    {"engine_the_offer_with_no_handler_is_the_default_sheet",
     test_engine_the_offer_with_no_handler_is_the_default_sheet},
    {"engine_never_from_the_request_persists_in_chromium_prefs",
     test_engine_never_from_the_request_persists_in_chromium_prefs},
    {"engine_the_default_lists_raise_chromium_s_interstitial",
     test_engine_the_default_lists_raise_chromium_s_interstitial},
    {"engine_the_app_reputation_provider_raises_the_interstitial",
     test_engine_the_app_reputation_provider_raises_the_interstitial},
    {"engine_a_page_gets_its_position_from_the_app_provider",
     test_engine_a_page_gets_its_position_from_the_app_provider},
    {"engine_no_key_reaches_chromium_without_a_google_provider",
     test_engine_no_key_reaches_chromium_without_a_google_provider},
    {NULL, NULL},
};
