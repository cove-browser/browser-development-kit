/* Providers (T-PRV-01, T-PRV-09): choosing a provider in the resolved configuration, registering an
 * application provider, the contract its answers must meet, and the translation, reputation and
 * geolocation behaviour built on it. The tests that need a page with text, a block list or a
 * geolocation watch use tests/harness_providers.h and are skipped by a runtime without those hooks;
 * the engine runs the same behaviour on real pages in tests_engine_providers.c. */
#include <ctype.h>
#include <stdlib.h>

#include "harness_providers.h"
#include "test_support.h"

/* ---- A fake provider, one for every service ------------------------------------------------ */

typedef struct fake {
    int pairs, prepares, translates, detects, cancels, checks, starts, released;
    int hold;               /* translate keeps the call instead of answering */
    bdk_context_t held;     /* the call it keeps */
    const char* pairs_json; /* supported_pairs answers this; NULL for fr->en and de->en */
    int bad_answer;         /* translate answers one text too few */
    int foreign_rejection;  /* translate rejects with a domain that is not the provider's */
    const char* language;   /* detect answers this; NULL answers null */
    const char* risky_host; /* check_url answers `verdict` for a URL containing this */
    const char* verdict;
    bdk_context_t watch;
} fake_t;

static fake_t F;

static void fake_reset(void) {
    memset(&F, 0, sizeof F);
}

static void fake_cancel(void* ctx, bdk_context_t call, bdk_string_t input) {
    (void)ctx, (void)call, (void)input;
    ++F.cancels;
}

static void fake_pairs(void* ctx, bdk_context_t call, bdk_string_t input) {
    (void)ctx, (void)input;
    ++F.pairs;
    bdk_provider_resolve(call, S(F.pairs_json ? F.pairs_json
                                              : "{\"pairs\":[{\"from\":\"fr\",\"to\":\"en\"},"
                                                "{\"from\":\"de\",\"to\":\"en\",\"installed\":"
                                                "false}]}"));
}

static void fake_prepare(void* ctx, bdk_context_t call, bdk_string_t input) {
    (void)ctx, (void)input;
    ++F.prepares;
    bdk_provider_progress(call, 0.5);
    bdk_provider_resolve(call, S("null"));
}

/* Answers the texts upper-cased. The tests use plain ASCII texts, so upper-casing the array's
 * characters keeps it JSON. */
static void fake_translate(void* ctx, bdk_context_t call, bdk_string_t input) {
    (void)ctx;
    ++F.translates;
    if (F.hold) {
        F.held = call;
        return;
    }
    if (F.foreign_rejection) {
        bdk_provider_reject(call, S("broker"), S("timeout"), S("upstream timed out"));
        return;
    }
    char text[4096];
    snprintf(text, sizeof text, "%.*s", (int)input.length, input.data);
    char* start = strstr(text, "\"texts\":[");
    char* end = start ? strchr(start, ']') : NULL;
    if (!start || !end) {
        bdk_provider_reject(call, S("provider"), S("remoteError"), S("no texts"));
        return;
    }
    start += 8;
    end[1] = '\0';
    for (char* p = start; *p; ++p)
        *p = (char)toupper((unsigned char)*p);
    if (F.bad_answer) {
        char* last = strrchr(start, ',');
        if (last) {
            last[0] = ']';
            last[1] = '\0';
        }
    }
    char out[4200];
    snprintf(out, sizeof out, "{\"texts\":%s}", start);
    bdk_provider_resolve(call, S(out));
}

static void fake_detect(void* ctx, bdk_context_t call, bdk_string_t input) {
    (void)ctx, (void)input;
    ++F.detects;
    char out[96];
    if (F.language)
        snprintf(out, sizeof out, "{\"language\":\"%s\",\"reliable\":true}", F.language);
    bdk_provider_resolve(call, S(F.language ? out : "null"));
}

static void fake_check_url(void* ctx, bdk_context_t call, bdk_string_t input) {
    (void)ctx;
    ++F.checks;
    char out[96];
    snprintf(out, sizeof out, "{\"verdict\":\"%s\"}",
             F.risky_host && str_has(input, F.risky_host) ? F.verdict : "safe");
    bdk_provider_resolve(call, S(out));
}

static void fake_start(void* ctx, bdk_context_t call, bdk_string_t input) {
    (void)ctx, (void)input;
    ++F.starts;
    F.watch = call;
    bdk_provider_report(call, S("{\"latitude\":51.5,\"longitude\":-0.12,\"accuracy\":10}"));
}

static void fake_released(void* ctx) {
    (void)ctx;
    ++F.released;
}

static bdk_provider_vtable_t vtable(const char* descriptor) {
    bdk_provider_vtable_t v;
    memset(&v, 0, sizeof v);
    v.struct_size = sizeof v;
    v.descriptor_json = S(descriptor ? descriptor
                                     : "{\"id\":\"app.fake\",\"name\":\"Fake\",\"privacy\":"
                                       "\"onDevice\"}");
    v.cancel = fake_cancel;
    v.supported_pairs = fake_pairs;
    v.prepare = fake_prepare;
    v.translate = fake_translate;
    v.detect = fake_detect;
    v.check_url = fake_check_url;
    v.start = fake_start;
    return v;
}

/* ---- Starting with a [services] table ----------------------------------------------------- */

/* Starts with the harness's resolved configuration plus `extra` (members of the root object, such
 * as "services":{...}), and the Google key. */
static bdk_status_t start_with(const char* extra, const char* key, bdk_error_t* err) {
    bdk_start_config_t c;
    t_fill_start_config(&c);
    const bdk_string_t base = c.resolved_config_json;
    REQUIRE(base.length > 1 && base.data[base.length - 1] == '}');
    static char resolved[2048];
    snprintf(resolved, sizeof resolved, "%.*s%s%s}", (int)(base.length - 1), base.data,
             base.length > 2 && extra[0] ? "," : "", extra);
    c.resolved_config_json = S(resolved);
    c.google_api_key = S(key ? key : "");
    return bdk_runtime_start(&c, &t_runtime, err);
}

static void start_app_providers(const char* extra) {
    fake_reset();
    bdk_error_t err = 0;
    REQUIRE(start_with(extra, NULL, &err) == BDK_OK);
}

