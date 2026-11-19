/* Scenarios for the services and the browser-owned prompts that only a real engine can run:
   passwords and autofill (T-SVC-04, T-UIR-04), downloads (T-UIR-03), printing (T-UIR-04) and the
   security interstitials (T-UIR-05). Built only into the engine conformance app
   (BDK_HARNESS_ENGINE_TESTS). Pages are served by the engine's test interceptor; what the operating
   system or a default presenter would ask the user is answered by the test hooks
   (bdk_testing_control in bdk_testing.h). */
#include <stdlib.h>
#include <unistd.h>

#include "bdk_testing.h"
#include "test_support.h"

/* From harness_engine.mm: while set, a new runtime keeps the persistent profiles of the last one.
 */
extern int bdk_harness_engine_keep_profiles;

/* ---- helpers ---- */

static void control(const char* verb, const char* args, char* out, size_t n) {
    REQUIRE(bdk_testing_control(t_runtime, S(verb), S(args), out, n) == BDK_OK);
}

static int control_count(const char* verb, const char* surface) {
    char out[32] = "";
    char args[96] = "{}";
    if (surface)
        snprintf(args, sizeof args, "{\"surface\":\"%s\"}", surface);
    control(verb, args, out, sizeof out);
    return atoi(out);
}

static void set_authentication(const char* answer) {
    char args[64];
    snprintf(args, sizeof args, "{\"answer\":\"%s\"}", answer);
    control("authentication", args, NULL, 0);
}

static int authentication_count(void) {
    return control_count("authentication.count", NULL);
}

/* Calls a built-in command on a profile and pumps until it answers. */
static void invoke(bdk_profile_t profile, const char* command, const char* args, t_capture_t* cap) {
    bdk_invoke_options_t o;
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.profile = profile;
    t_cap_init(cap);
    REQUIRE(bdk_invoke_named(t_runtime, S(command), S(args), &o, t_on_result, NULL, cap,
                             t_on_release) != 0);
    for (int i = 0; i < 40 && cap->results == 0; ++i)
        t_pump();
    REQUIRE(cap->results == 1);
}

static void invoke_ok(bdk_profile_t profile, const char* command, const char* args,
                      t_capture_t* cap) {
    invoke(profile, command, args, cap);
    if (cap->has_error)
        printf("    %s failed: %s.%s: %s\n", command, cap->domain, cap->code, cap->message);
    REQUIRE(!cap->has_error);
}

/* ---- passwords commands (T-SVC-04) ---- */

static void test_engine_passwords_are_stored_listed_updated_and_removed(void) {
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Passwords");
    t_capture_t cap;
    invoke_ok(p, "passwords.add",
              "{\"origin\":\"https://login.example\",\"username\":\"ada\",\"password\":\"s3cret\"}",
              &cap);
    char id[32];
    REQUIRE(t_json_string(cap.json, "id", id, sizeof id));
    CHECK(strstr(cap.json, "\"origin\":\"https://login.example/\"") != NULL);
    CHECK(strstr(cap.json, "\"username\":\"ada\"") != NULL);
    CHECK(strstr(cap.json, "s3cret") == NULL); /* never in a listing or an add result */

    invoke_ok(p, "passwords.list", "{}", &cap);
    CHECK(strstr(cap.json, id) != NULL && strstr(cap.json, "ada") != NULL);
    CHECK(strstr(cap.json, "s3cret") == NULL);
    invoke_ok(p, "passwords.forOrigin", "{\"origin\":\"https://login.example\"}", &cap);
    CHECK(strstr(cap.json, "ada") != NULL);
    invoke_ok(p, "passwords.forOrigin", "{\"origin\":\"https://other.example\"}", &cap);
    CHECK(strstr(cap.json, "ada") == NULL);

    /* A rename needs no authentication; a new password does. */
    const int before = authentication_count();
    set_authentication("allow");
    char args[160];
    snprintf(args, sizeof args, "{\"id\":\"%s\",\"username\":\"grace\"}", id);
    invoke_ok(p, "passwords.update", args, &cap);
    CHECK(strstr(cap.json, "\"username\":\"grace\"") != NULL);
    CHECK(authentication_count() == before);
    /* The username is part of the store's key, so a rename gives the login a new identifier. */
    REQUIRE(t_json_string(cap.json, "id", id, sizeof id));
    snprintf(args, sizeof args, "{\"id\":\"%s\",\"password\":\"n3w\"}", id);
    invoke_ok(p, "passwords.update", args, &cap);
    CHECK(authentication_count() == before + 1);

    snprintf(args, sizeof args, "{\"id\":\"%s\"}", id);
    invoke_ok(p, "passwords.remove", args, &cap);
    invoke_ok(p, "passwords.list", "{}", &cap);
    CHECK(strstr(cap.json, "grace") == NULL);
    invoke(p, "passwords.remove", args, &cap);
    CHECK(cap.has_error && strcmp(cap.code, "invalidArguments") == 0);
}

