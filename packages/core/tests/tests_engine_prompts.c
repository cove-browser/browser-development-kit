/* Scenarios for the services and the browser-owned prompts that only a real engine can run:
   passwords and autofill (T-SVC-04, T-UIR-04), downloads (T-UIR-03), printing (T-UIR-04) and the
   security interstitials (T-UIR-05). Built only into the engine conformance app
   (BDK_HARNESS_ENGINE_TESTS). Pages are served by the engine's test interceptor; what the operating
   system or a default presenter would ask the user is answered by the test hooks
   (bdk_testing_command in bdk_testing.h). */
#include <netinet/in.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "bdk_testing.h"
#include "test_support.h"

/* From harness_engine.mm: the [ui.surfaces] table the next runtime starts with. */
extern const char* bdk_harness_engine_surfaces;

/* From harness_engine.mm: while set, a new runtime keeps the persistent profiles of the last one.
 */
extern int bdk_harness_engine_keep_profiles;

/* ---- helpers ---- */

static void control(const char* verb, const char* args, char* out, size_t n) {
    REQUIRE(bdk_testing_command(t_runtime, S(verb), S(args), out, n) == BDK_OK);
}

/* The integer member `key` of the JSON object a test hook wrote. */
static int json_int(const char* json, const char* key) {
    char pattern[32];
    snprintf(pattern, sizeof pattern, "\"%s\":", key);
    const char* at = strstr(json, pattern);
    return at ? atoi(at + strlen(pattern)) : -1;
}

/* Scripts the default presenter of the surface: it shows nothing and answers `answer` (the key of
   a button, or a path). Scripting forgets what the presenter recorded. */
static void script_presenter(const char* surface, const char* answer) {
    char args[512];
    snprintf(args, sizeof args, "{\"surface\":\"%s\",\"answer\":{\"answer\":\"%s\"}}", surface,
             answer);
    control("present.script", args, NULL, 0);
}

/* How many times the surface's default presenter was asked to show something since it was
   scripted. */
static int presenter_uses(const char* surface) {
    char args[96], out[4096];
    snprintf(args, sizeof args, "{\"surface\":\"%s\"}", surface);
    control("present.record", args, out, sizeof out);
    return json_int(out, "shown");
}

static void set_authentication(const char* answer) {
    char args[64];
    snprintf(args, sizeof args, "{\"answer\":\"%s\"}", answer);
    control("authentication", args, NULL, 0);
}

static int authentication_count(void) {
    char out[32];
    control("authentication.count", "{}", out, sizeof out);
    return json_int(out, "count");
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

/* From harness_engine.mm: an application window registered with the runtime, and a page's view
   added to it. */
int bdk_harness_engine_window_open(bdk_runtime_t rt);
void bdk_harness_engine_window_attach(int window, bdk_contents_t contents);
void bdk_harness_engine_window_close(int window);

/* ---- pages and a recording handler ---- */

/* A page in a window of the application, focused: its tab is the active tab of an active context.
   Chromium shows page-modal prompts only for such a tab and queues them for any other. */
static int g_window = -1;

static void close_window(void) {
    if (g_window >= 0)
        bdk_harness_engine_window_close(g_window);
    g_window = -1;
}

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

/* Serves `html` (escaped here into the hook's JSON) at `url`, with extra response headers. */
static void script_page(const char* url, const char* content_type, const char* html,
                        const char* headers) {
    static char args[8192];
    size_t n = (size_t)snprintf(
        args, sizeof args, "{\"url\":\"%s\",\"contentType\":\"%s\",\"headers\":\"%s\",\"body\":\"",
        url, content_type, headers);
    for (const char* c = html; *c && n + 8 < sizeof args; ++c) {
        if (*c == '"' || *c == '\\')
            args[n++] = '\\';
        if (*c == '\n') {
            args[n++] = '\\';
            args[n++] = 'n';
            continue;
        }
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
    for (int i = 0; i < 60; ++i) {
        title_of(c, title, sizeof title);
        if (strcmp(title, want) == 0)
            return 1;
        t_pump();
        bdk_testing_run_for(0.1);
    }
    printf("    title is \"%s\", not \"%s\"\n", title, want);
    return 0;
}

static void run_script(bdk_contents_t c, const char* script, int gesture) {
    REQUIRE(bdk_testing_execute_script(c, S(script), (uint8_t)gesture) == BDK_OK);
    t_pump();
}

typedef struct {
    int calls;
    bdk_request_t request;
    char payload[1024];
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

static bdk_native_handle_t gesture(void) {
    bdk_native_handle_t g;
    memset(&g, 0, sizeof g);
    g.kind = BDK_NATIVE_NSEVENT;
    g.id = 1;
    return g;
}

static bdk_native_handle_t no_gesture(void) {
    bdk_native_handle_t g;
    memset(&g, 0, sizeof g);
    return g;
}

#define LOGIN_URL "https://login.example/"
#define LOGIN_PAGE                                                                                 \
    "<!doctype html><title>login</title><form id=f method=post action=/done>"                      \
    "<input id=u name=user autocomplete=username>"                                                 \
    "<input id=pw type=password name=pw autocomplete=current-password>"                            \
    "<input type=submit></form>"
#define DONE_PAGE "<!doctype html><title>welcome</title>"

/* Types a login into the page and submits it, as the user would. */
static void submit_login(bdk_contents_t c) {
    run_script(c,
               "document.getElementById('u').value='ada';"
               "document.getElementById('pw').value='s3cret';"
               "document.getElementById('f').requestSubmit();",
               1);
}

/* Waits, in real time, until the default presenter of the surface has run more than `before`
   times (the engine looks for a password question a few hundred milliseconds after a page). */
static void wait_for_presenter(const char* surface, int before) {
    for (int i = 0; i < 60 && presenter_uses(surface) == before; ++i) {
        t_pump();
        bdk_testing_run_for(0.1);
    }
}

/* ---- password save and fill prompts (T-UIR-04) ---- */

static void
test_engine_a_submitted_login_is_saved_by_the_default_prompt_and_filled_next_time(void) {
    t_start();
    t_freeze();
    script_page(LOGIN_URL, "text/html", LOGIN_PAGE, "");
    script_page(LOGIN_URL "done", "text/html", DONE_PAGE, "");
    script_presenter("passwords", "save");
    const int uses_before = presenter_uses("passwords");
    bdk_profile_t p = t_new_profile("Save and fill");
    bdk_contents_t c = new_active_page(p);
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_load(c, S(LOGIN_URL), &err) == BDK_OK);
    t_pump();
    submit_login(c);
    REQUIRE(wait_for_title(c, "welcome"));
    wait_for_presenter("passwords", uses_before);
    CHECK(presenter_uses("passwords") == uses_before + 1);
    t_capture_t cap;
    invoke_ok(p, "passwords.list", "{}", &cap);
    CHECK(strstr(cap.json, "login.example") != NULL &&
          strstr(cap.json, "\"username\":\"ada\"") != NULL);

    /* The next visit is filled by the password manager. */
    REQUIRE(bdk_contents_load(c, S(LOGIN_URL), &err) == BDK_OK);
    t_pump();
    /* Chromium hands a filled password to the page's script only after the user touched it. */
    t_pump();
    bdk_testing_run_for(0.5);
    REQUIRE(bdk_testing_click(c, 20, 20, 0) == BDK_OK);
    t_pump();
    run_script(c,
               "setInterval(function(){document.title='filled:'+"
               "document.getElementById('pw').value},200)",
               0);
    CHECK(wait_for_title(c, "filled:s3cret"));
    close_window();
}

static void test_engine_a_password_prompt_reaches_a_handler_and_saving_needs_a_gesture(void) {
    memset(&g_seen, 0, sizeof g_seen);
    t_start();
    const int presenter_before = presenter_uses("passwords");
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_PASSWORDS, hold, &g_seen, NULL, &err) ==
            BDK_OK);
    t_freeze();
    script_page(LOGIN_URL, "text/html", LOGIN_PAGE, "");
    script_page(LOGIN_URL "done", "text/html", DONE_PAGE, "");
    bdk_profile_t p = t_new_profile("Handled");
    bdk_contents_t c = new_active_page(p);
    REQUIRE(bdk_contents_load(c, S(LOGIN_URL), &err) == BDK_OK);
    t_pump();
    submit_login(c);
    REQUIRE(wait_for_title(c, "welcome"));
    for (int i = 0; i < 60 && g_seen.calls == 0; ++i) {
        t_pump();
        bdk_testing_run_for(0.1);
    }
    REQUIRE(g_seen.calls == 1);
    CHECK(strstr(g_seen.payload, "\"origin\":\"https://login.example/\"") != NULL);
    CHECK(strstr(g_seen.payload, "\"username\":\"ada\"") != NULL);
    CHECK(strstr(g_seen.payload, "\"isUpdate\":false") != NULL);
    CHECK(strstr(g_seen.payload, "s3cret") == NULL); /* the presenter never sees the password */
    CHECK(presenter_uses("passwords") == presenter_before); /* the handler took it */

    err = 0;
    CHECK(bdk_ui_complete(g_seen.request, S("grant"), S("{}"), no_gesture(), &err) != BDK_OK);
    t_capture_t cap;
    invoke_ok(p, "passwords.list", "{}", &cap);
    CHECK(strstr(cap.json, "ada") == NULL); /* nothing saved without the gesture */
    err = 0;
    CHECK(bdk_ui_complete(g_seen.request, S("grant"), S("{}"), gesture(), &err) == BDK_OK);
    t_pump();
    invoke_ok(p, "passwords.list", "{}", &cap);
    CHECK(strstr(cap.json, "\"username\":\"ada\"") != NULL);
    bdk_ui_request_release(g_seen.request);
    close_window();
}

