/* Version and hash, handle rules, errors, struct growth, thread rules. */
#include <stddef.h>

#include "test_support.h"

static void test_version_matches_header(void) {
    CHECK(bdk_abi_version() == BDK_ABI_VERSION);
    CHECK(bdk_abi_version() >= 1);
}

static void test_hash_is_lowercase_hex_sha256(void) {
    bdk_string_t h = bdk_abi_hash();
    REQUIRE(h.length == 64);
    for (size_t i = 0; i < h.length; ++i) {
        char c = h.data[i];
        CHECK((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    }
    bdk_string_t again = bdk_abi_hash();
    CHECK(again.length == 64 && memcmp(h.data, again.data, 64) == 0);
}

static void test_manifest_is_json_naming_the_hash(void) {
    bdk_string_t m = bdk_runtime_manifest_json();
    REQUIRE(m.length > 2);
    CHECK(m.data[0] == '{' && m.data[m.length - 1] == '}');
    CHECK(str_has(m, "\"abiHash\":\""));
    CHECK(str_has(m, bdk_abi_hash().data));
}

static void any_thread_probe(void* p) {
    int* ok = (int*)p;
    bdk_string_t h = bdk_abi_hash();
    *ok = bdk_abi_version() == BDK_ABI_VERSION && h.length == 64 &&
          bdk_runtime_manifest_json().length > 0;
}

static void test_version_and_hash_callable_on_any_thread_before_start(void) {
    int ok = 0;
    bdk_harness_run_on_other_thread(any_thread_probe, &ok);
    CHECK(ok == 1);
}

static void test_error_accessors_describe_a_failure(void) {
    t_start();
    bdk_error_t err = 0;
    REQUIRE(bdk_runtime_freeze_registrations(t_runtime, &err) == BDK_OK);
    REQUIRE(bdk_runtime_freeze_registrations(t_runtime, &err) == BDK_ERR_FAILED);
    REQUIRE(err != 0);
    CHECK(str_is(bdk_error_domain(err), "config"));
    CHECK(bdk_error_code(err).length > 0);
    CHECK(bdk_error_message(err).length > 0);
    bdk_string_t details = bdk_error_details_json(err);
    CHECK(details.length >= 2 && details.data[0] == '{');
    bdk_error_release(err);
    CHECK(bdk_error_domain(err).length == 0); /* released: nothing to read */
}

static void test_success_leaves_error_out_parameter_zero(void) {
    t_start();
    bdk_error_t err = 12345;
    CHECK(bdk_runtime_freeze_registrations(t_runtime, &err) == BDK_OK);
    CHECK(err == 0);
}

static void test_zero_handle_is_never_valid(void) {
    t_start_frozen();
    bdk_error_t err = 0;
    CHECK(bdk_release(0) == BDK_ERR_STALE_HANDLE);
    CHECK(bdk_profile_close(0, &err) == BDK_ERR_STALE_HANDLE);
    CHECK(bdk_contents_destroy(0) == BDK_ERR_STALE_HANDLE);
    CHECK(bdk_contents_load(0, S("https://example.com/"), &err) == BDK_ERR_STALE_HANDLE);
    CHECK(bdk_call_cancel(0) == BDK_ERR_STALE_HANDLE);
    CHECK(bdk_runtime_freeze_registrations(0, &err) == BDK_ERR_STALE_HANDLE);
    CHECK(bdk_session_revoke(0, &err) == BDK_ERR_STALE_HANDLE);
    CHECK(bdk_window_unregister(0, &err) == BDK_ERR_STALE_HANDLE);
    CHECK(bdk_ui_complete(0, S("deny"), S("{}"), (bdk_native_handle_t){0, 0, 0}, &err) ==
          BDK_ERR_STALE_HANDLE);
}

static void test_mistyped_handle_is_stale_and_touches_nothing(void) {
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Typed");
    bdk_contents_t c = t_new_contents(p);
    bdk_error_t err = 0;
    CHECK(bdk_contents_destroy(p) == BDK_ERR_STALE_HANDLE);
    CHECK(bdk_profile_close(c, &err) == BDK_ERR_STALE_HANDLE);
    CHECK(bdk_contents_stop(p, &err) == BDK_ERR_STALE_HANDLE);
    CHECK(bdk_session_revoke(c, &err) == BDK_ERR_STALE_HANDLE);
    CHECK(bdk_runtime_freeze_registrations(c, &err) == BDK_ERR_STALE_HANDLE);
    /* Both objects are intact. */
    bdk_contents_state_t st;
    memset(&st, 0, sizeof st);
    st.struct_size = sizeof st;
    CHECK(bdk_contents_state(c, &st, &err) == BDK_OK);
    bdk_buffer_t info = 0;
    CHECK(bdk_profile_info(p, &info, &err) == BDK_OK);
    CHECK(bdk_release(info) == BDK_OK);
}

static void test_double_release_is_stale(void) {
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Twice");
    bdk_contents_t c = t_new_contents(p);
    bdk_error_t err = 0;
    bdk_buffer_t b = 0;
    REQUIRE(bdk_contents_entries(c, &b, &err) == BDK_OK);
    CHECK(bdk_release(b) == BDK_OK);
    CHECK(bdk_release(b) == BDK_ERR_STALE_HANDLE);
    CHECK(bdk_buffer_bytes(b).length == 0);
    CHECK(bdk_release(c) == BDK_OK); /* the generic release destroys contents */
    CHECK(bdk_release(c) == BDK_ERR_STALE_HANDLE);
    CHECK(bdk_contents_destroy(c) == BDK_ERR_STALE_HANDLE);
}

static void test_reused_slot_does_not_resurrect_a_released_handle(void) {
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Slots");
    bdk_contents_t first = t_new_contents(p);
    REQUIRE(bdk_contents_destroy(first) == BDK_OK);
    bdk_contents_t second = t_new_contents(p);
    CHECK(second != first);
    bdk_error_t err = 0;
    CHECK(bdk_contents_stop(first, &err) == BDK_ERR_STALE_HANDLE);
    CHECK(bdk_contents_stop(second, &err) == BDK_OK);
}

static void test_runtime_handle_is_not_released_with_bdk_release(void) {
    t_start();
    CHECK(bdk_release(t_runtime) == BDK_ERR_INVALID_ARGUMENT);
    bdk_error_t err = 0;
    CHECK(bdk_runtime_freeze_registrations(t_runtime, &err) == BDK_OK); /* still alive */
}

static void test_struct_declared_smaller_fills_only_declared_bytes(void) {
    t_start_frozen();
    bdk_contents_t c = t_new_contents(t_new_profile("Small"));
    bdk_error_t err = 0;
    bdk_contents_state_t st;
    memset(&st, 0xAB, sizeof st);
    st.struct_size = offsetof(bdk_contents_state_t, security);
    CHECK(bdk_contents_state(c, &st, &err) == BDK_OK);
    CHECK(st.struct_size == offsetof(bdk_contents_state_t, security));
    CHECK(st.url.length > 0);          /* declared: filled */
    CHECK(st.security == 0xABABABABu); /* not declared: untouched */
    CHECK(st.zoom != 1.0);
    unsigned char* tail = (unsigned char*)&st + offsetof(bdk_contents_state_t, security);
    int untouched = 1;
    for (size_t i = 0; i < sizeof st - offsetof(bdk_contents_state_t, security); ++i) {
        if (tail[i] != 0xAB)
            untouched = 0;
    }
    CHECK(untouched);
}

static void test_struct_declared_larger_is_accepted_and_tail_untouched(void) {
    t_start_frozen();
    bdk_contents_t c = t_new_contents(t_new_profile("Large"));
    bdk_error_t err = 0;
    struct {
        bdk_contents_state_t state;
        unsigned char future[32];
    } big;
    memset(&big, 0xCD, sizeof big);
    big.state.struct_size = sizeof big;
    CHECK(bdk_contents_state(c, &big.state, &err) == BDK_OK);
    CHECK(big.state.zoom == 1.0);
    int untouched = 1;
    for (size_t i = 0; i < sizeof big.future; ++i) {
        if (big.future[i] != 0xCD)
            untouched = 0;
    }
    CHECK(untouched);
}

static void test_struct_without_a_size_is_rejected(void) {
    t_start_frozen();
    bdk_contents_t c = t_new_contents(t_new_profile("NoSize"));
    bdk_error_t err = 0;
    bdk_contents_state_t st;
    memset(&st, 0, sizeof st);
    CHECK(bdk_contents_state(c, &st, &err) == BDK_ERR_INVALID_ARGUMENT);
    CHECK(bdk_contents_state(c, NULL, &err) == BDK_ERR_INVALID_ARGUMENT);
    bdk_contents_options_t o;
    memset(&o, 0, sizeof o);
    bdk_contents_t out = 0;
    CHECK(bdk_contents_create(t_runtime, &o, &out, &err) == BDK_ERR_INVALID_ARGUMENT);
    CHECK(bdk_contents_create(t_runtime, NULL, &out, &err) == BDK_ERR_INVALID_ARGUMENT);
}

static void test_options_struct_may_stop_before_later_fields(void) {
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Opts");
    bdk_contents_options_t o;
    memset(&o, 0xEE, sizeof o);
    o.struct_size = offsetof(bdk_contents_options_t, initial_url); /* an older caller */
    o.profile = p;
    bdk_contents_t c = 0;
    bdk_error_t err = 0;
    CHECK(bdk_contents_create(t_runtime, &o, &c, &err) == BDK_OK);
    CHECK(c != 0);
}

static void probe_wrong_thread(void* p) {
    int* bad = (int*)p;
    bdk_error_t err = 0;
    bdk_profile_t profile = 0;
    bdk_contents_t contents = 0;
    bdk_start_config_t cfg;
    bdk_runtime_t other = 0;
    t_fill_start_config(&cfg);
    if (bdk_runtime_start(&cfg, &other, &err) != BDK_ERR_WRONG_THREAD)
        ++*bad;
    if (bdk_runtime_freeze_registrations(t_runtime, &err) != BDK_ERR_WRONG_THREAD)
        ++*bad;
    if (bdk_profile_open_private(t_runtime, &profile, &err) != BDK_ERR_WRONG_THREAD)
        ++*bad;
    bdk_contents_options_t o;
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    if (bdk_contents_create(t_runtime, &o, &contents, &err) != BDK_ERR_WRONG_THREAD)
        ++*bad;
    if (bdk_release(t_runtime) != BDK_ERR_WRONG_THREAD)
        ++*bad;
    if (bdk_call_cancel(1) != BDK_ERR_WRONG_THREAD)
        ++*bad;
    t_capture_t cap;
    t_cap_init(&cap);
    if (bdk_invoke_named(t_runtime, S("app.x"), S("{}"), NULL, t_on_result, NULL, &cap,
                         t_on_release) != 0)
        ++*bad;
    if (bdk_profiles_list(t_runtime, t_on_result, &cap, t_on_release) != 0)
        ++*bad;
    if (cap.results || cap.releases)
        ++*bad; /* a rejected call calls nothing back */
}

static void test_calls_from_another_thread_return_wrong_thread(void) {
    t_start_frozen();
    int bad = 0;
    bdk_harness_run_on_other_thread(probe_wrong_thread, &bad);
    CHECK(bad == 0);
    /* The UI thread is unaffected. */
    bdk_error_t err = 0;
    bdk_profile_t p = 0;
    CHECK(bdk_profile_open_private(t_runtime, &p, &err) == BDK_OK);
}

static int g_posted_thread_ok;
static int g_posted_count;
static void posted(void* ctx) {
    ++g_posted_count;
    g_posted_thread_ok = ctx == (void*)&g_posted_count;
}
static void post_from_thread(void* p) {
    (void)p;
    bdk_post_to_ui_thread(posted, &g_posted_count);
}

static void test_post_to_ui_thread_runs_later_on_the_ui_thread(void) {
    t_start();
    g_posted_count = 0;
    bdk_post_to_ui_thread(posted, &g_posted_count);
    CHECK(g_posted_count == 0); /* never synchronous */
    bdk_harness_run_on_other_thread(post_from_thread, NULL);
    CHECK(g_posted_count == 0);
    t_pump();
    CHECK(g_posted_count == 2);
    CHECK(g_posted_thread_ok);
}

const t_test_t t_base_tests[] = {
    {"base.version_matches_header", test_version_matches_header},
    {"base.hash_is_lowercase_hex_sha256", test_hash_is_lowercase_hex_sha256},
    {"base.manifest_is_json_naming_the_hash", test_manifest_is_json_naming_the_hash},
    {"base.version_and_hash_callable_on_any_thread_before_start",
     test_version_and_hash_callable_on_any_thread_before_start},
    {"errors.accessors_describe_a_failure", test_error_accessors_describe_a_failure},
    {"errors.success_leaves_out_parameter_zero", test_success_leaves_error_out_parameter_zero},
    {"handles.zero_is_never_valid", test_zero_handle_is_never_valid},
    {"handles.mistyped_is_stale_and_touches_nothing",
     test_mistyped_handle_is_stale_and_touches_nothing},
    {"handles.double_release_is_stale", test_double_release_is_stale},
    {"handles.reused_slot_does_not_resurrect",
     test_reused_slot_does_not_resurrect_a_released_handle},
    {"handles.runtime_is_not_released_with_release",
     test_runtime_handle_is_not_released_with_bdk_release},
    {"structs.declared_smaller_fills_only_declared_bytes",
     test_struct_declared_smaller_fills_only_declared_bytes},
    {"structs.declared_larger_is_accepted",
     test_struct_declared_larger_is_accepted_and_tail_untouched},
    {"structs.without_a_size_is_rejected", test_struct_without_a_size_is_rejected},
    {"structs.options_may_stop_before_later_fields",
     test_options_struct_may_stop_before_later_fields},
    {"threads.calls_from_another_thread_return_wrong_thread",
     test_calls_from_another_thread_return_wrong_thread},
    {"threads.post_to_ui_thread_runs_later_on_the_ui_thread",
     test_post_to_ui_thread_runs_later_on_the_ui_thread},
    {NULL, NULL}};