static void test_engine_revealing_a_password_needs_operating_system_authentication(void) {
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Reveal");
    t_capture_t cap;
    invoke_ok(p, "passwords.add",
              "{\"origin\":\"https://bank.example\",\"username\":\"ada\",\"password\":\"hunter2\"}",
              &cap);
    char id[32];
    REQUIRE(t_json_string(cap.json, "id", id, sizeof id));
    char args[64];
    snprintf(args, sizeof args, "{\"id\":\"%s\"}", id);

    set_authentication("deny");
    const int before = authentication_count();
    invoke(p, "passwords.reveal", args, &cap);
    CHECK(cap.has_error && strcmp(cap.code, "reauthenticationFailed") == 0);
    CHECK(strstr(cap.json, "hunter2") == NULL);
    CHECK(authentication_count() == before + 1);

    set_authentication("allow");
    invoke_ok(p, "passwords.reveal", args, &cap);
    CHECK(strstr(cap.json, "\"password\":\"hunter2\"") != NULL);
    CHECK(authentication_count() == before + 2);
}

static void test_engine_stored_passwords_survive_a_relaunch(void) {
    bdk_harness_engine_keep_profiles = 1;
    t_start_frozen();
    char profile_id[64];
    t_create_profile("Kept passwords", profile_id, sizeof profile_id);
    bdk_profile_t p = t_open_profile(profile_id);
    t_capture_t cap;
    invoke_ok(p, "passwords.add",
              "{\"origin\":\"https://kept.example\",\"username\":\"ada\",\"password\":\"p4ss\"}",
              &cap);
    char id[32];
    REQUIRE(t_json_string(cap.json, "id", id, sizeof id));
    t_stop();

    t_start_frozen();
    p = t_open_profile(profile_id);
    bdk_harness_engine_keep_profiles = 0;
    invoke_ok(p, "passwords.list", "{}", &cap);
    CHECK(strstr(cap.json, "kept.example") != NULL);
    set_authentication("allow");
    char args[64];
    snprintf(args, sizeof args, "{\"id\":\"%s\"}", id);
    invoke_ok(p, "passwords.reveal", args, &cap);
    CHECK(strstr(cap.json, "\"password\":\"p4ss\"") != NULL);
}

