#include "test_support.h"

#include <setjmp.h>
#include <stdlib.h>

static jmp_buf g_abort;
static int g_failures_in_test;
static const char* g_current;
bdk_runtime_t t_runtime;
static int g_seq;

void t_fail(const char* file, int line, const char* expr) {
    printf("    FAIL %s:%d: %s\n", file, line, expr);
    ++g_failures_in_test;
}
void t_abort(void) {
    longjmp(g_abort, 1);
}

bdk_string_t S(const char* s) {
    bdk_string_t v;
    v.data = s;
    v.length = s ? strlen(s) : 0;
    return v;
}
int str_is(bdk_string_t s, const char* expected) {
    size_t n = strlen(expected);
    return s.length == n && (n == 0 || (s.data && memcmp(s.data, expected, n) == 0));
}
int str_has(bdk_string_t s, const char* needle) {
    size_t n = strlen(needle);
    if (n == 0)
        return 1;
    for (size_t i = 0; s.data && i + n <= s.length; ++i) {
        if (memcmp(s.data + i, needle, n) == 0)
            return 1;
    }
    return 0;
}

const char* t_config_extra = NULL;

void t_fill_start_config(bdk_start_config_t* c) {
    static char resolved[2048];
    memset(c, 0, sizeof *c);
    c->struct_size = sizeof *c;
    snprintf(resolved, sizeof resolved, "{\"app\":{\"name\":\"conformance\"}%s%s}",
             t_config_extra ? "," : "", t_config_extra ? t_config_extra : "");
    c->resolved_config_json = S(resolved);
    c->bundle_path = S("/nonexistent/bundle");
    c->framework_path = S("/nonexistent/framework");
    c->host_abi_version = bdk_abi_version();
    c->host_abi_hash = bdk_abi_hash();
    bdk_harness_prepare_start_config(c);
}

void t_pump(void) {
    (void)bdk_harness_pump();
}

bdk_runtime_t t_start(void) {
    return t_start_with(NULL);
}

bdk_runtime_t t_start_with(const char* resolved_json) {
    bdk_start_config_t c;
    t_fill_start_config(&c);
    if (resolved_json) {
        c.resolved_config_json = S(resolved_json);
        bdk_harness_prepare_start_config(&c);
    }
    bdk_error_t err = 0;
    bdk_status_t s = bdk_runtime_start(&c, &t_runtime, &err);
    if (s != BDK_OK)
        t_runtime = 0;
    REQUIRE(s == BDK_OK);
    REQUIRE(t_runtime != 0);
    return t_runtime;
}

void t_freeze(void) {
    bdk_error_t err = 0;
    REQUIRE(bdk_runtime_freeze_registrations(t_runtime, &err) == BDK_OK);
    t_pump();
}

bdk_runtime_t t_start_frozen(void) {
    t_start();
    t_freeze();
    return t_runtime;
}

void t_stop(void) {
    t_capture_t cap;
    t_cap_init(&cap);
    REQUIRE(bdk_runtime_shutdown(t_runtime, t_on_result, &cap, t_on_release) != 0);
    t_pump();
    CHECK(cap.results == 1);
    CHECK(cap.releases == 1);
    CHECK(!cap.has_error);
    t_runtime = 0;
}

int t_next_seq(void) {
    return ++g_seq;
}

void t_cap_init(t_capture_t* c) {
    memset(c, 0, sizeof *c);
}