static const char* kAllApp = "\"services\":{\"translation\":{\"provider\":\"app\"},"
                             "\"reputation\":{\"provider\":\"app\"},"
                             "\"geolocation\":{\"provider\":\"app\"}}";

/* Starts with every service configured "app", registers the fake for each and freezes. */
static void start_with_fakes(const char* extra) {
    start_app_providers(extra ? extra : kAllApp);
    bdk_error_t err = 0;
    bdk_provider_vtable_t v = vtable(NULL);
    REQUIRE(bdk_provider_register(t_runtime, S("translation"), &v, NULL, fake_released, &err) ==
            BDK_OK);
    if (!extra) {
        REQUIRE(bdk_provider_register(t_runtime, S("reputation"), &v, NULL, fake_released, &err) ==
                BDK_OK);
        REQUIRE(bdk_provider_register(t_runtime, S("geolocation"), &v, NULL, fake_released, &err) ==
                BDK_OK);
    }
    t_freeze();
}

static bdk_status_t start_fails(const char* extra, const char* key, const char* domain,
                                const char* code) {
    bdk_error_t err = 0;
    bdk_status_t s = start_with(extra, key, &err);
    if (s == BDK_OK) {
        t_stop();
        return s;
    }
    CHECK(s == BDK_ERR_FAILED);
    CHECK(t_error_is(err, domain, code));
    t_runtime = 0;
    return s;
}

static bdk_call_t invoke(const char* name, const char* args, bdk_profile_t profile,
                         bdk_contents_t contents, t_capture_t* cap) {
    bdk_invoke_options_t o;
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.profile = profile;
    o.contents = contents;
    t_cap_init(cap);
    return bdk_invoke_named(t_runtime, S(name), S(args), &o, t_on_result, t_on_progress, cap,
                            t_on_release);
}

static int failed_with(const t_capture_t* cap, const char* domain, const char* code) {
    return cap->results == 1 && cap->has_error && strcmp(cap->domain, domain) == 0 &&
           strcmp(cap->code, code) == 0;
}

/* ---- Configuration (T-PRV-09) ------------------------------------------------------------- */

static void test_default_providers_need_nothing_registered(void) {
    bdk_error_t err = 0;
    REQUIRE(start_with("", NULL, &err) == BDK_OK);
    t_freeze();
    bdk_profile_t p = t_new_profile("Providers");
    t_capture_t cap;
    REQUIRE(invoke("translation.provider", "{}", p, 0, &cap) != 0);
    t_pump();
    CHECK(cap.results == 1 && !cap.has_error);
    CHECK(str_has(S(cap.json), "\"id\":\"default\"") &&
          str_has(S(cap.json), "\"privacy\":\"onDevice\""));
}

static void test_a_provider_bdk_does_not_know_or_has_not_built_fails_the_start(void) {
    CHECK(start_fails("\"services\":{\"translation\":{\"provider\":\"bogus\"}}", NULL, "config",
                      "resolvedInvalid") == BDK_ERR_FAILED);
    CHECK(start_fails("\"services\":{\"translation\":{\"provider\":\"deepl\"}}", NULL, "config",
                      "providerUnavailable") == BDK_ERR_FAILED);
    CHECK(start_fails("\"services\":{\"reputation\":{\"provider\":\"web-risk\"}}", NULL, "config",
                      "providerUnavailable") == BDK_ERR_FAILED);
    /* The credential and leak-check providers (S14, S16) are proposals: only their defaults run. */
    CHECK(start_fails("\"services\":{\"leak_check\":{\"provider\":\"app\"}}", NULL, "config",
                      "providerUnavailable") == BDK_ERR_FAILED);
    CHECK(start_fails("\"services\":{\"geolocation\":{\"provider\":7}}", NULL, "config",
                      "resolvedInvalid") == BDK_ERR_FAILED);
}

static void test_only_the_default_reputation_lists_take_an_endpoint(void) {
    CHECK(start_fails("\"services\":{\"geolocation\":{\"provider\":\"system\",\"endpoint\":"
                      "\"https://geo.example.com\"}}",
                      NULL, "config", "endpointUnsupported") == BDK_ERR_FAILED);
    CHECK(start_fails("\"services\":{\"translation\":{\"provider\":\"app\",\"endpoint\":"
                      "\"https://mt.example.com\"}}",
                      NULL, "config", "endpointUnsupported") == BDK_ERR_FAILED);
    CHECK(start_fails("\"services\":{\"reputation\":{\"provider\":\"default\",\"endpoint\":"
                      "\"http://lists.example.com\"}}",
                      NULL, "config", "resolvedInvalid") == BDK_ERR_FAILED);
    bdk_error_t err = 0;
    CHECK(start_with("\"services\":{\"reputation\":{\"provider\":\"default\",\"endpoint\":"
                     "\"https://lists.example.com/bdk\"}}",
                     NULL, &err) == BDK_OK);
}

static void test_the_google_key_is_given_only_with_a_google_provider(void) {
    CHECK(start_fails("\"services\":{\"geolocation\":{\"provider\":\"google\"}}", NULL, "config",
                      "keyMissing") == BDK_ERR_FAILED);
    CHECK(start_fails("", "AIzaTestKey", "config", "keyUnused") == BDK_ERR_FAILED);
    /* Chromium takes the key once per process, before it starts (google_api_keys.cc), so only a
       runtime with no Chromium starts with one here; tests_engine_providers.c covers the engine. */
    if (!bdk_harness_provider_hooks())
        return;
    bdk_error_t err = 0;
    CHECK(start_with("\"services\":{\"geolocation\":{\"provider\":\"google\"}}", "AIzaTestKey",
                     &err) == BDK_OK);
}