static void test_engine_declining_the_password_prompt_saves_nothing(void) {
    t_start();
    t_freeze();
    script_page(LOGIN_URL, "text/html", LOGIN_PAGE, "");
    script_page(LOGIN_URL "done", "text/html", DONE_PAGE, "");
    script_presenter("passwords", "notNow");
    const int uses_before = presenter_uses("passwords");
    bdk_profile_t p = t_new_profile("Not now");
    bdk_contents_t c = new_active_page(p);
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_load(c, S(LOGIN_URL), &err) == BDK_OK);
    t_pump();
    submit_login(c);
    REQUIRE(wait_for_title(c, "welcome"));
    wait_for_presenter("passwords", uses_before);
    CHECK(presenter_uses("passwords") == uses_before + 1);
    t_capture_t cap;
    invoke_ok(p, "passwords.list", "{}", &cap);
    CHECK(strstr(cap.json, "ada") == NULL);
    close_window();
}

/* ---- printing (T-UIR-04) ---- */

static void test_engine_window_print_raises_a_printing_request_for_a_handler(void) {
    memset(&g_seen, 0, sizeof g_seen);
    t_start();
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_PRINTING, hold, &g_seen, NULL, &err) ==
            BDK_OK);
    t_freeze();
    script_page(LOGIN_URL, "text/html", LOGIN_PAGE, "");
    bdk_contents_t c = new_active_page(t_new_profile("Print handler"));
    REQUIRE(bdk_contents_load(c, S(LOGIN_URL), &err) == BDK_OK);
    REQUIRE(wait_for_title(c, "login"));
    run_script(c, "window.print()", 1);
    for (int i = 0; i < 80 && g_seen.calls == 0; ++i)
        t_pump();
    REQUIRE(g_seen.calls == 1);
    CHECK(strstr(g_seen.payload, "\"isUserInitiated\":false") != NULL);
    CHECK(presenter_uses("printing") == 0); /* the application has it */
    /* The application cancels: nothing is printed and the page carries on. */
    err = 0;
    CHECK(bdk_ui_complete(g_seen.request, S("deny"), S("{}"), no_gesture(), &err) == BDK_OK);
    t_pump();
    run_script(c, "document.title='after print'", 0);
    CHECK(wait_for_title(c, "after print"));
    bdk_ui_request_release(g_seen.request);
    close_window();
}

static void test_engine_window_print_with_no_handler_uses_the_system_print_panel(void) {
    t_start_frozen();
    script_presenter("printing", "systemDialog");
    const int uses_before = presenter_uses("printing");
    script_page(LOGIN_URL, "text/html", LOGIN_PAGE, "");
    bdk_error_t err = 0;
    bdk_contents_t c = new_active_page(t_new_profile("Print default"));
    REQUIRE(bdk_contents_load(c, S(LOGIN_URL), &err) == BDK_OK);
    REQUIRE(wait_for_title(c, "login"));
    run_script(c, "window.print()", 1);
    wait_for_presenter("printing", uses_before);
    CHECK(presenter_uses("printing") == uses_before + 1);
    run_script(c, "document.title='after print'", 0);
    CHECK(wait_for_title(c, "after print"));
    close_window();
}

static void test_engine_the_application_prints_for_a_handler(void) {
    memset(&g_seen, 0, sizeof g_seen);
    t_start();
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_PRINTING, hold, &g_seen, NULL, &err) ==
            BDK_OK);
    t_freeze();
    script_page(LOGIN_URL, "text/html", LOGIN_PAGE, "");
    bdk_contents_t c = new_active_page(t_new_profile("Print by application"));
    REQUIRE(bdk_contents_load(c, S(LOGIN_URL), &err) == BDK_OK);
    REQUIRE(wait_for_title(c, "login"));
    REQUIRE(bdk_contents_print(c, &err) == BDK_OK);
    for (int i = 0; i < 80 && g_seen.calls == 0; ++i)
        t_pump();
    REQUIRE(g_seen.calls == 1);
    CHECK(strstr(g_seen.payload, "\"isUserInitiated\":true") != NULL);
    CHECK(presenter_uses("printing") == 0); /* the application has it */
    err = 0;
    CHECK(bdk_ui_complete(g_seen.request, S("deny"), S("{}"), no_gesture(), &err) == BDK_OK);
    t_pump();
    run_script(c, "document.title='after print'", 0);
    CHECK(wait_for_title(c, "after print"));
    bdk_ui_request_release(g_seen.request);
    close_window();
}

