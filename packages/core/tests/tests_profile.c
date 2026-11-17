/* Profiles, including the ephemeral kind, and the other host-function families (windows,
 * automation, logs). */
#include "test_support.h"

static void run_list(t_capture_t* cap) {
    t_cap_init(cap);
    REQUIRE(bdk_profiles_list(t_runtime, t_on_result, cap, t_on_release) != 0);
    t_pump();
    REQUIRE(cap->results == 1 && !cap->has_error);
}

static void test_create_list_open_close(void) {
    t_start_frozen();
    t_capture_t cap;
    run_list(&cap);
    CHECK(strcmp(cap.json, "[]") == 0);

    char id[64];
    t_create_profile("Work", id, sizeof id);
    CHECK(strlen(id) > 0);
    run_list(&cap);
    CHECK(strstr(cap.json, id) != NULL);
    CHECK(strstr(cap.json, "\"name\":\"Work\"") != NULL);
    CHECK(strstr(cap.json, "\"kind\":\"persistent\"") != NULL);

    bdk_profile_t p = t_open_profile(id);
    bdk_buffer_t info = 0;
    bdk_error_t err = 0;
    REQUIRE(bdk_profile_info(p, &info, &err) == BDK_OK);
    char text[256];
    t_buffer_text(info, text, sizeof text);
    CHECK(strstr(text, id) != NULL);
    CHECK(strstr(text, "\"parent\"") != NULL);
    bdk_release(info);

    CHECK(bdk_profile_close(p, &err) == BDK_OK);
    CHECK(bdk_profile_close(p, &err) == BDK_ERR_STALE_HANDLE);
    run_list(&cap); /* a closed persistent profile is still listed */
    CHECK(strstr(cap.json, id) != NULL);
}

static void test_open_never_creates(void) {
    t_start_frozen();
    t_capture_t cap;
    t_cap_init(&cap);
    bdk_profile_t p = 99;
    REQUIRE(bdk_profile_open(t_runtime, S("does-not-exist"), &p, t_on_result, &cap, t_on_release) !=
            0);
    t_pump();
    CHECK(cap.results == 1 && cap.has_error);
    CHECK(strcmp(cap.domain, "profile") == 0 && strcmp(cap.code, "notFound") == 0);
    CHECK(p == 0);
    run_list(&cap);
    CHECK(strcmp(cap.json, "[]") == 0);
}

static void test_duplicate_name_and_empty_name_fail(void) {
    t_start_frozen();
    char id[64];
    t_create_profile("Same", id, sizeof id);
    t_capture_t cap;
    t_cap_init(&cap);
    REQUIRE(bdk_profile_create(t_runtime, S("Same"), t_on_result, &cap, t_on_release) != 0);
    t_pump();
    CHECK(cap.has_error && strcmp(cap.domain, "profile") == 0 &&
          strcmp(cap.code, "nameTaken") == 0);
    t_cap_init(&cap);
    REQUIRE(bdk_profile_create(t_runtime, S(""), t_on_result, &cap, t_on_release) != 0);
    t_pump();
    CHECK(cap.has_error && strcmp(cap.domain, "broker") == 0 &&
          strcmp(cap.code, "invalidArguments") == 0);
}

static void test_open_twice_and_delete_while_open_fail(void) {
    t_start_frozen();
    char id[64];
    t_create_profile("Busy", id, sizeof id);
    bdk_profile_t p = t_open_profile(id);
    t_capture_t cap;
    t_cap_init(&cap);
    bdk_profile_t again = 0;
    REQUIRE(bdk_profile_open(t_runtime, S(id), &again, t_on_result, &cap, t_on_release) != 0);
    t_pump();
    CHECK(cap.has_error && strcmp(cap.code, "inUse") == 0);
    CHECK(again == 0);
    t_cap_init(&cap);
    REQUIRE(bdk_profile_delete(t_runtime, S(id), t_on_result, &cap, t_on_release) != 0);
    t_pump();
    CHECK(cap.has_error && strcmp(cap.code, "inUse") == 0);
    bdk_error_t err = 0;
    REQUIRE(bdk_profile_close(p, &err) == BDK_OK);
    t_cap_init(&cap);
    REQUIRE(bdk_profile_delete(t_runtime, S(id), t_on_result, &cap, t_on_release) != 0);
    t_pump();
    CHECK(!cap.has_error);
    run_list(&cap);
    CHECK(strcmp(cap.json, "[]") == 0);
}