static void test_an_app_service_needs_its_provider_by_the_freeze(void) {
    start_app_providers("\"services\":{\"translation\":{\"provider\":\"app\"},\"geolocation\":{"
                        "\"provider\":\"app\"}}");
    bdk_error_t err = 0;
    CHECK(bdk_runtime_freeze_registrations(t_runtime, &err) == BDK_ERR_FAILED);
    char domain[32], code[48];
    t_error_text(err, domain, sizeof domain, code, sizeof code);
    CHECK(strcmp(code, "providerMissing") == 0);
    bdk_provider_vtable_t v = vtable(NULL);
    CHECK(bdk_provider_register(t_runtime, S("translation"), &v, NULL, fake_released, &err) ==
          BDK_OK);
    CHECK(bdk_runtime_freeze_registrations(t_runtime, &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "config", "providerMissing"));
    CHECK(bdk_provider_register(t_runtime, S("geolocation"), &v, NULL, fake_released, &err) ==
          BDK_OK);
    CHECK(bdk_runtime_freeze_registrations(t_runtime, &err) == BDK_OK);
    CHECK(bdk_provider_register(t_runtime, S("reputation"), &v, NULL, NULL, &err) ==
          BDK_ERR_FAILED);
    CHECK(t_error_is(err, "config", "registrationAfterStart"));
    t_stop();
    CHECK(F.released == 2);
}

static void test_registration_checks_the_service_the_operations_and_the_descriptor(void) {
    start_app_providers("\"services\":{\"translation\":{\"provider\":\"app\"}}");
    bdk_error_t err = 0;
    bdk_provider_vtable_t v = vtable(NULL);
    CHECK(bdk_provider_register(t_runtime, S("reputation"), &v, NULL, NULL, &err) ==
          BDK_ERR_FAILED);
    CHECK(t_error_is(err, "config", "providerNotApp"));
    CHECK(bdk_provider_register(t_runtime, S("credentials"), &v, NULL, NULL, &err) ==
          BDK_ERR_INVALID_ARGUMENT);
    CHECK(bdk_provider_register(t_runtime, S("translation"), NULL, NULL, NULL, &err) ==
          BDK_ERR_INVALID_ARGUMENT);
    bdk_provider_vtable_t missing = v;
    missing.translate = NULL;
    CHECK(bdk_provider_register(t_runtime, S("translation"), &missing, NULL, NULL, &err) ==
          BDK_ERR_INVALID_ARGUMENT);
    bdk_provider_vtable_t remote =
        vtable("{\"id\":\"app.cloud\",\"name\":\"Cloud\",\"privacy\":\"remote\","
               "\"host\":\"mt.example.com\"}");
    CHECK(bdk_provider_register(t_runtime, S("translation"), &remote, NULL, NULL, &err) ==
          BDK_ERR_FAILED);
    CHECK(t_error_is(err, "config", "descriptorInvalid"));
    bdk_provider_vtable_t unknown = vtable("{\"id\":\"a\",\"name\":\"b\",\"privacy\":\"onDevice\","
                                           "\"colour\":\"red\"}");
    CHECK(bdk_provider_register(t_runtime, S("translation"), &unknown, NULL, NULL, &err) ==
          BDK_ERR_FAILED);
    CHECK(t_error_is(err, "config", "descriptorInvalid"));
    CHECK(F.released == 0); /* a refused registration keeps nothing */
    CHECK(bdk_provider_register(t_runtime, S("translation"), &v, NULL, fake_released, &err) ==
          BDK_OK);
    CHECK(bdk_provider_register(t_runtime, S("translation"), &v, NULL, fake_released, &err) ==
          BDK_ERR_FAILED);
    CHECK(t_error_is(err, "config", "duplicateName"));
    CHECK(bdk_provider_set_key(t_runtime, S("translation"), S("k"), &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "config", "keyUnused"));
    t_freeze();
    t_stop();
    CHECK(F.released == 1);
}

/* ---- The contract (T-PRV-01) --------------------------------------------------------------- */

static void test_the_app_provider_answers_through_the_contract(void) {
    start_with_fakes(NULL);
    bdk_profile_t p = t_new_profile("Providers");
    t_capture_t cap;
    REQUIRE(invoke("translation.provider", "{}", p, 0, &cap) != 0);
    t_pump();
    CHECK(str_has(S(cap.json), "\"id\":\"app.fake\""));
    REQUIRE(invoke("translation.languages", "{}", p, 0, &cap) != 0);
    CHECK(F.pairs == 0); /* never from the call that caused it */
    t_pump();
    CHECK(cap.results == 1 && !cap.has_error && F.pairs == 1);
    /* An omitted "installed" is true. */
    CHECK(str_has(S(cap.json), "{\"from\":\"fr\",\"to\":\"en\",\"installed\":true}"));
    CHECK(str_has(S(cap.json), "{\"from\":\"de\",\"to\":\"en\",\"installed\":false}"));
}

static void test_an_answer_that_breaks_the_contract_is_refused(void) {
    start_with_fakes(NULL);
    bdk_profile_t p = t_new_profile("Providers");
    t_capture_t cap;
    F.pairs_json = "{\"pairs\":[{\"from\":\"fr\",\"to\":\"fr\"}]}";
    REQUIRE(invoke("translation.languages", "{}", p, 0, &cap) != 0);
    t_pump();
    CHECK(failed_with(&cap, "provider", "contractViolation"));
    F.pairs_json = "{\"pairs\":[{\"from\":\"french\",\"to\":\"en\"}]}";
    REQUIRE(invoke("translation.languages", "{}", p, 0, &cap) != 0);
    t_pump();
    CHECK(failed_with(&cap, "provider", "contractViolation"));
    F.pairs_json = "[1,2]";
    REQUIRE(invoke("translation.languages", "{}", p, 0, &cap) != 0);
    t_pump();
    CHECK(failed_with(&cap, "provider", "contractViolation"));
}

static void test_the_default_translation_provider_is_not_in_this_runtime(void) {
    bdk_error_t err = 0;
    REQUIRE(start_with("", NULL, &err) == BDK_OK);
    t_freeze();
    bdk_profile_t p = t_new_profile("Providers");
    t_capture_t cap;
    REQUIRE(invoke("translation.languages", "{}", p, 0, &cap) != 0);
    t_pump();
    CHECK(failed_with(&cap, "provider", "unavailable"));
}