static void test_engine_the_application_prints_with_no_handler_through_the_default(void) {
    t_start_frozen();
    script_presenter("printing", "systemDialog"); /* scripted: no system panel opens */
    const int uses_before = presenter_uses("printing");
    script_page(LOGIN_URL, "text/html", LOGIN_PAGE, "");
    bdk_error_t err = 0;
    bdk_contents_t c = new_active_page(t_new_profile("Print by application default"));
    REQUIRE(bdk_contents_load(c, S(LOGIN_URL), &err) == BDK_OK);
    REQUIRE(wait_for_title(c, "login"));
    REQUIRE(bdk_contents_print(c, &err) == BDK_OK);
    wait_for_presenter("printing", uses_before);
    CHECK(presenter_uses("printing") == uses_before + 1);
    run_script(c, "document.title='after print'", 0);
    CHECK(wait_for_title(c, "after print"));
    close_window();
}

/* ---- downloads (T-UIR-03) ---- */

#define DOWNLOAD_URL "https://dl.example/file.bin"
#define DANGEROUS_URL "https://dl.example/setup.command"
#define DOWNLOAD_HEADERS "Content-Disposition: attachment\\n"

static char g_dl_dir[64];
static char g_dl_chosen[128];

/* Answers a "where to save" request with the temporary directory; holds a dangerous one. */
static bdk_ui_response_t downloads_handler(void* ctx, const bdk_ui_request_info_t* info) {
    seen_t* seen = (seen_t*)ctx;
    if (info->payload_json.data && strstr(info->payload_json.data, "\"needsLocation\":true")) {
        char args[256];
        snprintf(args, sizeof args, "{\"destination\":\"%s\"}", g_dl_chosen);
        bdk_error_t err = 0;
        CHECK(bdk_ui_complete(info->request, S("accept"), S(args), no_gesture(), &err) == BDK_OK);
        return BDK_UI_HANDLED;
    }
    return hold(seen, info);
}

static void prepare_downloads(void) {
    snprintf(g_dl_dir, sizeof g_dl_dir, "/tmp/bdk-dl-XXXXXX");
    REQUIRE(mkdtemp(g_dl_dir) != NULL);
    snprintf(g_dl_chosen, sizeof g_dl_chosen, "%s/chosen.bin", g_dl_dir);
    script_page(DOWNLOAD_URL, "application/octet-stream", "0123456789", DOWNLOAD_HEADERS);
    script_page(DANGEROUS_URL, "application/octet-stream", "#!/bin/sh\necho hi\n",
                DOWNLOAD_HEADERS);
    script_page("https://dl.example/", "text/html", "<!doctype html><title>dl</title>", "");
    control("downloads.danger", "{\"on\":false}", NULL, 0);
}

static int wait_for_file(const char* path) {
    for (int i = 0; i < 120; ++i) {
        if (access(path, F_OK) == 0)
            return 1;
        t_pump();
    }
    return 0;
}

static void test_engine_a_download_destination_is_chosen_by_the_handler(void) {
    memset(&g_seen, 0, sizeof g_seen);
    t_start();
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_DOWNLOADS, downloads_handler, &g_seen, NULL,
                               &err) == BDK_OK);
    t_freeze();
    prepare_downloads();
    bdk_profile_t p = t_new_profile("Download destination");
    control("downloads.ask", "{\"on\":true}", NULL, 0);
    bdk_contents_t c = new_active_page(p);
    REQUIRE(bdk_contents_load(c, S(DOWNLOAD_URL), &err) == BDK_OK);
    CHECK(wait_for_file(g_dl_chosen)); /* the handler's destination, not the downloads folder */
    control("downloads.ask", "{\"on\":false}", NULL, 0);
    close_window();
}

static void test_engine_the_default_download_presenter_saves_where_the_user_says(void) {
    t_start_frozen();
    prepare_downloads();
    script_presenter("downloads", g_dl_chosen);
    const int uses_before = presenter_uses("downloads");
    bdk_error_t err = 0;
    bdk_profile_t p = t_new_profile("Download default");
    control("downloads.ask", "{\"on\":true}", NULL, 0);
    bdk_contents_t c = new_active_page(p);
    REQUIRE(bdk_contents_load(c, S(DOWNLOAD_URL), &err) == BDK_OK);
    CHECK(wait_for_file(g_dl_chosen));
    CHECK(presenter_uses("downloads") == uses_before + 1);
    control("downloads.ask", "{\"on\":false}", NULL, 0);
    close_window();
}

static void test_engine_a_dangerous_download_is_kept_only_with_a_gesture(void) {
    memset(&g_seen, 0, sizeof g_seen);
    t_start();
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_DOWNLOADS, downloads_handler, &g_seen, NULL,
                               &err) == BDK_OK);
    t_freeze();
    prepare_downloads();
    control("downloads.danger", "{\"on\":true}", NULL, 0);
    bdk_profile_t p = t_new_profile("Download danger");
    control("downloads.ask", "{\"on\":true}", NULL, 0);
    bdk_contents_t c = new_active_page(p);
    REQUIRE(bdk_contents_load(c, S("https://dl.example/"), &err) == BDK_OK);
    t_pump();
    /* A download the page starts without a user gesture. */
    run_script(c, "location.href='" DOWNLOAD_URL "'", 0);
    for (int i = 0; i < 120 && g_seen.calls == 0; ++i)
        t_pump();
    REQUIRE(g_seen.calls == 1);
    CHECK(strstr(g_seen.payload, "\"danger\":\"dangerousContent\"") != NULL);
    CHECK(access(g_dl_chosen, F_OK) != 0); /* not kept yet */

    err = 0;
    CHECK(bdk_ui_complete(g_seen.request, S("grant"), S("{}"), no_gesture(), &err) != BDK_OK);
    for (int i = 0; i < 20; ++i)
        t_pump();
    CHECK(access(g_dl_chosen, F_OK) != 0); /* still not kept */
    err = 0;
    CHECK(bdk_ui_complete(g_seen.request, S("grant"), S("{}"), gesture(), &err) == BDK_OK);
    CHECK(wait_for_file(g_dl_chosen));
    bdk_ui_request_release(g_seen.request);
    control("downloads.ask", "{\"on\":false}", NULL, 0);
    control("downloads.danger", "{\"on\":false}", NULL, 0);
    close_window();
}