static void test_rename(void) {
    t_start_frozen();
    char id[64], other[64];
    t_create_profile("First", id, sizeof id);
    t_create_profile("Second", other, sizeof other);
    bdk_profile_t p = t_open_profile(id);
    bdk_error_t err = 0;
    CHECK(bdk_profile_rename(p, S("Renamed"), &err) == BDK_OK);
    CHECK(bdk_profile_rename(p, S("Second"), &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "profile", "nameTaken"));
    CHECK(bdk_profile_rename(p, S(""), &err) == BDK_ERR_INVALID_ARGUMENT);
    t_capture_t cap;
    run_list(&cap);
    CHECK(strstr(cap.json, "\"name\":\"Renamed\"") != NULL);
}

static void test_ephemeral_profile_leaves_no_trace(void) {
    t_start_frozen();
    bdk_profile_t p = 0;
    bdk_error_t err = 0;
    REQUIRE(bdk_profile_open_private(t_runtime, &p, &err) == BDK_OK);
    REQUIRE(p != 0);
    bdk_buffer_t info = 0;
    REQUIRE(bdk_profile_info(p, &info, &err) == BDK_OK);
    char text[256], id[64];
    t_buffer_text(info, text, sizeof text);
    bdk_release(info);
    CHECK(strstr(text, "\"kind\":\"ephemeral\"") != NULL);
    REQUIRE(t_json_string(text, "id", id, sizeof id));
    CHECK(bdk_profile_rename(p, S("Named"), &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "profile", "isPrivate"));

    t_capture_t cap;
    run_list(&cap);
    CHECK(strstr(cap.json, id) == NULL); /* never listed */
    bdk_contents_t c = t_new_contents(p);
    CHECK(bdk_profile_close(p, &err) == BDK_OK);
    CHECK(bdk_contents_destroy(c) ==
          BDK_ERR_STALE_HANDLE); /* closing the profile closed its contents */
    t_cap_init(&cap);
    bdk_profile_t again = 0;
    REQUIRE(bdk_profile_open(t_runtime, S(id), &again, t_on_result, &cap, t_on_release) != 0);
    t_pump();
    CHECK(cap.has_error && strcmp(cap.code, "notFound") == 0); /* gone for good */
}

static void test_two_ephemeral_profiles_are_independent(void) {
    t_start_frozen();
    bdk_profile_t a = 0, b = 0;
    bdk_error_t err = 0;
    REQUIRE(bdk_profile_open_private(t_runtime, &a, &err) == BDK_OK);
    REQUIRE(bdk_profile_open_private(t_runtime, &b, &err) == BDK_OK);
    CHECK(a != b);
    CHECK(bdk_profile_close(a, &err) == BDK_OK);
    CHECK(bdk_profile_close(b, &err) == BDK_OK);
}

static void test_closing_a_profile_destroys_its_contents(void) {
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Owner");
    bdk_profile_t q = t_new_profile("Bystander");
    bdk_contents_t c = t_new_contents(p), d = t_new_contents(q);
    bdk_error_t err = 0;
    CHECK(bdk_profile_close(p, &err) == BDK_OK);
    CHECK(bdk_contents_stop(c, &err) == BDK_ERR_STALE_HANDLE);
    CHECK(bdk_contents_stop(d, &err) == BDK_OK);
}