static void test_cancelling_a_call_tells_the_provider_and_ignores_its_answer(void) {
    start_with_fakes(NULL);
    if (!bdk_harness_provider_hooks())
        return;
    bdk_profile_t p = t_new_profile("Providers");
    bdk_contents_t c = t_new_contents(p);
    REQUIRE(bdk_harness_page_text_set(c, "fr", "[\"bonjour\"]") == BDK_OK);
    t_pump();
    F.hold = 1;
    t_capture_t cap;
    bdk_call_t call = invoke("translation.translate", "{}", 0, c, &cap);
    REQUIRE(call != 0);
    t_pump();
    REQUIRE(F.held != 0);
    CHECK(bdk_provider_is_cancelled(F.held) == 0);
    CHECK(bdk_call_cancel(call) == BDK_OK);
    t_pump();
    CHECK(failed_with(&cap, "broker", "cancelled"));
    CHECK(F.cancels == 1);
    CHECK(bdk_provider_is_cancelled(F.held) == 1);
    bdk_provider_resolve(F.held, S("{\"texts\":[\"BONJOUR\"]}")); /* ignored */
    t_pump();
    char text[128];
    REQUIRE(bdk_harness_page_text(c, text, sizeof text) == BDK_OK);
    CHECK(strcmp(text, "[\"bonjour\"]") == 0);
}

static void test_a_call_outstanding_at_shutdown_is_cancelled_before_release(void) {
    start_with_fakes(NULL);
    if (!bdk_harness_provider_hooks())
        return;
    bdk_contents_t c = t_new_contents(t_new_profile("Providers"));
    REQUIRE(bdk_harness_page_text_set(c, "fr", "[\"bonjour\"]") == BDK_OK);
    t_pump();
    F.hold = 1;
    t_capture_t cap;
    REQUIRE(invoke("translation.translate", "{}", 0, c, &cap) != 0);
    t_pump();
    REQUIRE(F.held != 0);
    t_stop();
    CHECK(F.cancels == 1 && F.released == 3);
    CHECK(failed_with(&cap, "broker", "cancelled"));
}

/* ---- Translation preferences and models (T-PRV-03) ---------------------------------------- */

static int g_prefs_events;
static char g_prefs_payload[256];
static void on_prefs_event(void* ctx, uint32_t id, bdk_string_t name, bdk_string_t payload) {
    (void)ctx, (void)id, (void)name;
    ++g_prefs_events;
    snprintf(g_prefs_payload, sizeof g_prefs_payload, "%.*s", (int)payload.length, payload.data);
}

static void test_preferences_are_kept_per_profile_and_reported(void) {
    start_with_fakes(NULL);
    bdk_profile_t p = t_new_profile("Providers");
    g_prefs_events = 0;
    bdk_subscription_t sub = bdk_subscribe(t_runtime, BDK_EVENT_TRANSLATION_PREFERENCES_CHANGED,
                                           S(""), NULL, on_prefs_event, NULL, NULL);
    REQUIRE(sub != 0);
    t_capture_t cap;
    REQUIRE(invoke("translation.setPreferences",
                   "{\"target\":\"de\",\"neverTranslate\":[\"fr\"],\"alwaysTranslate\":[\"es\"]}",
                   p, 0, &cap) != 0);
    t_pump();
    CHECK(cap.results == 1 && !cap.has_error);
    CHECK(str_has(S(cap.json), "\"target\":\"de\""));
    CHECK(str_has(S(cap.json), "\"neverTranslate\":[\"fr\"]"));
    CHECK(str_has(S(cap.json), "\"alwaysTranslate\":[\"es\"]"));
    CHECK(g_prefs_events == 1 && str_has(S(g_prefs_payload), "\"target\":\"de\""));
    /* A language is on one list only: putting it on the always list takes it off the never list. */
    REQUIRE(invoke("translation.setPreferences", "{\"alwaysTranslate\":[\"fr\"]}", p, 0, &cap) !=
            0);
    t_pump();
    CHECK(str_has(S(cap.json), "\"neverTranslate\":[]"));
    REQUIRE(invoke("translation.setPreferences", "{\"target\":\"not a language\"}", p, 0, &cap) !=
            0);
    t_pump();
    CHECK(failed_with(&cap, "broker", "invalidArguments"));
    REQUIRE(invoke("translation.preferences", "{}", p, 0, &cap) != 0);
    t_pump();
    CHECK(str_has(S(cap.json), "\"target\":\"de\"") &&
          str_has(S(cap.json), "\"alwaysTranslate\":[\"fr\"]"));
    /* Another profile has its own. */
    bdk_profile_t other = t_new_profile("Other");
    REQUIRE(invoke("translation.preferences", "{}", other, 0, &cap) != 0);
    t_pump();
    CHECK(!str_has(S(cap.json), "\"target\":\"de\""));
    bdk_unsubscribe(sub);
}

static int g_models_events;
static void on_models_event(void* ctx, uint32_t id, bdk_string_t name, bdk_string_t payload) {
    (void)ctx, (void)id, (void)name, (void)payload;
    ++g_models_events;
}

static void test_downloading_a_model_reports_progress_and_the_new_models(void) {
    start_with_fakes(NULL);
    bdk_profile_t p = t_new_profile("Providers");
    g_models_events = 0;
    bdk_subscription_t sub = bdk_subscribe(t_runtime, BDK_EVENT_TRANSLATION_MODELS_CHANGED, S(""),
                                           NULL, on_models_event, NULL, NULL);
    t_capture_t cap;
    REQUIRE(invoke("translation.downloadModel", "{\"from\":\"de\",\"to\":\"en\"}", p, 0, &cap) !=
            0);
    t_pump();
    CHECK(cap.results == 1 && !cap.has_error && F.prepares == 1);
    CHECK(cap.progress >= 1 && cap.fraction == 0.5);
    CHECK(g_models_events == 1);
    /* The fake has no remove_model: the provider cannot do it. */
    REQUIRE(invoke("translation.removeModel", "{\"from\":\"de\",\"to\":\"en\"}", p, 0, &cap) != 0);
    t_pump();
    CHECK(failed_with(&cap, "provider", "unavailable"));
    bdk_unsubscribe(sub);
}

static void test_a_disabled_translation_surface_turns_the_service_off(void) {
    start_app_providers("\"services\":{\"translation\":{\"provider\":\"app\"}},"
                        "\"surfaces\":{\"translation\":\"disabled\"}");
    bdk_error_t err = 0;
    bdk_provider_vtable_t v = vtable(NULL);
    REQUIRE(bdk_provider_register(t_runtime, S("translation"), &v, NULL, NULL, &err) == BDK_OK);
    t_freeze();
    bdk_profile_t p = t_new_profile("Providers");
    t_capture_t cap;
    REQUIRE(invoke("translation.languages", "{}", p, 0, &cap) != 0);
    t_pump();
    CHECK(failed_with(&cap, "provider", "unavailable"));
    CHECK(F.pairs == 0);
}