static void test_engine_a_dropped_dangerous_download_is_discarded(void) {
    memset(&g_seen, 0, sizeof g_seen);
    t_start();
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_DOWNLOADS, downloads_handler, &g_seen, NULL,
                               &err) == BDK_OK);
    t_freeze();
    prepare_downloads();
    control("downloads.danger", "{\"on\":true}", NULL, 0);
    bdk_profile_t p = t_new_profile("Download dropped");
    control("downloads.ask", "{\"on\":true}", NULL, 0);
    bdk_contents_t c = new_active_page(p);
    REQUIRE(bdk_contents_load(c, S("https://dl.example/"), &err) == BDK_OK);
    t_pump();
    run_script(c, "location.href='" DOWNLOAD_URL "'", 0);
    for (int i = 0; i < 120 && g_seen.calls == 0; ++i)
        t_pump();
    REQUIRE(g_seen.calls == 1);
    bdk_ui_request_release(g_seen.request); /* never granted */
    for (int i = 0; i < 40; ++i)
        t_pump();
    CHECK(access(g_dl_chosen, F_OK) != 0);
    control("downloads.ask", "{\"on\":false}", NULL, 0);
    close_window();
}

/* ---- certificate interstitials (T-UIR-05) ---- */

/* A TLS server with a certificate nobody trusts: the shipped framework has no embedded test
   server, so the scenario runs the system's openssl as one on a loopback port. The engine's
   test interceptor leaves loopback addresses to the network. Returns its pid, or 0 (the test then
   skips). */
static pid_t g_tls_pid;
static int g_tls_port;

static void stop_tls_server(void) {
    if (g_tls_pid) {
        kill(g_tls_pid, SIGTERM);
        waitpid(g_tls_pid, NULL, 0);
        g_tls_pid = 0;
    }
}

static int start_tls_server(void) {
    char dir[] = "/tmp/bdk-tls-XXXXXX";
    if (!mkdtemp(dir))
        return 0;
    char cmd[512], key[128], cert[128], port[16];
    snprintf(key, sizeof key, "%s/k.pem", dir);
    snprintf(cert, sizeof cert, "%s/c.pem", dir);
    snprintf(cmd, sizeof cmd,
             "/usr/bin/openssl req -x509 -newkey rsa:2048 -nodes -keyout %s -out %s "
             "-subj /CN=bdk.invalid -days 2 >/dev/null 2>&1",
             key, cert);
    if (system(cmd) != 0)
        return 0;
    g_tls_port = 20000 + (int)(getpid() % 20000);
    snprintf(port, sizeof port, "%d", g_tls_port);
    g_tls_pid = fork();
    if (g_tls_pid == 0) {
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);
        execl("/usr/bin/openssl", "openssl", "s_server", "-accept", port, "-key", key, "-cert",
              cert, "-www", "-quiet", (char*)NULL);
        _exit(1);
    }
    /* Wait until it accepts connections. */
    for (int i = 0; i < 50; ++i) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_port = htons((uint16_t)g_tls_port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int ok = connect(fd, (struct sockaddr*)&a, sizeof a) == 0;
        close(fd);
        if (ok)
            return 1;
        usleep(100000);
    }
    stop_tls_server();
    return 0;
}

/* A page in a window that has loaded the login page, so there is history to go back to. */
static bdk_contents_t page_with_history(bdk_profile_t p) {
    bdk_contents_t c = new_active_page(p);
    bdk_error_t err = 0;
    script_page(LOGIN_URL, "text/html", LOGIN_PAGE, "");
    REQUIRE(bdk_contents_load(c, S(LOGIN_URL), &err) == BDK_OK);
    REQUIRE(wait_for_title(c, "login"));
    return c;
}

static void open_bad_certificate_page(bdk_contents_t c, char* url, size_t n) {
    snprintf(url, n, "https://127.0.0.1:%d/", g_tls_port);
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_load(c, S(url), &err) == BDK_OK);
}

static bdk_ui_response_t reject(void* ctx, const bdk_ui_request_info_t* info) {
    hold(ctx, info);
    return BDK_UI_REJECT;
}

static void test_engine_a_certificate_error_blocks_the_page_with_no_handler(void) {
    if (!start_tls_server()) {
        printf("    (skipped: no openssl to serve a bad certificate)\n");
        return;
    }
    t_start_frozen();
    bdk_contents_t c = page_with_history(t_new_profile("Interstitial"));
    char url[64];
    open_bad_certificate_page(c, url, sizeof url);
    REQUIRE(wait_for_title(c, "Privacy error"));
    for (int i = 0; i < 10; ++i)
        t_pump();
    char title[128];
    title_of(c, title, sizeof title);
    CHECK(strcmp(title, "Privacy error") == 0); /* Chromium's page stays */
    stop_tls_server();
    close_window();
}

static void test_engine_a_certificate_error_request_proceeds_only_with_a_gesture(void) {
    if (!start_tls_server()) {
        printf("    (skipped: no openssl to serve a bad certificate)\n");
        return;
    }
    memset(&g_seen, 0, sizeof g_seen);
    t_start();
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_CERTIFICATE_ERRORS, hold, &g_seen, NULL,
                               &err) == BDK_OK);
    t_freeze();
    bdk_contents_t c = page_with_history(t_new_profile("Interstitial proceed"));
    char url[64];
    open_bad_certificate_page(c, url, sizeof url);
    for (int i = 0; i < 40 && g_seen.calls == 0; ++i)
        t_pump();
    REQUIRE(g_seen.calls == 1);
    CHECK(strstr(g_seen.payload, "\"isOverridable\":true") != NULL);
    CHECK(strstr(g_seen.payload, "\"error\":\"") != NULL);
    CHECK(strstr(g_seen.payload, "127.0.0.1") != NULL);

    err = 0;
    CHECK(bdk_ui_complete(g_seen.request, S("proceed"), S("{}"), no_gesture(), &err) != BDK_OK);
    char title[128];
    title_of(c, title, sizeof title);
    CHECK(strcmp(title, "Privacy error") == 0); /* still blocked */
    err = 0;
    CHECK(bdk_ui_complete(g_seen.request, S("grant"), S("{}"), gesture(), &err) != BDK_OK);
    err = 0;
    CHECK(bdk_ui_complete(g_seen.request, S("proceed"), S("{}"), gesture(), &err) == BDK_OK);
    for (int i = 0; i < 60; ++i) {
        t_pump();
        title_of(c, title, sizeof title);
        if (strcmp(title, "Privacy error") != 0)
            break;
    }
    CHECK(strcmp(title, "Privacy error") != 0); /* the page loaded */
    bdk_ui_request_release(g_seen.request);
    stop_tls_server();
    close_window();
}

/* Pumps until the contents' title is not `title`. */
static int wait_for_title_not(bdk_contents_t c, const char* title) {
    char now[128];
    for (int i = 0; i < 80; ++i) {
        t_pump();
        title_of(c, now, sizeof now);
        if (strcmp(now, title) != 0)
            return 1;
    }
    return 0;
}