static void test_engine_passwords_export_and_import_round_trip(void) {
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Export");
    t_capture_t cap;
    invoke_ok(p, "passwords.add",
              "{\"origin\":\"https://csv.example\",\"username\":\"a,b\",\"password\":\"q\\\"x\"}",
              &cap);
    char path[128];
    snprintf(path, sizeof path, "%s/bdk-passwords-%d.csv",
             getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
    char args[256];
    snprintf(args, sizeof args, "{\"path\":\"%s\"}", path);
    set_authentication("allow");
    invoke_ok(p, "passwords.export", args, &cap);
    FILE* f = fopen(path, "r");
    REQUIRE(f != NULL);
    char text[512] = "";
    size_t n = fread(text, 1, sizeof text - 1, f);
    text[n] = 0;
    fclose(f);
    CHECK(strstr(text, "name,url,username,password,note") != NULL);
    CHECK(strstr(text, "\"a,b\"") != NULL && strstr(text, "\"q\"\"x\"") != NULL);

    bdk_profile_t q = t_new_profile("Import");
    invoke_ok(q, "passwords.import", args, &cap);
    CHECK(strstr(cap.json, "\"imported\":1") != NULL);
    invoke_ok(q, "passwords.list", "{}", &cap);
    CHECK(strstr(cap.json, "csv.example") != NULL && strstr(cap.json, "a,b") != NULL);
    remove(path);
}

static void test_engine_passwords_without_a_leak_check_provider_have_no_issues(void) {
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Issues");
    t_capture_t cap;
    invoke_ok(p, "passwords.issues", "{}", &cap);
    CHECK(strstr(cap.json, "\"items\":[]") != NULL);
    set_authentication("allow");
    invoke(p, "passwords.check", "{}", &cap);
    CHECK(cap.has_error && strcmp(cap.domain, "provider") == 0 &&
          strcmp(cap.code, "unavailable") == 0);
}

/* ---- autofill commands (T-SVC-04) ---- */

static void test_engine_autofill_addresses_are_stored_updated_and_removed(void) {
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Addresses");
    t_capture_t cap;
    invoke_ok(p, "autofill.addresses.add",
              "{\"fields\":{\"fullName\":\"Ada Lovelace\",\"city\":\"London\","
              "\"postalCode\":\"N1 9GU\",\"country\":\"GB\"}}",
              &cap);
    char id[64];
    REQUIRE(t_json_string(cap.json, "id", id, sizeof id));
    CHECK(strstr(cap.json, "Ada Lovelace") != NULL && strstr(cap.json, "London") != NULL);
    invoke_ok(p, "autofill.addresses.list", "{}", &cap);
    CHECK(strstr(cap.json, id) != NULL);
    char args[256];
    snprintf(args, sizeof args, "{\"id\":\"%s\",\"fields\":{\"city\":\"Oxford\"}}", id);
    invoke_ok(p, "autofill.addresses.update", args, &cap);
    CHECK(strstr(cap.json, "Oxford") != NULL && strstr(cap.json, "Ada Lovelace") != NULL);
    snprintf(args, sizeof args, "{\"id\":\"%s\"}", id);
    invoke_ok(p, "autofill.addresses.remove", args, &cap);
    invoke_ok(p, "autofill.addresses.list", "{}", &cap);
    CHECK(strstr(cap.json, id) == NULL);
    invoke(p, "autofill.addresses.remove", args, &cap);
    CHECK(cap.has_error && strcmp(cap.code, "invalidArguments") == 0);
}

static void test_engine_autofill_cards_are_masked_and_revealing_needs_authentication(void) {
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Cards");
    t_capture_t cap;
    invoke_ok(p, "autofill.cards.add",
              "{\"card\":{\"holder\":\"Ada Lovelace\",\"number\":\"4111111111111111\","
              "\"expiryMonth\":12,\"expiryYear\":2035}}",
              &cap);
    char id[64];
    REQUIRE(t_json_string(cap.json, "id", id, sizeof id));
    CHECK(strstr(cap.json, "\"lastFour\":\"1111\"") != NULL);
    CHECK(strstr(cap.json, "4111111111111111") == NULL);
    invoke_ok(p, "autofill.cards.list", "{}", &cap);
    CHECK(strstr(cap.json, id) != NULL && strstr(cap.json, "4111111111111111") == NULL);

    char args[96];
    snprintf(args, sizeof args, "{\"id\":\"%s\"}", id);
    set_authentication("deny");
    invoke(p, "autofill.cards.reveal", args, &cap);
    CHECK(cap.has_error && strcmp(cap.code, "reauthenticationFailed") == 0);
    set_authentication("allow");
    invoke_ok(p, "autofill.cards.reveal", args, &cap);
    CHECK(strstr(cap.json, "\"number\":\"4111111111111111\"") != NULL);

    snprintf(args, sizeof args, "{\"id\":\"%s\",\"card\":{\"expiryYear\":2036}}", id);
    invoke_ok(p, "autofill.cards.update", args, &cap);
    CHECK(strstr(cap.json, "\"expiryYear\":2036") != NULL);
    snprintf(args, sizeof args, "{\"id\":\"%s\"}", id);
    invoke_ok(p, "autofill.cards.remove", args, &cap);
    invoke_ok(p, "autofill.cards.list", "{}", &cap);
    CHECK(strstr(cap.json, id) == NULL);
}

const t_test_t t_engine_prompt_tests[] = {
    {"engine_passwords_are_stored_listed_updated_and_removed",
     test_engine_passwords_are_stored_listed_updated_and_removed},
    {"engine_revealing_a_password_needs_operating_system_authentication",
     test_engine_revealing_a_password_needs_operating_system_authentication},
    {"engine_stored_passwords_survive_a_relaunch", test_engine_stored_passwords_survive_a_relaunch},
    {"engine_passwords_export_and_import_round_trip",
     test_engine_passwords_export_and_import_round_trip},
    {"engine_passwords_without_a_leak_check_provider_have_no_issues",
     test_engine_passwords_without_a_leak_check_provider_have_no_issues},
    {"engine_autofill_addresses_are_stored_updated_and_removed",
     test_engine_autofill_addresses_are_stored_updated_and_removed},
    {"engine_autofill_cards_are_masked_and_revealing_needs_authentication",
     test_engine_autofill_cards_are_masked_and_revealing_needs_authentication},
    {NULL, NULL},
};