/* ---- Translating a page (stub hooks; tests_engine_providers.c on real pages) --------------- */

static int g_tr_calls;
static bdk_request_t g_tr_request;
static char g_tr_payload[512];
static bdk_ui_response_t tr_handler(void* ctx, const bdk_ui_request_info_t* info) {
    (void)ctx;
    ++g_tr_calls;
    g_tr_request = info->request;
    snprintf(g_tr_payload, sizeof g_tr_payload, "%.*s", (int)info->payload_json.length,
             info->payload_json.data);
    return BDK_UI_HANDLED;
}

static bdk_profile_t g_tr_profile;

/* A page, with the fake as the translation provider and, if asked, a translation handler. */
static bdk_contents_t translation_page(int handler) {
    g_tr_calls = 0;
    g_tr_request = 0;
    start_app_providers("\"services\":{\"translation\":{\"provider\":\"app\"}}");
    bdk_error_t err = 0;
    bdk_provider_vtable_t v = vtable(NULL);
    REQUIRE(bdk_provider_register(t_runtime, S("translation"), &v, NULL, NULL, &err) == BDK_OK);
    if (handler)
        REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_TRANSLATION, tr_handler, NULL, NULL,
                                   &err) == BDK_OK);
    t_freeze();
    g_tr_profile = t_new_profile("Translation");
    return t_new_contents(g_tr_profile);
}

static void page_text(bdk_contents_t c, char* out, size_t n) {
    REQUIRE(bdk_harness_page_text(c, out, n) == BDK_OK);
}

static void test_a_page_is_translated_through_the_app_provider(void) {
    if (!bdk_harness_provider_hooks())
        return;
    bdk_contents_t c = translation_page(0);
    t_events_t ev;
    t_ev_init(&ev);
    REQUIRE(bdk_contents_subscribe(c, t_on_contents_event, &ev, t_on_event_release) != 0);
    REQUIRE(bdk_harness_page_text_set(c, "fr", "[\"bonjour\",\"  \",\"le monde\"]") == BDK_OK);
    t_pump();
    t_capture_t cap;
    REQUIRE(invoke("translation.state", "{}", 0, c, &cap) != 0);
    t_pump();
    CHECK(str_has(S(cap.json), "\"phase\":\"available\"") &&
          str_has(S(cap.json), "\"detected\":\"fr\""));
    REQUIRE(invoke("translation.translate", "{}", 0, c, &cap) != 0);
    t_pump();
    CHECK(cap.results == 1 && !cap.has_error);
    CHECK(str_has(S(cap.json), "\"phase\":\"translated\"") &&
          str_has(S(cap.json), "\"from\":\"fr\"") && str_has(S(cap.json), "\"to\":\"en\""));
    CHECK(cap.progress >= 1);
    char text[256];
    page_text(c, text, sizeof text);
    /* Blank segments are not sent and come back as they were. */
    CHECK(strcmp(text, "[\"BONJOUR\",\"  \",\"LE MONDE\"]") == 0);
    CHECK(F.translates == 1);
    int translation_events = 0;
    for (int i = 0; i < ev.count; ++i)
        translation_events += ev.kind[i] == BDK_CONTENTS_EVENT_TRANSLATION;
    CHECK(translation_events >= 3); /* available, translating, translated */
    REQUIRE(invoke("translation.revert", "{}", 0, c, &cap) != 0);
    t_pump();
    page_text(c, text, sizeof text);
    CHECK(strcmp(text, "[\"bonjour\",\"  \",\"le monde\"]") == 0);
    REQUIRE(invoke("translation.state", "{}", 0, c, &cap) != 0);
    t_pump();
    CHECK(str_has(S(cap.json), "\"phase\":\"available\""));
}

static void test_the_offer_reaches_the_handler_and_its_answer_translates(void) {
    if (!bdk_harness_provider_hooks())
        return;
    bdk_contents_t c = translation_page(1);
    REQUIRE(bdk_harness_page_text_set(c, "fr", "[\"bonjour\"]") == BDK_OK);
    t_pump();
    REQUIRE(g_tr_calls == 1);
    CHECK(str_has(S(g_tr_payload), "\"phase\":\"offer\"") &&
          str_has(S(g_tr_payload), "\"detected\":\"fr\"") &&
          str_has(S(g_tr_payload), "\"target\":\"en\"") &&
          str_has(S(g_tr_payload), "\"id\":\"app.fake\""));
    bdk_native_handle_t none;
    memset(&none, 0, sizeof none);
    bdk_error_t err = 0;
    CHECK(bdk_ui_complete(g_tr_request, S("translate"), S("{}"), none, &err) == BDK_OK);
    t_pump();
    char text[128];
    page_text(c, text, sizeof text);
    CHECK(strcmp(text, "[\"BONJOUR\"]") == 0);
    /* present() asks again, in the page's current phase. */
    t_capture_t cap;
    REQUIRE(invoke("translation.present", "{}", 0, c, &cap) != 0);
    t_pump();
    CHECK(g_tr_calls == 2 && str_has(S(g_tr_payload), "\"phase\":\"translated\""));
    CHECK(bdk_ui_complete(g_tr_request, S("revert"), S("{}"), none, &err) == BDK_OK);
    t_pump();
    page_text(c, text, sizeof text);
    CHECK(strcmp(text, "[\"bonjour\"]") == 0);
}