static void test_engine_certificate_exceptions_are_listed_and_revoked(void) {
    if (!start_tls_server()) {
        printf("    (skipped: no openssl to serve a bad certificate)\n");
        return;
    }
    memset(&g_seen, 0, sizeof g_seen);
    t_start();
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_CERTIFICATE_ERRORS, hold, &g_seen, NULL,
                               &err) == BDK_OK);
    t_freeze();
    bdk_profile_t profile = t_new_profile("Certificate exceptions");
    bdk_profile_t other = t_new_profile("Certificate exceptions other");
    bdk_contents_t c = page_with_history(profile);
    t_capture_t cap;
    invoke_ok(profile, "permissions.certificateExceptions", "{}", &cap);
    CHECK(strstr(cap.json, "\"hosts\":[]") != NULL);

    /* Proceeding past the error records the host. */
    char url[64];
    open_bad_certificate_page(c, url, sizeof url);
    for (int i = 0; i < 40 && g_seen.calls == 0; ++i)
        t_pump();
    REQUIRE(g_seen.calls == 1);
    err = 0;
    REQUIRE(bdk_ui_complete(g_seen.request, S("proceed"), S("{}"), gesture(), &err) == BDK_OK);
    bdk_ui_request_release(g_seen.request);
    CHECK(wait_for_title_not(c, "Privacy error"));
    invoke_ok(profile, "permissions.certificateExceptions", "{}", &cap);
    CHECK(strstr(cap.json, "\"hosts\":[\"127.0.0.1\"]") != NULL);
    invoke_ok(other, "permissions.certificateExceptions", "{}", &cap);
    CHECK(strstr(cap.json, "\"hosts\":[]") != NULL); /* profiles keep their own */

    /* A revisit goes straight through: no new request. */
    bdk_error_t load_err = 0;
    REQUIRE(bdk_contents_load(c, S(LOGIN_URL), &load_err) == BDK_OK);
    REQUIRE(wait_for_title(c, "login"));
    open_bad_certificate_page(c, url, sizeof url);
    for (int i = 0; i < 20; ++i)
        t_pump();
    CHECK(g_seen.calls == 1);

    /* Revoking the exception brings the interstitial back. */
    invoke_ok(profile, "permissions.revokeCertificateException", "{\"host\":\"127.0.0.1\"}", &cap);
    invoke_ok(profile, "permissions.certificateExceptions", "{}", &cap);
    CHECK(strstr(cap.json, "\"hosts\":[]") != NULL);
    invoke(profile, "permissions.revokeCertificateException", "{\"host\":\"\"}", &cap);
    CHECK(cap.has_error && strcmp(cap.code, "invalidArguments") == 0);
    REQUIRE(bdk_contents_load(c, S(LOGIN_URL), &load_err) == BDK_OK);
    REQUIRE(wait_for_title(c, "login"));
    open_bad_certificate_page(c, url, sizeof url);
    for (int i = 0; i < 40 && g_seen.calls < 2; ++i)
        t_pump();
    CHECK(g_seen.calls == 2);
    bdk_ui_request_release(g_seen.request);
    stop_tls_server();
    close_window();
}

static void test_engine_a_rejected_certificate_error_request_goes_back(void) {
    if (!start_tls_server()) {
        printf("    (skipped: no openssl to serve a bad certificate)\n");
        return;
    }
    memset(&g_seen, 0, sizeof g_seen);
    t_start();
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_CERTIFICATE_ERRORS, reject, &g_seen, NULL,
                               &err) == BDK_OK);
    t_freeze();
    bdk_contents_t c = page_with_history(t_new_profile("Interstitial reject"));
    char url[64];
    open_bad_certificate_page(c, url, sizeof url);
    for (int i = 0; i < 40 && g_seen.calls == 0; ++i)
        t_pump();
    CHECK(g_seen.calls == 1);
    for (int i = 0; i < 20; ++i)
        t_pump();
    CHECK(wait_for_title(c, "login")); /* going back, never proceeding */
    stop_tls_server();
    close_window();
}

static void test_engine_a_dropped_certificate_error_request_goes_back(void) {
    if (!start_tls_server()) {
        printf("    (skipped: no openssl to serve a bad certificate)\n");
        return;
    }
    memset(&g_seen, 0, sizeof g_seen);
    t_start();
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_CERTIFICATE_ERRORS, hold, &g_seen, NULL,
                               &err) == BDK_OK);
    t_freeze();
    bdk_contents_t c = page_with_history(t_new_profile("Interstitial drop"));
    char url[64];
    open_bad_certificate_page(c, url, sizeof url);
    for (int i = 0; i < 40 && g_seen.calls == 0; ++i)
        t_pump();
    REQUIRE(g_seen.calls == 1);
    bdk_ui_request_release(g_seen.request); /* taken and released without completing */
    for (int i = 0; i < 20; ++i)
        t_pump();
    CHECK(wait_for_title(c, "login"));
    stop_tls_server();
    close_window();
}

/* ---- autofill save prompts and suggestions (T-UIR-04) ---- */

#define ADDRESS_URL "https://addr.example/"
#define FIELD "position:absolute;left:20px;width:240px;height:28px;top:"
/* An address form whose fields Chromium recognises from their autocomplete attributes. The title
   follows the street field, so a test can see a fill. */
#define ADDRESS_PAGE                                                                               \
    "<!doctype html><title>empty</title><form id=f method=post action=/done>"                      \
    "<input id=n name=n autocomplete=name style='" FIELD "20px'>"                                  \
    "<input id=s name=s autocomplete=address-line1 style='" FIELD "60px'>"                         \
    "<input id=c name=c autocomplete=address-level2 style='" FIELD "100px'>"                       \
    "<input id=r name=r autocomplete=address-level1 style='" FIELD "140px'>"                       \
    "<input id=z name=z autocomplete=postal-code style='" FIELD "180px'>"                          \
    "<input id=e name=e autocomplete=email style='" FIELD "220px'></form>"                         \
    "<script>setInterval(function(){document.title="                                               \
    "document.getElementById('s').value||'empty'},50)</script>"

static void submit_address(bdk_contents_t c) {
    run_script(c,
               "document.getElementById('n').value='Ada Lovelace';"
               "document.getElementById('s').value='1 Main St';"
               "document.getElementById('c').value='Springfield';"
               "document.getElementById('r').value='IL';"
               "document.getElementById('z').value='62701';"
               "document.getElementById('e').value='ada@example.com';"
               "document.getElementById('f').requestSubmit();",
               1);
}

/* Clicks the name field of the address form, as the person would to get suggestions. */
static void click_name_field(bdk_contents_t c) {
    REQUIRE(bdk_testing_click(c, 100, 34, 0) == BDK_OK);
    t_pump();
}

static seen_t g_suggestions;

/* Rejects what it is asked. */
static bdk_ui_response_t reject_request(void* ctx, const bdk_ui_request_info_t* info) {
    (void)info;
    ++((seen_t*)ctx)->calls;
    return BDK_UI_REJECT;
}

static int saved_addresses_contain(bdk_profile_t p, const char* text) {
    t_capture_t cap;
    invoke(p, "autofill.addresses.list", "{}", &cap);
    return !cap.has_error && strstr(cap.json, text) != NULL;
}