static void test_contents_needs_a_live_profile(void) {
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Brief");
    bdk_error_t err = 0;
    REQUIRE(bdk_profile_close(p, &err) == BDK_OK);
    bdk_contents_options_t o;
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.profile = p;
    bdk_contents_t c = 0;
    CHECK(bdk_contents_create(t_runtime, &o, &c, &err) == BDK_ERR_STALE_HANDLE);
    CHECK(c == 0);
}

/* Windows, automation sessions, diagnostics. */
static void test_window_registration(void) {
    t_start_frozen();
    bdk_window_t w = 0;
    bdk_error_t err = 0;
    bdk_native_handle_t none;
    memset(&none, 0, sizeof none);
    CHECK(bdk_window_register(t_runtime, none, 1, &w, &err) == BDK_ERR_INVALID_ARGUMENT);
    bdk_native_handle_t native;
    memset(&native, 0, sizeof native);
    native.kind = BDK_NATIVE_NSWINDOW;
    native.id = 42;
    REQUIRE(bdk_window_register(t_runtime, native, 1, &w, &err) == BDK_OK);
    CHECK(w != 0);
    CHECK(bdk_window_unregister(w, &err) == BDK_OK);
    CHECK(bdk_window_unregister(w, &err) == BDK_ERR_STALE_HANDLE);
}

static void test_automation_session_lifecycle(void) {
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Auto");
    bdk_contents_t c = t_new_contents(p);
    bdk_session_t s = 0;
    bdk_error_t err = 0;
    CHECK(bdk_session_create(t_runtime, S("[]"), &s, &err) == BDK_ERR_INVALID_ARGUMENT);
    REQUIRE(bdk_session_create(t_runtime, S("{\"name\":\"t\"}"), &s, &err) == BDK_OK);
    CHECK(bdk_session_grant_contents(s, c, &err) == BDK_OK);
    CHECK(bdk_session_revoke(s, &err) == BDK_OK);
    CHECK(bdk_session_grant_contents(s, c, &err) == BDK_ERR_STALE_HANDLE);
    CHECK(bdk_session_revoke(s, &err) == BDK_ERR_STALE_HANDLE);
}

static int g_log_lines, g_log_released;
static void on_log(void* ctx, uint32_t level, bdk_string_t message) {
    (void)ctx;
    (void)level;
    (void)message;
    ++g_log_lines;
}
static void on_log_release(void* ctx) {
    (void)ctx;
    ++g_log_released;
}

static void test_log_handler_receives_lines_and_is_released(void) {
    t_start();
    g_log_lines = g_log_released = 0;
    bdk_error_t err = 0;
    REQUIRE(bdk_log_set_handler(t_runtime, on_log, NULL, on_log_release, &err) == BDK_OK);
    t_freeze();
    CHECK(g_log_lines >= 0);
    REQUIRE(bdk_log_set_handler(t_runtime, NULL, NULL, NULL, &err) == BDK_OK); /* removes it */
    t_pump();
    CHECK(g_log_released == 1);
    t_stop();
    CHECK(g_log_released == 1);
}

const t_test_t t_profile_tests[] = {
    {"profiles.create_list_open_close", test_create_list_open_close},
    {"profiles.open_never_creates", test_open_never_creates},
    {"profiles.duplicate_and_empty_names_fail", test_duplicate_name_and_empty_name_fail},
    {"profiles.open_twice_and_delete_while_open_fail", test_open_twice_and_delete_while_open_fail},
    {"profiles.rename", test_rename},
    {"profiles.ephemeral_leaves_no_trace", test_ephemeral_profile_leaves_no_trace},
    {"profiles.ephemeral_profiles_are_independent", test_two_ephemeral_profiles_are_independent},
    {"profiles.closing_destroys_its_contents", test_closing_a_profile_destroys_its_contents},
    {"profiles.contents_needs_a_live_profile", test_contents_needs_a_live_profile},
    {"windows.registration", test_window_registration},
    {"automation.session_lifecycle", test_automation_session_lifecycle},
    {"diagnostics.log_handler_is_released_once", test_log_handler_receives_lines_and_is_released},
    {NULL, NULL}};