static void test_never_and_always_decide_without_asking(void) {
    if (!bdk_harness_provider_hooks())
        return;
    bdk_contents_t c = translation_page(1);
    REQUIRE(bdk_harness_page_text_set(c, "fr", "[\"bonjour\"]") == BDK_OK);
    t_pump();
    REQUIRE(g_tr_calls == 1);
    bdk_native_handle_t none;
    memset(&none, 0, sizeof none);
    bdk_error_t err = 0;
    CHECK(bdk_ui_complete(g_tr_request, S("never"), S("{}"), none, &err) == BDK_OK);
    t_pump();
    /* French is never offered again. */
    REQUIRE(bdk_harness_page_text_set(c, "fr", "[\"salut\"]") == BDK_OK);
    t_pump();
    CHECK(g_tr_calls == 1);
    /* German on the always list is translated with no request. */
    t_capture_t cap;
    bdk_profile_t p = g_tr_profile;
    REQUIRE(invoke("translation.setPreferences", "{\"alwaysTranslate\":[\"de\"]}", p, 0, &cap) !=
            0);
    t_pump();
    REQUIRE(bdk_harness_page_text_set(c, "de", "[\"hallo\"]") == BDK_OK);
    t_pump();
    CHECK(g_tr_calls == 1);
    char text[128];
    page_text(c, text, sizeof text);
    CHECK(strcmp(text, "[\"HALLO\"]") == 0);
    CHECK(F.prepares == 1); /* de->en is not installed: the provider prepares it first */
    /* A page in the target language is not offered. */
    REQUIRE(bdk_harness_page_text_set(c, "en-GB", "[\"hello\"]") == BDK_OK);
    t_pump();
    CHECK(g_tr_calls == 1);
}

static void test_the_provider_detects_an_undeclared_language(void) {
    if (!bdk_harness_provider_hooks())
        return;
    bdk_contents_t c = translation_page(1);
    F.language = "fr";
    REQUIRE(bdk_harness_page_text_set(c, "", "[\"bonjour\"]") == BDK_OK);
    t_pump();
    CHECK(F.detects == 1 && g_tr_calls == 1 && str_has(S(g_tr_payload), "\"detected\":\"fr\""));
    /* Not detected: unavailable, and nothing is offered. */
    F.language = NULL;
    REQUIRE(bdk_harness_page_text_set(c, "", "[\"???\"]") == BDK_OK);
    t_pump();
    CHECK(g_tr_calls == 1);
    t_capture_t cap;
    REQUIRE(invoke("translation.state", "{}", 0, c, &cap) != 0);
    t_pump();
    CHECK(str_has(S(cap.json), "\"phase\":\"unavailable\""));
}

static void test_a_long_page_goes_in_batches_and_a_bad_batch_fails_it(void) {
    if (!bdk_harness_provider_hooks())
        return;
    bdk_contents_t c = translation_page(0);
    char texts[2048] = "[";
    for (int i = 0; i < 150; ++i)
        strcat(texts, i ? ",\"ab\"" : "\"ab\"");
    strcat(texts, "]");
    REQUIRE(bdk_harness_page_text_set(c, "fr", texts) == BDK_OK);
    t_pump();
    t_capture_t cap;
    REQUIRE(invoke("translation.translate", "{}", 0, c, &cap) != 0);
    t_pump();
    CHECK(cap.results == 1 && !cap.has_error);
    CHECK(F.translates == 3); /* 64 + 64 + 22 */
    F.bad_answer = 1;
    REQUIRE(invoke("translation.translate", "{}", 0, c, &cap) != 0);
    t_pump();
    CHECK(failed_with(&cap, "provider", "contractViolation"));
    REQUIRE(invoke("translation.state", "{}", 0, c, &cap) != 0);
    t_pump();
    CHECK(str_has(S(cap.json), "\"phase\":\"failed\"") &&
          str_has(S(cap.json), "\"code\":\"contractViolation\""));
    F.bad_answer = 0;
    F.foreign_rejection = 1;
    REQUIRE(invoke("translation.translate", "{}", 0, c, &cap) != 0);
    t_pump();
    CHECK(failed_with(&cap, "provider", "remoteError"));
    CHECK(str_has(S(cap.message), "broker.timeout"));
}

static void test_a_pair_the_provider_does_not_offer_is_refused(void) {
    if (!bdk_harness_provider_hooks())
        return;
    bdk_contents_t c = translation_page(0);
    REQUIRE(bdk_harness_page_text_set(c, "ja", "[\"konnichiwa\"]") == BDK_OK);
    t_pump();
    t_capture_t cap;
    REQUIRE(invoke("translation.translate", "{}", 0, c, &cap) != 0);
    t_pump();
    CHECK(failed_with(&cap, "provider", "unsupportedPair"));
    CHECK(F.translates == 0);
    /* No page named: the command needs one. */
    REQUIRE(invoke("translation.translate", "{}", g_tr_profile, 0, &cap) != 0);
    t_pump();
    CHECK(failed_with(&cap, "broker", "invalidArguments"));
}

static void test_navigating_away_cancels_the_translation(void) {
    if (!bdk_harness_provider_hooks())
        return;
    bdk_contents_t c = translation_page(0);
    REQUIRE(bdk_harness_page_text_set(c, "fr", "[\"bonjour\"]") == BDK_OK);
    t_pump();
    F.hold = 1;
    t_capture_t cap;
    REQUIRE(invoke("translation.translate", "{}", 0, c, &cap) != 0);
    t_pump();
    REQUIRE(F.held != 0);
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_load(c, S("https://elsewhere.test/"), &err) == BDK_OK);
    t_pump();
    CHECK(F.cancels == 1);
    CHECK(failed_with(&cap, "broker", "cancelled"));
    REQUIRE(invoke("translation.state", "{}", 0, c, &cap) != 0);
    t_pump();
    CHECK(str_has(S(cap.json), "\"phase\":\"unavailable\""));
}

/* ---- Reputation (T-PRV-05) ----------------------------------------------------------------- */

static int g_rep_calls;
static bdk_ui_response_t g_rep_answer; /* reject goes back, so the navigation ends blocked */
static bdk_request_t g_rep_request;
static char g_rep_payload[640];
static bdk_ui_response_t rep_handler(void* ctx, const bdk_ui_request_info_t* info) {
    (void)ctx;
    ++g_rep_calls;
    g_rep_request = info->request;
    snprintf(g_rep_payload, sizeof g_rep_payload, "%.*s", (int)info->payload_json.length,
             info->payload_json.data);
    return g_rep_answer;
}