static int wait_for_saved_address(bdk_profile_t p, const char* text) {
    for (int i = 0; i < 60; ++i) {
        if (saved_addresses_contain(p, text))
            return 1;
        t_pump();
        bdk_testing_run_for(0.1);
    }
    return 0;
}

static void wait_for_calls(const seen_t* seen, int calls) {
    for (int i = 0; i < 60 && seen->calls < calls; ++i) {
        t_pump();
        bdk_testing_run_for(0.1);
    }
}

/* Loads the address form into `c` and waits until it is there. */
static void load_address_form(bdk_contents_t c) {
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_load(c, S(ADDRESS_URL), &err) == BDK_OK);
    t_pump();
    bdk_testing_run_for(0.3);
}

/* The decimal id of the first suggestion in a request payload. */
static int first_suggestion_id(const char* payload, char* out, size_t n) {
    const char* at = strstr(payload, "\"id\":\"");
    if (!at)
        return 0;
    at += strlen("\"id\":\"");
    size_t len = 0;
    while (at[len] && at[len] != '"' && len + 1 < n) {
        out[len] = at[len];
        ++len;
    }
    out[len] = '\0';
    return len > 0;
}

static void test_engine_a_submitted_address_is_saved_with_a_gesture_and_offered_next_time(void) {
    memset(&g_seen, 0, sizeof g_seen);
    memset(&g_suggestions, 0, sizeof g_suggestions);
    t_start();
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_AUTOFILL, hold, &g_seen, NULL, &err) ==
            BDK_OK);
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_AUTOFILL_SUGGESTIONS, hold, &g_suggestions,
                               NULL, &err) == BDK_OK);
    t_freeze();
    script_page(ADDRESS_URL, "text/html", ADDRESS_PAGE, "");
    script_page(ADDRESS_URL "done", "text/html", DONE_PAGE, "");
    bdk_profile_t p = t_new_profile("Address save");
    bdk_contents_t c = new_active_page(p);
    load_address_form(c);
    submit_address(c);
    REQUIRE(wait_for_title(c, "welcome"));
    wait_for_calls(&g_seen, 1);
    REQUIRE(g_seen.calls == 1);
    CHECK(strstr(g_seen.payload, "\"kind\":\"address\"") != NULL);
    CHECK(strstr(g_seen.payload, "\"isUpdate\":false") != NULL);
    CHECK(strstr(g_seen.payload, "1 Main St") != NULL);
    CHECK(!saved_addresses_contain(p, "1 Main St")); /* nothing saved before the person says so */

    err = 0;
    CHECK(bdk_ui_complete(g_seen.request, S("grant"), S("{}"), no_gesture(), &err) != BDK_OK);
    CHECK(!saved_addresses_contain(p, "1 Main St"));
    err = 0;
    CHECK(bdk_ui_complete(g_seen.request, S("grant"), S("{}"), gesture(), &err) == BDK_OK);
    CHECK(wait_for_saved_address(p, "1 Main St"));
    bdk_ui_request_release(g_seen.request);

    /* The next form offers it. */
    load_address_form(c);
    CHECK(wait_for_title(c, "empty"));
    click_name_field(c);
    wait_for_calls(&g_suggestions, 1);
    REQUIRE(g_suggestions.calls == 1);
    CHECK(strstr(g_suggestions.payload, "Ada Lovelace") != NULL);
    CHECK(strstr(g_suggestions.payload, "\"fieldRect\"") != NULL);
    char id[16];
    REQUIRE(first_suggestion_id(g_suggestions.payload, id, sizeof id));
    char args[64];
    snprintf(args, sizeof args, "{\"suggestion\":\"%s\"}", id);
    bdk_testing_run_for(0.8); /* Chromium ignores a choice made at once */
    err = 0;
    CHECK(bdk_ui_complete(g_suggestions.request, S("grant"), S(args), no_gesture(), &err) !=
          BDK_OK);
    CHECK(wait_for_title(c, "empty")); /* refused: nothing filled */
    err = 0;
    CHECK(bdk_ui_complete(g_suggestions.request, S("grant"), S(args), gesture(), &err) == BDK_OK);
    CHECK(wait_for_title(c, "1 Main St"));
    bdk_ui_request_release(g_suggestions.request);
    close_window();
}

/* Runs an address submission with `handler` for the save prompt, completing the request it holds
   with "deny" when `complete_with_deny` is set, and checks that nothing was saved. */
static void submit_address_and_check_nothing_is_saved(bdk_ui_handler_cb handler,
                                                      int complete_with_deny, const char* name) {
    memset(&g_seen, 0, sizeof g_seen);
    t_start();
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_AUTOFILL, handler, &g_seen, NULL, &err) ==
            BDK_OK);
    t_freeze();
    script_page(ADDRESS_URL, "text/html", ADDRESS_PAGE, "");
    script_page(ADDRESS_URL "done", "text/html", DONE_PAGE, "");
    bdk_profile_t p = t_new_profile(name);
    bdk_contents_t c = new_active_page(p);
    load_address_form(c);
    submit_address(c);
    REQUIRE(wait_for_title(c, "welcome"));
    wait_for_calls(&g_seen, 1);
    REQUIRE(g_seen.calls == 1);
    if (complete_with_deny) {
        err = 0;
        CHECK(bdk_ui_complete(g_seen.request, S("deny"), S("{}"), no_gesture(), &err) == BDK_OK);
        bdk_ui_request_release(g_seen.request);
    }
    bdk_testing_run_for(1.0);
    CHECK(!saved_addresses_contain(p, "1 Main St"));
    close_window();
}

static void test_engine_a_rejected_address_prompt_saves_nothing(void) {
    submit_address_and_check_nothing_is_saved(reject_request, 0, "Address reject");
}

static void test_engine_a_declined_address_prompt_saves_nothing(void) {
    submit_address_and_check_nothing_is_saved(hold, 1, "Address decline");
}

static void test_engine_the_default_address_prompt_saves_and_suggestions_use_chromiums_popup(void) {
    t_start();
    t_freeze();
    script_page(ADDRESS_URL, "text/html", ADDRESS_PAGE, "");
    script_page(ADDRESS_URL "done", "text/html", DONE_PAGE, "");
    script_presenter("autofill", "save");
    bdk_profile_t p = t_new_profile("Address default");
    bdk_contents_t c = new_active_page(p);
    load_address_form(c);
    submit_address(c);
    REQUIRE(wait_for_title(c, "welcome"));
    CHECK(wait_for_saved_address(p, "1 Main St"));
    CHECK(presenter_uses("autofill") == 1);

    /* No handler, no script: Chromium's own popup, built for the page's view. */
    const int popups_before = presenter_uses("autofill_suggestions");
    load_address_form(c);
    click_name_field(c);
    for (int i = 0; i < 30 && presenter_uses("autofill_suggestions") == popups_before; ++i) {
        t_pump();
        bdk_testing_run_for(0.1);
    }
    CHECK(presenter_uses("autofill_suggestions") == popups_before + 1);
    close_window();
}