void t_on_result(void* ctx, bdk_error_t error, bdk_string_t json, bdk_buffer_t binary) {
    t_capture_t* c = (t_capture_t*)ctx;
    ++c->results;
    c->result_seq = t_next_seq();
    c->has_error = error != 0;
    c->has_binary = binary != 0;
    c->domain[0] = c->code[0] = c->message[0] = '\0';
    if (error) {
        bdk_string_t d = bdk_error_domain(error), k = bdk_error_code(error),
                     m = bdk_error_message(error);
        snprintf(c->domain, sizeof c->domain, "%.*s", (int)d.length, d.data ? d.data : "");
        snprintf(c->code, sizeof c->code, "%.*s", (int)k.length, k.data ? k.data : "");
        snprintf(c->message, sizeof c->message, "%.*s", (int)m.length, m.data ? m.data : "");
    }
    snprintf(c->json, sizeof c->json, "%.*s", (int)json.length, json.data ? json.data : "");
    if (c->on_result)
        c->on_result(c);
}
void t_on_progress(void* ctx, double fraction, bdk_string_t message) {
    t_capture_t* c = (t_capture_t*)ctx;
    (void)message;
    ++c->progress;
    c->fraction = fraction;
}
void t_on_release(void* ctx) {
    t_capture_t* c = (t_capture_t*)ctx;
    ++c->releases;
    c->release_seq = t_next_seq();
}

void t_error_text(bdk_error_t e, char* domain, size_t dn, char* code, size_t cn) {
    bdk_string_t d = bdk_error_domain(e), k = bdk_error_code(e);
    snprintf(domain, dn, "%.*s", (int)d.length, d.data ? d.data : "");
    snprintf(code, cn, "%.*s", (int)k.length, k.data ? k.data : "");
}
int t_error_is(bdk_error_t e, const char* domain, const char* code) {
    int ok = e != 0 && str_is(bdk_error_domain(e), domain) && str_is(bdk_error_code(e), code);
    if (e)
        bdk_error_release(e);
    return ok;
}

void t_ev_init(t_events_t* e) {
    memset(e, 0, sizeof *e);
}
void t_on_contents_event(void* ctx, const bdk_contents_event_t* ev) {
    t_events_t* e = (t_events_t*)ctx;
    if (e->count >= T_MAX_EVENTS)
        return;
    int i = e->count++;
    e->kind[i] = ev->kind;
    e->flags[i] = ev->flags;
    e->net_error[i] = ev->net_error;
    snprintf(e->url[i], sizeof e->url[i], "%.*s", (int)ev->url.length,
             ev->url.data ? ev->url.data : "");
    snprintf(e->detail[i], sizeof e->detail[i], "%.*s", (int)ev->detail_json.length,
             ev->detail_json.data ? ev->detail_json.data : "");
    if (getenv("BDK_TEST_TRACE")) /* a runtime's actual event order, for diagnosis */
        printf("    event %u %s %s %d 0x%x\n", ev->kind, e->url[i], e->detail[i], ev->net_error,
               ev->flags);
}
void t_on_event_release(void* ctx) {
    ++((t_events_t*)ctx)->releases;
}

int t_json_string(const char* json, const char* key, char* out, size_t n) {
    char pattern[64];
    snprintf(pattern, sizeof pattern, "\"%s\":\"", key);
    const char* p = strstr(json, pattern);
    if (!p)
        return 0;
    p += strlen(pattern);
    const char* q = strchr(p, '"');
    if (!q)
        return 0;
    snprintf(out, n, "%.*s", (int)(q - p), p);
    return 1;
}

void t_buffer_text(bdk_buffer_t b, char* out, size_t n) {
    bdk_bytes_t v = bdk_buffer_bytes(b);
    snprintf(out, n, "%.*s", (int)v.length, v.data ? (const char*)v.data : "");
}

void t_create_profile(const char* name, char* id_out, size_t n) {
    t_capture_t cap;
    t_cap_init(&cap);
    REQUIRE(bdk_profile_create(t_runtime, S(name), t_on_result, &cap, t_on_release) != 0);
    t_pump();
    REQUIRE(cap.results == 1 && !cap.has_error);
    REQUIRE(t_json_string(cap.json, "id", id_out, n));
}

bdk_profile_t t_open_profile(const char* id) {
    t_capture_t cap;
    t_cap_init(&cap);
    bdk_profile_t p = 0;
    REQUIRE(bdk_profile_open(t_runtime, S(id), &p, t_on_result, &cap, t_on_release) != 0);
    t_pump();
    REQUIRE(cap.results == 1 && !cap.has_error);
    REQUIRE(p != 0);
    return p;
}

bdk_profile_t t_new_profile(const char* name) {
    char id[64];
    t_create_profile(name, id, sizeof id);
    return t_open_profile(id);
}