static const char* list_json(int sequence, int expired) {
    static char list[512];
    snprintf(list, sizeof list,
             "{\"format\":\"bdk-reputation-list\",\"version\":1,\"sequence\":%d,"
             "\"generated\":1700000000000,\"expires\":%s,\"source\":\"test\",\"entries\":["
             "{\"expression\":\"evil.test/\",\"verdict\":\"phishing\"},"
             "{\"expression\":\"files.example.test/bad/\",\"verdict\":\"malware\"}]}",
             sequence, expired ? "1700000000001" : "4102444800000");
    return list;
}

static int navigate(bdk_contents_t c, const char* url) {
    t_events_t ev;
    t_ev_init(&ev);
    bdk_subscription_t sub = bdk_contents_subscribe(c, t_on_contents_event, &ev, NULL);
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_load(c, S(url), &err) == BDK_OK);
    t_pump();
    bdk_unsubscribe(sub);
    for (int i = 0; i < ev.count; ++i) {
        if (ev.kind[i] == BDK_CONTENTS_EVENT_NAVIGATION_COMMITTED)
            return 1;
        if (ev.kind[i] == BDK_CONTENTS_EVENT_NAVIGATION_FAILED &&
            str_has(S(ev.detail[i]), "blocked"))
            return 0;
    }
    return -1;
}

static void test_the_default_lists_block_a_listed_url_with_a_warning(void) {
    if (!bdk_harness_provider_hooks())
        return;
    g_rep_calls = 0;
    g_rep_answer = BDK_UI_REJECT;
    bdk_error_t err = 0;
    REQUIRE(start_with("", NULL, &err) == BDK_OK);
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_REPUTATION_WARNINGS, rep_handler, NULL, NULL,
                               &err) == BDK_OK);
    t_freeze();
    bdk_contents_t c = t_new_contents(t_new_profile("Reputation"));
    /* No list yet: nothing is blocked. */
    CHECK(navigate(c, "https://evil.test/login") == 1);
    REQUIRE(bdk_harness_reputation_list_load(t_runtime, list_json(1, 0), &err) == BDK_OK);
    CHECK(navigate(c, "https://www.evil.test/login?next=1") == 0);
    REQUIRE(g_rep_calls == 1);
    CHECK(str_has(S(g_rep_payload), "\"verdict\":\"phishing\"") &&
          str_has(S(g_rep_payload), "\"protection\":\"reduced\"") &&
          str_has(S(g_rep_payload), "reduced protection"));
    CHECK(navigate(c, "https://files.example.test/bad/a/b.exe") == 0);
    CHECK(str_has(S(g_rep_payload), "\"verdict\":\"malware\""));
    CHECK(navigate(c, "https://files.example.test/good/b.exe") == 1);
    CHECK(navigate(c, "https://notevil.test/") == 1);
    CHECK(g_rep_calls == 2);
}

static void test_proceeding_past_a_warning_needs_a_gesture(void) {
    if (!bdk_harness_provider_hooks())
        return;
    g_rep_calls = 0;
    g_rep_answer = BDK_UI_HANDLED;
    bdk_error_t err = 0;
    REQUIRE(start_with("", NULL, &err) == BDK_OK);
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_REPUTATION_WARNINGS, rep_handler, NULL, NULL,
                               &err) == BDK_OK);
    t_freeze();
    bdk_contents_t c = t_new_contents(t_new_profile("Reputation"));
    REQUIRE(bdk_harness_reputation_list_load(t_runtime, list_json(1, 0), &err) == BDK_OK);
    t_events_t ev;
    t_ev_init(&ev);
    REQUIRE(bdk_contents_subscribe(c, t_on_contents_event, &ev, NULL) != 0);
    REQUIRE(bdk_contents_load(c, S("https://evil.test/"), &err) == BDK_OK);
    t_pump();
    REQUIRE(g_rep_calls == 1);
    bdk_native_handle_t none, gesture;
    memset(&none, 0, sizeof none);
    memset(&gesture, 0, sizeof gesture);
    gesture.kind = BDK_NATIVE_NSEVENT;
    gesture.id = 7;
    CHECK(bdk_ui_complete(g_rep_request, S("proceed"), S("{}"), none, &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "ui", "gestureRequired"));
    CHECK(bdk_ui_complete(g_rep_request, S("proceed"), S("{}"), gesture, &err) == BDK_OK);
    t_pump();
    int committed = 0;
    for (int i = 0; i < ev.count; ++i)
        committed += ev.kind[i] == BDK_CONTENTS_EVENT_NAVIGATION_COMMITTED;
    CHECK(committed == 1);
}

static void test_a_list_must_be_well_formed_newer_and_current(void) {
    if (!bdk_harness_provider_hooks())
        return;
    bdk_error_t err = 0;
    REQUIRE(start_with("", NULL, &err) == BDK_OK);
    t_freeze();
    bdk_contents_t c = t_new_contents(t_new_profile("Reputation"));
    CHECK(bdk_harness_reputation_list_load(t_runtime, "{\"format\":\"other\"}", &err) ==
          BDK_ERR_FAILED);
    CHECK(t_error_is(err, "provider", "invalidList"));
    CHECK(bdk_harness_reputation_list_load(
              t_runtime,
              "{\"format\":\"bdk-reputation-list\",\"version\":1,\"sequence\":1,\"generated\":1,"
              "\"expires\":2,\"source\":\"t\",\"entries\":[{\"expression\":\"Evil.test/\","
              "\"verdict\":\"phishing\"}]}",
              &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "provider", "invalidList"));
    /* An expired list protects nothing (and says so: its verdict is unknown). */
    REQUIRE(bdk_harness_reputation_list_load(t_runtime, list_json(1, 1), &err) == BDK_OK);
    CHECK(navigate(c, "https://evil.test/") == 1);
    CHECK(bdk_harness_reputation_list_load(t_runtime, list_json(1, 0), &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "provider", "invalidList"));
    REQUIRE(bdk_harness_reputation_list_load(t_runtime, list_json(2, 0), &err) == BDK_OK);
    CHECK(navigate(c, "https://evil.test/") == 0);
}