static void test_engine_a_scripted_suggestion_fills_the_form(void) {
    t_start();
    t_freeze();
    script_page(ADDRESS_URL, "text/html", ADDRESS_PAGE, "");
    script_page(ADDRESS_URL "done", "text/html", DONE_PAGE, "");
    script_presenter("autofill", "save");
    bdk_profile_t p = t_new_profile("Address scripted");
    bdk_contents_t c = new_active_page(p);
    load_address_form(c);
    submit_address(c);
    REQUIRE(wait_for_title(c, "welcome"));
    CHECK(wait_for_saved_address(p, "1 Main St"));

    script_presenter("autofill_suggestions", "Ada Lovelace");
    load_address_form(c);
    CHECK(wait_for_title(c, "empty"));
    click_name_field(c);
    CHECK(wait_for_title(c, "1 Main St"));
    CHECK(presenter_uses("autofill_suggestions") >= 1);
    close_window();
}

static void test_engine_disabled_autofill_offers_nothing_and_fills_nothing(void) {
    memset(&g_seen, 0, sizeof g_seen);
    memset(&g_suggestions, 0, sizeof g_suggestions);
    bdk_harness_engine_surfaces = "{\"autofill\":\"disabled\"}";
    t_start();
    bdk_harness_engine_surfaces = NULL;
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_AUTOFILL, hold, &g_seen, NULL, &err) ==
            BDK_OK);
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_AUTOFILL_SUGGESTIONS, hold, &g_suggestions,
                               NULL, &err) == BDK_OK);
    t_freeze();
    script_page(ADDRESS_URL, "text/html", ADDRESS_PAGE, "");
    script_page(ADDRESS_URL "done", "text/html", DONE_PAGE, "");
    bdk_profile_t p = t_new_profile("Address disabled");
    t_capture_t cap;
    invoke_ok(p, "autofill.addresses.add",
              "{\"fields\":{\"fullName\":\"Grace Hopper\",\"street\":\"9 Elm St\","
              "\"city\":\"Springfield\",\"region\":\"IL\",\"postalCode\":\"62701\","
              "\"country\":\"US\"}}",
              &cap);
    bdk_contents_t c = new_active_page(p);
    load_address_form(c);
    submit_address(c);
    REQUIRE(wait_for_title(c, "welcome"));
    bdk_testing_run_for(1.5);
    CHECK(g_seen.calls == 0); /* no save prompt */
    CHECK(!saved_addresses_contain(p, "1 Main St"));

    const int popups_before = presenter_uses("autofill_suggestions");
    load_address_form(c);
    click_name_field(c);
    bdk_testing_run_for(1.5);
    CHECK(g_suggestions.calls == 0);                                /* no request */
    CHECK(presenter_uses("autofill_suggestions") == popups_before); /* no popup */
    CHECK(wait_for_title(c, "empty"));                              /* nothing filled */
    close_window();
}

static void test_engine_a_certificate_error_surface_cannot_be_disabled(void) {
    bdk_start_config_t config;
    memset(&config, 0, sizeof config);
    t_fill_start_config(&config);
    config.resolved_config_json =
        S("{\"browser\":{\"name\":\"BDK Conformance\"},\"surfaces\":{\"certificate_errors\":"
          "\"disabled\"}}");
    bdk_error_t err = 0;
    bdk_runtime_t rt = 0;
    CHECK(bdk_runtime_start(&config, &rt, &err) != BDK_OK);
    CHECK(rt == 0);
}

/* ---- service events: cookies, passwords and autofill (T-SVC-07) ---- */

/* The payloads of the events of one name a subscription received, one per line. */
typedef struct {
    int count;
    char payloads[4096];
} event_log_t;

static void on_service_event(void* ctx, uint32_t id, bdk_string_t name, bdk_string_t payload) {
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
    REQUIRE(bdk_subscribe_named(t_runtime, S(event), S(""), &o, on_service_event, log, NULL) != 0);
}

/* Pumps, in real time, until `text` is in the log's payloads. */
static int wait_for_payload(const event_log_t* log, const char* text) {
    for (int i = 0; i < 100 && !strstr(log->payloads, text); ++i) {
        t_pump();
        bdk_testing_run_for(0.05);
    }
    if (!strstr(log->payloads, text))
        printf("    no event with \"%s\"; got:\n%s", text, log->payloads);
    return strstr(log->payloads, text) != NULL;
}

static void test_engine_cookie_changes_are_reported_with_their_values(void) {
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Cookie events");
    bdk_profile_t other = t_new_profile("Other cookie events");
    event_log_t log, other_log;
    subscribe_to("cookies.changed", p, &log);
    subscribe_to("cookies.changed", other, &other_log);
    t_capture_t cap;
    invoke_ok(p, "cookies.set",
              "{\"cookie\":{\"name\":\"sid\",\"value\":\"v1\",\"domain\":\"cookies.example\","
              "\"path\":\"/\",\"secure\":true,\"httpOnly\":true,\"sameSite\":\"lax\","
              "\"expires\":4102444800000}}",
              &cap);
    CHECK(wait_for_payload(&log, "\"value\":\"v1\""));
    CHECK(strstr(log.payloads, "\"name\":\"sid\"") != NULL &&
          strstr(log.payloads, "\"domain\":\".cookies.example\"") != NULL &&
          strstr(log.payloads, "\"removed\":false") != NULL &&
          strstr(log.payloads, "\"source\":\"application\"") != NULL);
    CHECK(strstr(log.payloads, "\"secure\":true") != NULL &&
          strstr(log.payloads, "\"httpOnly\":true") != NULL &&
          strstr(log.payloads, "\"sameSite\":\"lax\"") != NULL);

    /* Replacing a cookie is one change, not a removal and an insertion. */
    invoke_ok(p, "cookies.set",
              "{\"cookie\":{\"name\":\"sid\",\"value\":\"v2\",\"domain\":\"cookies.example\","
              "\"path\":\"/\",\"secure\":true,\"httpOnly\":true,\"sameSite\":\"lax\"}}",
              &cap);
    CHECK(wait_for_payload(&log, "\"value\":\"v2\""));
    CHECK(strstr(log.payloads, "\"removed\":true") == NULL);

    invoke_ok(p, "cookies.remove",
              "{\"name\":\"sid\",\"domain\":\".cookies.example\",\"path\":\"/\"}", &cap);
    CHECK(wait_for_payload(&log, "\"removed\":true"));
    CHECK(other_log.count == 0); /* another profile's subscription hears nothing */
}

#define COOKIE_PAGE_URL "https://cookie-page.example/"

static void test_engine_a_cookie_set_by_a_page_is_reported(void) {
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Page cookie events");
    event_log_t log;
    subscribe_to("cookies.changed", p, &log);
    script_page(COOKIE_PAGE_URL, "text/html", "<!doctype html><title>cookie page</title>", "");
    bdk_contents_t c = new_active_page(p);
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_load(c, S(COOKIE_PAGE_URL), &err) == BDK_OK);
    REQUIRE(wait_for_title(c, "cookie page"));
    run_script(c, "document.cookie='seen=by-page; path=/'", 0);
    CHECK(wait_for_payload(&log, "\"name\":\"seen\""));
    CHECK(strstr(log.payloads, "\"value\":\"by-page\"") != NULL &&
          strstr(log.payloads, "\"domain\":\"cookie-page.example\"") != NULL &&
          strstr(log.payloads, "\"removed\":false") != NULL);
    close_window();
}