bdk_contents_t t_new_contents(bdk_profile_t profile) {
    bdk_contents_options_t o;
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.profile = profile;
    bdk_contents_t c = 0;
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_create(t_runtime, &o, &c, &err) == BDK_OK);
    REQUIRE(c != 0);
    return c;
}

/* ---- runner ---- */

static const t_test_t* const kTables[] = {
    t_base_tests,
    t_runtime_tests,
    t_broker_tests,
    t_profile_tests,
    t_contents_tests,
    t_ui_tests,
    t_automation_tests,
    t_endpoint_tests,
    t_provider_tests, /* providers: contracts, configuration, translation, reputation
                         (tests_providers.c) */
    t_pages_tests,
#ifdef BDK_HARNESS_ENGINE_TESTS
    t_engine_tests,            /* scenarios only a real engine can run (tests_engine.c) */
    t_engine_ui_tests,         /* browser-owned surfaces on the engine (tests_engine_ui.c) */
    t_engine_prompt_tests,     /* services and prompts (tests_engine_prompts.c) */
    t_engine_automation_tests, /* automation and DevTools (tests_engine_automation.c) */
    t_engine_provider_tests,   /* providers on Chromium (tests_engine_providers.c) */
    t_engine_privacy_tests, /* the network audit and Google-free start-up (tests_engine_privacy.c)
                             */
    t_engine_pages_tests,   /* internal pages, search and M3 follow-ups (tests_engine_pages.c) */
#endif
};

static int run_one(const t_test_t* t) {
    g_failures_in_test = 0;
    g_current = t->name;
    if (setjmp(g_abort) == 0)
        t->fn();
    /* Leave the runtime stopped for the next test, whatever happened. */
    if (t_runtime) {
        t_capture_t cap;
        t_cap_init(&cap);
        (void)bdk_runtime_shutdown(t_runtime, t_on_result, &cap, t_on_release);
        t_pump();
        t_runtime = 0;
    }
    t_pump();
    t_config_extra = NULL;
    printf("%s %s\n", g_failures_in_test ? "[FAIL]" : "[ ok ]", t->name);
    return g_failures_in_test == 0;
}

/* `pattern` is a test name, or a prefix followed by '*' for every test that starts with it; a
   leading '!' selects every test that does not match the rest. */
static int t_name_matches(const char* pattern, const char* name) {
    if (pattern[0] == '!')
        return !t_name_matches(pattern + 1, name);
    size_t n = strlen(pattern);
    if (n > 0 && pattern[n - 1] == '*')
        return strncmp(pattern, name, n - 1) == 0;
    return strcmp(pattern, name) == 0;
}

/* A harness whose runtime must own the process's main thread first (an application event loop)
   defines BDK_HARNESS_OWNS_MAIN and calls this from its own main. */
#ifdef BDK_HARNESS_OWNS_MAIN
int bdk_conformance_main(int argc, char** argv) {
#else
int main(int argc, char** argv) {
#endif
    int list = 0;
    const char* only = NULL;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--list") == 0)
            list = 1;
        else if (strcmp(argv[i], "--run") == 0 && i + 1 < argc)
            only = argv[++i];
        else {
            fprintf(stderr, "usage: %s [--list] [--run <test, prefix* or !pattern>]\n", argv[0]);
            return 2;
        }
    }
    int total = 0, failed = 0;
    for (size_t k = 0; k < sizeof kTables / sizeof kTables[0]; ++k) {
        for (const t_test_t* t = kTables[k]; t->name; ++t) {
            if (list) {
                printf("%s\n", t->name);
                continue;
            }
            if (only && !t_name_matches(only, t->name))
                continue;
            ++total;
            if (!run_one(t))
                ++failed;
        }
    }
    if (list)
        return 0;
    if (only && total == 0) {
        fprintf(stderr, "no test named %s\n", only);
        return 2;
    }
    printf("%d tests, %d passed, %d failed\n", total, total - failed, failed);
    (void)g_current;
    return failed ? 1 : 0;
}