static void test_the_app_reputation_provider_decides_the_warning(void) {
    if (!bdk_harness_provider_hooks())
        return;
    g_rep_calls = 0;
    g_rep_answer = BDK_UI_REJECT;
    start_app_providers("\"services\":{\"reputation\":{\"provider\":\"app\"}}");
    bdk_error_t err = 0;
    bdk_provider_vtable_t v =
        vtable("{\"id\":\"app.rep\",\"name\":\"Rep\",\"privacy\":\"remote\","
               "\"host\":\"rep.example.com\",\"policyUrl\":"
               "\"https://rep.example.com/privacy\",\"protection\":\"full\"}");
    REQUIRE(bdk_provider_register(t_runtime, S("reputation"), &v, NULL, NULL, &err) == BDK_OK);
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_REPUTATION_WARNINGS, rep_handler, NULL, NULL,
                               &err) == BDK_OK);
    t_freeze();
    F.risky_host = "malware.test";
    F.verdict = "malware";
    bdk_contents_t c = t_new_contents(t_new_profile("Reputation"));
    CHECK(navigate(c, "https://fine.test/") == 1);
    CHECK(navigate(c, "https://malware.test/") == 0);
    CHECK(F.checks == 2 && g_rep_calls == 1);
    CHECK(str_has(S(g_rep_payload), "\"id\":\"app.rep\"") &&
          !str_has(S(g_rep_payload), "reduced protection"));
    /* A verdict outside the contract lets the page load, as a failed check does. */
    F.verdict = "dangerousDownload";
    CHECK(navigate(c, "https://malware.test/") == 1);
}

/* ---- Geolocation (T-PRV-04) ---------------------------------------------------------------- */

static void test_the_app_geolocation_provider_gives_the_page_its_position(void) {
    if (!bdk_harness_provider_hooks())
        return;
    start_app_providers("\"services\":{\"geolocation\":{\"provider\":\"app\"}}");
    bdk_error_t err = 0;
    bdk_provider_vtable_t v = vtable(NULL);
    REQUIRE(bdk_provider_register(t_runtime, S("geolocation"), &v, NULL, NULL, &err) == BDK_OK);
    t_freeze();
    bdk_contents_t c = t_new_contents(t_new_profile("Geolocation"));
    REQUIRE(bdk_harness_geolocation_watch(c, 1) == BDK_OK);
    t_pump();
    char last[256];
    REQUIRE(bdk_harness_geolocation_last(c, last, sizeof last) == BDK_OK);
    CHECK(F.starts == 1 && str_has(S(last), "\"latitude\":51.5"));
    bdk_provider_report(F.watch, S("{\"latitude\":52,\"longitude\":0.1,\"accuracy\":5}"));
    t_pump();
    REQUIRE(bdk_harness_geolocation_last(c, last, sizeof last) == BDK_OK);
    CHECK(str_has(S(last), "\"latitude\":52"));
    /* A position out of range ends the watch rather than reach the page. */
    bdk_provider_report(F.watch, S("{\"latitude\":91,\"longitude\":0,\"accuracy\":5}"));
    t_pump();
    REQUIRE(bdk_harness_geolocation_last(c, last, sizeof last) == BDK_OK);
    CHECK(str_has(S(last), "\"error\":\"positionUnavailable\""));
    CHECK(bdk_provider_is_cancelled(F.watch) == 1);
    /* Stopping a watch tells the provider. */
    REQUIRE(bdk_harness_geolocation_watch(c, 0) == BDK_OK);
    t_pump();
    REQUIRE(bdk_harness_geolocation_stop(c) == BDK_OK);
    t_pump();
    CHECK(F.cancels == 1);
}

const t_test_t t_provider_tests[] = {
    {"providers.default_providers_need_nothing_registered",
     test_default_providers_need_nothing_registered},
    {"providers.unknown_or_unbuilt_provider_fails_start",
     test_a_provider_bdk_does_not_know_or_has_not_built_fails_the_start},
    {"providers.only_default_reputation_takes_an_endpoint",
     test_only_the_default_reputation_lists_take_an_endpoint},
    {"providers.google_key_only_with_a_google_provider",
     test_the_google_key_is_given_only_with_a_google_provider},
    {"providers.app_service_needs_provider_by_freeze",
     test_an_app_service_needs_its_provider_by_the_freeze},
    {"providers.registration_checks",
     test_registration_checks_the_service_the_operations_and_the_descriptor},
    {"providers.app_provider_answers_through_contract",
     test_the_app_provider_answers_through_the_contract},
    {"providers.contract_violation_refused", test_an_answer_that_breaks_the_contract_is_refused},
    {"providers.default_translation_unavailable",
     test_the_default_translation_provider_is_not_in_this_runtime},
    {"providers.cancel_tells_provider",
     test_cancelling_a_call_tells_the_provider_and_ignores_its_answer},
    {"providers.shutdown_cancels_before_release",
     test_a_call_outstanding_at_shutdown_is_cancelled_before_release},
    {"translation.preferences_per_profile", test_preferences_are_kept_per_profile_and_reported},
    {"translation.download_model_progress",
     test_downloading_a_model_reports_progress_and_the_new_models},
    {"translation.disabled_surface_turns_service_off",
     test_a_disabled_translation_surface_turns_the_service_off},
    {"translation.page_translated_through_app_provider",
     test_a_page_is_translated_through_the_app_provider},
    {"translation.offer_reaches_handler",
     test_the_offer_reaches_the_handler_and_its_answer_translates},
    {"translation.never_and_always", test_never_and_always_decide_without_asking},
    {"translation.provider_detects_language", test_the_provider_detects_an_undeclared_language},
    {"translation.batches_and_bad_batch",
     test_a_long_page_goes_in_batches_and_a_bad_batch_fails_it},
    {"translation.unsupported_pair", test_a_pair_the_provider_does_not_offer_is_refused},
    {"translation.navigation_cancels", test_navigating_away_cancels_the_translation},
    {"reputation.default_lists_block_listed_url",
     test_the_default_lists_block_a_listed_url_with_a_warning},
    {"reputation.proceed_needs_gesture", test_proceeding_past_a_warning_needs_a_gesture},
    {"reputation.list_rules", test_a_list_must_be_well_formed_newer_and_current},
    {"reputation.app_provider_decides", test_the_app_reputation_provider_decides_the_warning},
    {"geolocation.app_provider_positions",
     test_the_app_geolocation_provider_gives_the_page_its_position},
    {NULL, NULL},
};