static void test_engine_password_changes_are_reported(void) {
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Password events");
    event_log_t changed, issues;
    subscribe_to("passwords.changed", p, &changed);
    subscribe_to("passwords.issuesChanged", p, &issues);
    t_capture_t cap;
    invoke_ok(p, "passwords.add",
              "{\"origin\":\"https://events.example\",\"username\":\"ada\","
              "\"password\":\"s3cret\"}",
              &cap);
    char id[32], args[160];
    REQUIRE(t_json_string(cap.json, "id", id, sizeof id));
    CHECK(wait_for_payload(&changed, "\"change\":\"added\""));
    char want[96];
    snprintf(want, sizeof want, "\"id\":\"%s\"", id);
    CHECK(strstr(changed.payloads, want) != NULL &&
          strstr(changed.payloads, "\"source\":\"application\"") != NULL);
    CHECK(strstr(changed.payloads, "s3cret") == NULL); /* never in an event */

    set_authentication("allow");
    snprintf(args, sizeof args, "{\"id\":\"%s\",\"password\":\"n3w\"}", id);
    invoke_ok(p, "passwords.update", args, &cap);
    CHECK(wait_for_payload(&changed, "\"change\":\"updated\""));

    snprintf(args, sizeof args, "{\"id\":\"%s\"}", id);
    invoke_ok(p, "passwords.remove", args, &cap);
    CHECK(wait_for_payload(&changed, "\"change\":\"removed\""));
    /* No command flags a login as insecure without a leak-check provider (M6), so the issues
       never change here. */
    CHECK(issues.count == 0);
}

static void test_engine_autofill_changes_are_reported(void) {
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Autofill events");
    event_log_t log;
    subscribe_to("autofill.changed", p, &log);
    t_capture_t cap;
    invoke_ok(p, "autofill.addresses.add",
              "{\"fields\":{\"fullName\":\"Ada Lovelace\",\"city\":\"London\","
              "\"country\":\"GB\"}}",
              &cap);
    CHECK(wait_for_payload(&log, "\"kind\":\"address\""));
    CHECK(strstr(log.payloads, "\"source\":\"application\"") != NULL);
    CHECK(strstr(log.payloads, "Lovelace") == NULL); /* the event carries no data */
    invoke_ok(p, "autofill.cards.add",
              "{\"card\":{\"holder\":\"Ada Lovelace\",\"number\":\"4111111111111111\","
              "\"expiryMonth\":12,\"expiryYear\":2099}}",
              &cap);
    CHECK(wait_for_payload(&log, "\"kind\":\"card\""));
    CHECK(strstr(log.payloads, "4111") == NULL);
}

const t_test_t t_engine_prompt_tests[] = {
    {"engine_window_print_raises_a_printing_request_for_a_handler",
     test_engine_window_print_raises_a_printing_request_for_a_handler},
    {"engine_the_application_prints_for_a_handler",
     test_engine_the_application_prints_for_a_handler},
    {"engine_the_application_prints_with_no_handler_through_the_default",
     test_engine_the_application_prints_with_no_handler_through_the_default},
    {"engine_window_print_with_no_handler_uses_the_system_print_panel",
     test_engine_window_print_with_no_handler_uses_the_system_print_panel},
    {"engine_a_download_destination_is_chosen_by_the_handler",
     test_engine_a_download_destination_is_chosen_by_the_handler},
    {"engine_the_default_download_presenter_saves_where_the_user_says",
     test_engine_the_default_download_presenter_saves_where_the_user_says},
    {"engine_a_dangerous_download_is_kept_only_with_a_gesture",
     test_engine_a_dangerous_download_is_kept_only_with_a_gesture},
    {"engine_a_dropped_dangerous_download_is_discarded",
     test_engine_a_dropped_dangerous_download_is_discarded},
    {"engine_a_certificate_error_blocks_the_page_with_no_handler",
     test_engine_a_certificate_error_blocks_the_page_with_no_handler},
    {"engine_a_certificate_error_request_proceeds_only_with_a_gesture",
     test_engine_a_certificate_error_request_proceeds_only_with_a_gesture},
    {"engine_certificate_exceptions_are_listed_and_revoked",
     test_engine_certificate_exceptions_are_listed_and_revoked},
    {"engine_a_rejected_certificate_error_request_goes_back",
     test_engine_a_rejected_certificate_error_request_goes_back},
    {"engine_a_dropped_certificate_error_request_goes_back",
     test_engine_a_dropped_certificate_error_request_goes_back},
    {"engine_a_certificate_error_surface_cannot_be_disabled",
     test_engine_a_certificate_error_surface_cannot_be_disabled},
    {"engine_a_submitted_login_is_saved_by_the_default_prompt_and_filled_next_time",
     test_engine_a_submitted_login_is_saved_by_the_default_prompt_and_filled_next_time},
    {"engine_a_password_prompt_reaches_a_handler_and_saving_needs_a_gesture",
     test_engine_a_password_prompt_reaches_a_handler_and_saving_needs_a_gesture},
    {"engine_declining_the_password_prompt_saves_nothing",
     test_engine_declining_the_password_prompt_saves_nothing},
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
    {"engine_cookie_changes_are_reported_with_their_values",
     test_engine_cookie_changes_are_reported_with_their_values},
    {"engine_a_cookie_set_by_a_page_is_reported", test_engine_a_cookie_set_by_a_page_is_reported},
    {"engine_password_changes_are_reported", test_engine_password_changes_are_reported},
    {"engine_autofill_changes_are_reported", test_engine_autofill_changes_are_reported},
    {"engine_ui_autofill_a_submitted_address_is_saved_with_a_gesture_and_offered_next_time",
     test_engine_a_submitted_address_is_saved_with_a_gesture_and_offered_next_time},
    {"engine_ui_autofill_a_rejected_address_prompt_saves_nothing",
     test_engine_a_rejected_address_prompt_saves_nothing},
    {"engine_ui_autofill_a_declined_address_prompt_saves_nothing",
     test_engine_a_declined_address_prompt_saves_nothing},
    {"engine_ui_autofill_the_default_prompt_saves_and_suggestions_use_chromiums_popup",
     test_engine_the_default_address_prompt_saves_and_suggestions_use_chromiums_popup},
    {"engine_ui_autofill_a_scripted_suggestion_fills_the_form",
     test_engine_a_scripted_suggestion_fills_the_form},
    {"engine_ui_autofill_disabled_offers_nothing_and_fills_nothing",
     test_engine_disabled_autofill_offers_nothing_and_fills_nothing},
    {NULL, NULL},
};
