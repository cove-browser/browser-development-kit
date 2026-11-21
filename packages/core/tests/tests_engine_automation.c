/* Scenarios for automation and DevTools that only a real engine can run (M7): scripts in the V8
   automation host (T-AUT-01, T-AUT-02), page operations through BDK's protocol client (T-AUT-03),
   captures (T-SVC-09) and the DevTools front ends (T-AUT-05). Built only into the engine
   conformance app (BDK_HARNESS_ENGINE_TESTS). The scripts and front ends are fixtures packaged into
   the app's bundle by CMakeLists.txt, which passes their digests (tests/fixtures/automation). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bdk_testing.h"
#include "test_support.h"

#ifndef BDK_TEST_ACCEPTANCE_DIGEST
#define BDK_TEST_ACCEPTANCE_DIGEST "sha256:unset"
#define BDK_TEST_WAIT_DIGEST "sha256:unset"
#define BDK_TEST_FAIL_DIGEST "sha256:unset"
#endif

#define SCRIPT(name, digest)                                                                       \
    "\"" name "\":{\"path\":\"Automation/" name ".ts\",\"digest\":\"sha256:0\",\"bundle\":"        \
    "\"BDK/Automation/" name ".js\",\"bundle_digest\":\"" digest "\"}"

#define AUTOMATION                                                                                                                                                                        \
    "\"automation\":{\"enabled\":true,\"external\":{\"enabled\":false},\"scripts\":{" SCRIPT(                                                                                             \
        "acceptance",                                                                                                                                                                     \
        BDK_TEST_ACCEPTANCE_DIGEST) "," SCRIPT("wait",                                                                                                                                    \
                                               BDK_TEST_WAIT_DIGEST) "," SCRIPT("fail",                                                                                                   \
                                                                                BDK_TEST_FAIL_DIGEST) ","                                                                                 \
                                                                                                      "\"tampered\":{\"path\":\"Automation/fail.ts\",\"digest\":\"sha256:0\",\"bundle\":" \
                                                                                                      "\"BDK/Automation/fail.js\",\"bundle_digest\":\"sha256:0000\"},"                    \
                                                                                                      "\"unbuilt\":{\"path\":\"Automation/fail.ts\",\"digest\":\"sha256:0\"}}}"

/* ---- waiting on the engine ---- */

static void settle(void) {
    t_pump();
    bdk_testing_run_for(0.05);
}

static int wait_until(const int* flag) {
    for (int i = 0; i < 300 && !*flag; ++i)
        settle();
    return *flag;
}

static void control(const char* verb, const char* args, char* out, size_t n) {
    REQUIRE(bdk_testing_command(t_runtime, S(verb), S(args), out, n) == BDK_OK);
}

static void script_page(const char* url, const char* html) {
    static char args[8192];
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
    for (int i = 0; i < 100; ++i) {
        title_of(c, title, sizeof title);
        if (strcmp(title, want) == 0)
            return 1;
        settle();
    }
    printf("    title is \"%s\", not \"%s\"\n", title, want);
    return 0;
}

/* ---- calls from the host ---- */

typedef struct big_capture {
    int results, has_error, has_binary;
    char code[64], message[256];
    char json[8192];
    unsigned char head[8];
    size_t bytes;
} big_capture_t;

static void on_big_result(void* ctx, bdk_error_t error, bdk_string_t json, bdk_buffer_t binary) {
    big_capture_t* c = (big_capture_t*)ctx;
    ++c->results;
    c->has_error = error != 0;
    if (error) {
        bdk_string_t k = bdk_error_code(error), m = bdk_error_message(error);
        snprintf(c->code, sizeof c->code, "%.*s", (int)k.length, k.data);
        snprintf(c->message, sizeof c->message, "%.*s", (int)m.length, m.data);
    }
    snprintf(c->json, sizeof c->json, "%.*s", (int)json.length, json.data ? json.data : "");
    if (binary) {
        bdk_bytes_t b = bdk_buffer_bytes(binary);
        c->has_binary = 1;
        c->bytes = b.length;
        memcpy(c->head, b.data, b.length < sizeof c->head ? b.length : sizeof c->head);
    }
}

static void call(const char* command, const char* args, bdk_profile_t profile,
                 bdk_contents_t contents, big_capture_t* cap) {
    memset(cap, 0, sizeof *cap);
    bdk_invoke_options_t o;
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.profile = profile;
    o.contents = contents;
    REQUIRE(bdk_invoke_named(t_runtime, S(command), S(args), &o, on_big_result, NULL, cap, NULL) !=
            0);
    wait_until(&cap->results);
    if (cap->has_error)
        printf("    %s: %s (%s)\n", command, cap->code, cap->message);
}

static void profile_id(bdk_profile_t p, char* out, size_t n) {
    big_capture_t cap;
    call("profiles.current", "{}", p, 0, &cap);
    REQUIRE(t_json_string(cap.json, "id", out, n));
}

static bdk_contents_t page_at(bdk_profile_t p, const char* url) {
    bdk_contents_t c = t_new_contents(p);
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_load(c, S(url), &err) == BDK_OK);
    big_capture_t cap;
    call("contents.waitFor", "{\"load\":\"finished\",\"timeoutMs\":10000}", 0, c, &cap);
    CHECK(!cap.has_error);
    return c;
}

/* ---- tasks ---- */

typedef struct task_log {
    int finished, logs, commands;
    char finish[4096];
} task_log_t;

static void on_task(void* ctx, bdk_task_t task, uint32_t kind, bdk_string_t detail) {
    (void)task;
    task_log_t* log = (task_log_t*)ctx;
    if (kind == BDK_TASK_EVENT_LOG)
        ++log->logs;
    if (kind == BDK_TASK_EVENT_COMMAND)
        ++log->commands;
    if (kind == BDK_TASK_EVENT_FINISHED) {
        snprintf(log->finish, sizeof log->finish, "%.*s", (int)detail.length, detail.data);
        log->finished = 1;
    }
}

static bdk_task_t run(bdk_session_t s, const char* script, const char* args, task_log_t* log) {
    memset(log, 0, sizeof *log);
    bdk_task_t task = 0;
    bdk_error_t err = 0;
    REQUIRE(bdk_session_run(s, S(script), S(args), &task, &err) == BDK_OK);
    REQUIRE(bdk_task_subscribe(task, on_task, log, NULL) != 0);
    return task;
}

static int g_notes;
static char g_note_input[256];

static void on_note(void* ctx, bdk_context_t call_context, bdk_string_t input) {
    (void)ctx;
    ++g_notes;
    snprintf(g_note_input, sizeof g_note_input, "%.*s", (int)input.length, input.data);
    bdk_app_command_resolve(call_context, S("{\"ok\":true}"));
}

static bdk_session_t start_session(const char* capabilities, char* pid, size_t n,
                                   bdk_profile_t* out_profile) {
    t_config_extra = AUTOMATION;
    t_start();
    bdk_error_t err = 0;
    g_notes = 0;
    REQUIRE(bdk_app_command_register(t_runtime, S("app.notes.create"), S("{}"), on_note, NULL, NULL,
                                     &err) == BDK_OK);
    t_freeze();
    bdk_profile_t p = t_new_profile("Automation");
    profile_id(p, pid, n);
    char options[512];
    snprintf(options, sizeof options,
             "{\"name\":\"agent\",\"capabilities\":%s,\"scope\":{\"profiles\":[\"%s\"]}}",
             capabilities, pid);
    bdk_session_t s = 0;
    REQUIRE(bdk_session_create(t_runtime, S(options), &s, &err) == BDK_OK);
    *out_profile = p;
    return s;
}

static void serve_two_pages(void) {
    script_page("https://a.bdk.test/", "<title>A</title><script>var pageVar = 1;</script>");
    script_page("https://b.bdk.test/", "<title>Page B</title>");
}

/* T-AUT-02's acceptance: with no page open, a script drives two pages, runs in an isolated world,
   receives events and calls a registered command. */
static void test_engine_a_script_drives_two_pages(void) {
    char pid[64];
    bdk_profile_t p = 0;
    bdk_session_t s = start_session(
        "[\"contents.read\",\"contents.control\",\"contents.script\",\"app.notes.create\"]", pid,
        sizeof pid, &p);
    serve_two_pages();
    static task_log_t log;
    (void)run(s, "acceptance", "{\"a\":\"https://a.bdk.test/\",\"b\":\"https://b.bdk.test/\"}",
              &log);
    REQUIRE(wait_until(&log.finished));
    printf("    finished: %s\n", log.finish);
    CHECK(strstr(log.finish, "\"result\":") != NULL);
    CHECK(strstr(log.finish, "\"navigations\":2") != NULL);
    CHECK(strstr(log.finish, "\"isolated\":\"undefined\"") !=
          NULL); /* the page's var is not seen */
    CHECK(strstr(log.finish, "\"page\":\"number\"") != NULL);
    CHECK(strstr(log.finish, "\"title\":\"Page B\"") != NULL);
    CHECK(strstr(log.finish, "\"note\":{\"ok\":true}") != NULL);
    CHECK(g_notes == 1 && strstr(g_note_input, "Page B") != NULL);
    CHECK(log.logs >= 1);     /* console.log reached the task's events */
    CHECK(log.commands >= 8); /* every call was audited */
}

static void test_engine_a_session_without_a_capability_is_refused(void) {
    char pid[64];
    bdk_profile_t p = 0;
    bdk_session_t s =
        start_session("[\"contents.read\",\"contents.control\"]", pid, sizeof pid, &p);
    serve_two_pages();
    static task_log_t log;
    (void)run(s, "acceptance", "{\"a\":\"https://a.bdk.test/\",\"b\":\"https://b.bdk.test/\"}",
              &log);
    REQUIRE(wait_until(&log.finished));
    CHECK(strstr(log.finish, "\"code\":\"scriptFailed\"") != NULL);
    CHECK(strstr(log.finish, "capability.denied") != NULL);
}

static void test_engine_cancel_and_revoke_end_a_running_script(void) {
    char pid[64];
    bdk_profile_t p = 0;
    bdk_session_t s = start_session("[]", pid, sizeof pid, &p);
    static task_log_t log;
    bdk_task_t task = run(s, "wait", "null", &log);
    for (int i = 0; i < 40 && log.logs == 0; ++i)
        settle(); /* the script is running: its timer logged */
    CHECK(log.logs >= 1);
    bdk_error_t err = 0;
    CHECK(bdk_task_cancel(task, &err) == BDK_OK);
    REQUIRE(wait_until(&log.finished));
    CHECK(strstr(log.finish, "\"code\":\"cancelled\"") != NULL);
    static task_log_t second;
    (void)run(s, "wait", "null", &second);
    settle();
    CHECK(bdk_session_revoke(s, &err) == BDK_OK);
    REQUIRE(wait_until(&second.finished));
    CHECK(strstr(second.finish, "\"code\":\"sessionRevoked\"") != NULL);
}

static void test_engine_a_failing_or_tampered_script_finishes_with_why(void) {
    char pid[64];
    bdk_profile_t p = 0;
    bdk_session_t s = start_session("[]", pid, sizeof pid, &p);
    static task_log_t fail, tampered, unbuilt;
    (void)run(s, "fail", "null", &fail);
    (void)run(s, "tampered", "null", &tampered);
    (void)run(s, "unbuilt", "null", &unbuilt);
    REQUIRE(wait_until(&fail.finished));
    REQUIRE(wait_until(&tampered.finished));
    REQUIRE(wait_until(&unbuilt.finished));
    CHECK(strstr(fail.finish, "\"code\":\"scriptFailed\"") != NULL);
    CHECK(strstr(fail.finish, "boom") != NULL);
    CHECK(strstr(tampered.finish, "\"code\":\"scriptFailed\"") != NULL);
    CHECK(strstr(tampered.finish, "digest") != NULL);
    CHECK(strstr(unbuilt.finish, "\"code\":\"scriptFailed\"") != NULL);
    CHECK(strstr(unbuilt.finish, "bdk build automation") != NULL);
}

/* ---- page operations (T-AUT-03), each on two pages ---- */

static void start_plain(void) {
    t_start_frozen();
}

#define FORM_PAGE                                                                                  \
    "<title>form</title><script>var pageVar = 'page';</script>"                                    \
    "<button id=go onclick=\"document.title='clicked'\">Go</button>"                               \
    "<input id=field><p>Some visible text</p><div style='height:3000px'></div>"                    \
    "<iframe srcdoc=\"<p id=inner>inside</p>\"></iframe>"

static void test_engine_scripts_run_in_worlds_and_frames_on_two_pages(void) {
    start_plain();
    bdk_profile_t p = t_new_profile("Pages");
    script_page("https://one.bdk.test/", FORM_PAGE);
    script_page("https://two.bdk.test/", FORM_PAGE);
    bdk_contents_t pages[2] = {page_at(p, "https://one.bdk.test/"),
                               page_at(p, "https://two.bdk.test/")};
    for (int i = 0; i < 2; ++i) {
        bdk_contents_t c = pages[i];
        big_capture_t cap;
        call("contents.evaluate", "{\"expression\":\"typeof pageVar\"}", 0, c, &cap);
        CHECK(strcmp(cap.json, "{\"value\":\"undefined\"}") == 0);
        call("contents.evaluate", "{\"expression\":\"pageVar\",\"world\":\"page\"}", 0, c, &cap);
        CHECK(strcmp(cap.json, "{\"value\":\"page\"}") == 0);
        call("contents.evaluate", "{\"expression\":\"window.mine = 41; 1\",\"world\":\"w\"}", 0, c,
             &cap);
        call("contents.evaluate", "{\"expression\":\"window.mine + 1\",\"world\":\"w\"}", 0, c,
             &cap);
        CHECK(strcmp(cap.json, "{\"value\":42}") == 0); /* a named world keeps its state */
        call("contents.evaluate", "{\"expression\":\"Promise.resolve({a:[1,2]})\"}", 0, c, &cap);
        CHECK(strcmp(cap.json, "{\"value\":{\"a\":[1,2]}}") == 0);
        call("contents.evaluate", "{\"expression\":\"throw new Error('nope')\"}", 0, c, &cap);
        CHECK(cap.has_error && strcmp(cap.code, "scriptError") == 0);
        call("contents.frames", "{}", 0, c, &cap);
        REQUIRE(!cap.has_error);
        const char* second = strstr(cap.json, "\"parent\":\"");
        REQUIRE(second != NULL); /* the iframe */
        const char* id_at = strstr(cap.json, "},{\"id\":\"");
        REQUIRE(id_at != NULL);
        char frame[64] = "";
        sscanf(id_at + strlen("},{\"id\":\""), "%63[^\"]", frame);
        char args[256];
        snprintf(
            args, sizeof args,
            "{\"expression\":\"document.getElementById('inner').textContent\",\"frame\":\"%s\"}",
            frame);
        call("contents.evaluate", args, 0, c, &cap);
        CHECK(strcmp(cap.json, "{\"value\":\"inside\"}") == 0);
        call("contents.text", "{}", 0, c, &cap);
        CHECK(strstr(cap.json, "Some visible text") != NULL);
        call("contents.query", "{\"selector\":\"button\"}", 0, c, &cap);
        CHECK(strstr(cap.json, "\"tag\":\"button\"") != NULL);
        CHECK(strstr(cap.json, "\"box\":{") != NULL);
        call("contents.snapshot", "{\"kind\":\"accessibility\"}", 0, c, &cap);
        CHECK(!cap.has_error && strstr(cap.json, "\"root\":") != NULL);
    }
    for (int i = 0; i < 2; ++i)
        CHECK(bdk_contents_destroy(pages[i]) == BDK_OK);
}

static void test_engine_css_and_input_reach_two_pages_and_end_with_the_document(void) {
    start_plain();
    bdk_profile_t p = t_new_profile("Pages");
    script_page("https://one.bdk.test/", FORM_PAGE);
    script_page("https://two.bdk.test/", FORM_PAGE);
    bdk_contents_t pages[2] = {page_at(p, "https://one.bdk.test/"),
                               page_at(p, "https://two.bdk.test/")};
    for (int i = 0; i < 2; ++i) {
        bdk_contents_t c = pages[i];
        bdk_harness_attach_view(c);
        big_capture_t cap;
        call("contents.insertCSS", "{\"css\":\"p { color: rgb(1, 2, 3) }\"}", 0, c, &cap);
        REQUIRE(!cap.has_error);
        char css_id[128] = "";
        REQUIRE(t_json_string(cap.json, "id", css_id, sizeof css_id));
        call("contents.evaluate",
             "{\"expression\":\"getComputedStyle(document.querySelector('p')).color\"}", 0, c,
             &cap);
        CHECK(strcmp(cap.json, "{\"value\":\"rgb(1, 2, 3)\"}") == 0);
        char args[256];
        snprintf(args, sizeof args, "{\"id\":\"%s\"}", css_id);
        call("contents.removeCSS", args, 0, c, &cap);
        CHECK(!cap.has_error);
        call("contents.evaluate",
             "{\"expression\":\"getComputedStyle(document.querySelector('p')).color\"}", 0, c,
             &cap);
        CHECK(strcmp(cap.json, "{\"value\":\"rgb(1, 2, 3)\"}") != 0);
        call("contents.click", "{\"selector\":\"#go\"}", 0, c, &cap);
        CHECK(!cap.has_error);
        CHECK(wait_for_title(c, "clicked"));
        call("contents.type", "{\"selector\":\"#field\",\"text\":\"hello\"}", 0, c, &cap);
        CHECK(!cap.has_error);
        call("contents.evaluate", "{\"expression\":\"document.getElementById('field').value\"}", 0,
             c, &cap);
        CHECK(strcmp(cap.json, "{\"value\":\"hello\"}") == 0);
        call("contents.press", "{\"key\":\"Tab\"}", 0, c, &cap);
        CHECK(!cap.has_error);
        call("contents.scroll", "{\"dy\":400}", 0, c, &cap);
        CHECK(!cap.has_error);
        call("contents.evaluate", "{\"expression\":\"window.scrollY > 0\"}", 0, c, &cap);
        CHECK(strcmp(cap.json, "{\"value\":true}") == 0);
        call("contents.click", "{\"selector\":\"#missing\"}", 0, c, &cap);
        CHECK(cap.has_error && strcmp(cap.code, "noElement") == 0);
        /* What a script added ends with the document. */
        call("contents.insertCSS", "{\"css\":\"p { color: rgb(9, 9, 9) }\"}", 0, c, &cap);
        call("contents.navigate", "{\"url\":\"https://one.bdk.test/\",\"waitUntil\":\"finished\"}",
             0, c, &cap);
        CHECK(!cap.has_error);
        call("contents.evaluate",
             "{\"expression\":\"getComputedStyle(document.querySelector('p')).color\"}", 0, c,
             &cap);
        CHECK(strcmp(cap.json, "{\"value\":\"rgb(9, 9, 9)\"}") != 0);
        call("contents.evaluate", "{\"expression\":\"window.mine\",\"world\":\"w\"}", 0, c, &cap);
        CHECK(strcmp(cap.json, "{\"value\":null}") == 0);
    }
    for (int i = 0; i < 2; ++i)
        CHECK(bdk_contents_destroy(pages[i]) == BDK_OK);
}

static void test_engine_user_scripts_follow_their_match_patterns(void) {
    start_plain();
    bdk_profile_t p = t_new_profile("Scripts");
    script_page("https://match.bdk.test/", "<title>m</title>");
    script_page("https://other.bdk.test/", "<title>o</title>");
    big_capture_t cap;
    call("scripts.register",
         "{\"script\":{\"id\":\"marker\",\"matches\":[\"https://match.bdk.test/*\"],\"runAt\":"
         "\"documentStart\",\"javascript\":[\"document.documentElement.dataset.bdk = '1'\"]}}",
         p, 0, &cap);
    REQUIRE(!cap.has_error);
    bdk_contents_t a = page_at(p, "https://match.bdk.test/");
    bdk_contents_t b = page_at(p, "https://other.bdk.test/");
    call("contents.evaluate",
         "{\"expression\":\"document.documentElement.dataset.bdk || null\",\"world\":\"page\"}", 0,
         a, &cap);
    CHECK(strcmp(cap.json, "{\"value\":\"1\"}") == 0);
    call("contents.evaluate",
         "{\"expression\":\"document.documentElement.dataset.bdk || null\",\"world\":\"page\"}", 0,
         b, &cap);
    CHECK(strcmp(cap.json, "{\"value\":null}") == 0);
    call("scripts.unregister", "{\"id\":\"marker\"}", p, 0, &cap);
    call("contents.reload", "{}", 0, a, &cap);
    call("contents.waitFor", "{\"load\":\"finished\"}", 0, a, &cap);
    call("contents.evaluate",
         "{\"expression\":\"document.documentElement.dataset.bdk || null\",\"world\":\"page\"}", 0,
         a, &cap);
    CHECK(strcmp(cap.json, "{\"value\":null}") == 0);
    CHECK(bdk_contents_destroy(a) == BDK_OK);
    CHECK(bdk_contents_destroy(b) == BDK_OK);
}

typedef struct debug_log {
    int events, load_fired;
} debug_log_t;

static void on_debug_event(void* ctx, uint32_t id, bdk_string_t name, bdk_string_t payload) {
    (void)id;
    (void)name;
    debug_log_t* log = (debug_log_t*)ctx;
    ++log->events;
    char text[512];
    snprintf(text, sizeof text, "%.*s", (int)payload.length, payload.data);
    if (strstr(text, "Page.loadEventFired"))
        log->load_fired = 1;
}

static void test_engine_the_devtools_protocol_attaches_sends_and_detaches(void) {
    start_plain();
    bdk_profile_t p = t_new_profile("Debug");
    script_page("https://debug.bdk.test/", "<title>d</title>");
    bdk_contents_t c = page_at(p, "https://debug.bdk.test/");
    static debug_log_t log;
    memset(&log, 0, sizeof log);
    REQUIRE(bdk_subscribe_named(t_runtime, S("debug.event"), S(""), NULL, on_debug_event, &log,
                                NULL) != 0);
    big_capture_t cap;
    call("debug.attach", "{}", 0, c, &cap);
    REQUIRE(!cap.has_error);
    char session[128] = "";
    REQUIRE(t_json_string(cap.json, "session", session, sizeof session));
    char args[512];
    snprintf(args, sizeof args,
             "{\"session\":\"%s\",\"method\":\"Runtime.evaluate\",\"params\":{\"expression\":"
             "\"1+2\",\"returnByValue\":true}}",
             session);
    call("debug.send", args, 0, c, &cap);
    CHECK(strstr(cap.json, "\"value\":3") != NULL);
    snprintf(args, sizeof args, "{\"session\":\"%s\",\"method\":\"Page.enable\"}", session);
    call("debug.send", args, 0, c, &cap);
    call("contents.reload", "{}", 0, c, &cap);
    CHECK(wait_until(&log.load_fired));
    snprintf(args, sizeof args, "{\"session\":\"%s\"}", session);
    call("debug.detach", args, 0, c, &cap);
    CHECK(!cap.has_error);
    snprintf(args, sizeof args, "{\"session\":\"%s\",\"method\":\"Runtime.enable\"}", session);
    call("debug.send", args, 0, c, &cap);
    CHECK(cap.has_error);
    CHECK(bdk_contents_destroy(c) == BDK_OK);
}

static void test_engine_internal_pages_cannot_be_scripted_or_debugged(void) {
    start_plain();
    bdk_profile_t p = t_new_profile("Internal");
    bdk_contents_t c = t_new_contents(p);
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_load(c, S("chrome://version"), &err) == BDK_OK);
    big_capture_t cap;
    call("contents.waitFor", "{\"load\":\"finished\"}", 0, c, &cap);
    call("contents.evaluate", "{\"expression\":\"1\"}", 0, c, &cap);
    CHECK(cap.has_error && strcmp(cap.code, "internalPage") == 0);
    call("debug.attach", "{}", 0, c, &cap);
    CHECK(cap.has_error && strcmp(cap.code, "internalPage") == 0);
    CHECK(bdk_contents_destroy(c) == BDK_OK);
}

/* ---- captures (T-SVC-09) ---- */

static void test_engine_screenshots_pdfs_and_saved_pages(void) {
    start_plain();
    bdk_profile_t p = t_new_profile("Capture");
    script_page("https://capture.bdk.test/",
                "<title>c</title><p>capture me</p><div style='height:2400px'></div>");
    bdk_contents_t c = page_at(p, "https://capture.bdk.test/");
    bdk_harness_attach_view(c);
    static const unsigned char png[4] = {0x89, 'P', 'N', 'G'};
    big_capture_t visible, full, rect;
    call("contents.screenshot", "{}", 0, c, &visible);
    CHECK(!visible.has_error && visible.has_binary && memcmp(visible.head, png, 4) == 0);
    call("contents.screenshot", "{\"area\":\"fullPage\"}", 0, c, &full);
    CHECK(!full.has_error && full.has_binary && memcmp(full.head, png, 4) == 0);
    call("contents.screenshot",
         "{\"area\":\"rect\",\"rect\":{\"x\":0,\"y\":0,\"width\":40,\"height\":30}}", 0, c, &rect);
    CHECK(!rect.has_error && rect.has_binary && rect.bytes < full.bytes);
    big_capture_t jpeg;
    call("contents.screenshot", "{\"format\":\"jpeg\",\"quality\":50}", 0, c, &jpeg);
    CHECK(!jpeg.has_error && jpeg.head[0] == 0xff && jpeg.head[1] == 0xd8);
    big_capture_t pdf;
    call("contents.pdf", "{\"landscape\":true}", 0, c, &pdf);
    CHECK(!pdf.has_error && pdf.has_binary && memcmp(pdf.head, "%PDF", 4) == 0);
    char dir[] = "/tmp/bdk-save-XXXXXX";
    REQUIRE(mkdtemp(dir) != NULL);
    const char* formats[3] = {"htmlOnly", "completePage", "singleFile"};
    for (int i = 0; i < 3; ++i) {
        char path[256], args[512];
        snprintf(path, sizeof path, "%s/page-%d.%s", dir, i, i == 2 ? "mhtml" : "html");
        snprintf(args, sizeof args, "{\"path\":\"%s\",\"format\":\"%s\"}", path, formats[i]);
        big_capture_t saved;
        call("contents.save", args, 0, c, &saved);
        CHECK(!saved.has_error);
        struct stat st;
        CHECK(stat(path, &st) == 0 && st.st_size > 0);
    }
    char path[256], args[512];
    snprintf(path, sizeof path, "%s/shot.png", dir);
    snprintf(args, sizeof args, "{\"path\":\"%s\"}", path);
    big_capture_t to_file;
    call("contents.screenshot", args, 0, c, &to_file);
    CHECK(!to_file.has_error && !to_file.has_binary && strstr(to_file.json, path) != NULL);
    CHECK(bdk_contents_destroy(c) == BDK_OK);
}

/* ---- DevTools front ends (T-AUT-05) ---- */

typedef struct dt_log {
    int calls;
    char payload[1024];
    bdk_request_t request;
} dt_log_t;

static bdk_ui_response_t on_devtools(void* ctx, const bdk_ui_request_info_t* info) {
    dt_log_t* log = (dt_log_t*)ctx;
    ++log->calls;
    log->request = info->request;
    snprintf(log->payload, sizeof log->payload, "%.*s", (int)info->payload_json.length,
             info->payload_json.data);
    bdk_native_handle_t none;
    memset(&none, 0, sizeof none);
    bdk_error_t err = 0;
    CHECK(bdk_ui_complete(info->request, S("accept"), S(""), none, &err) == BDK_OK);
    return BDK_UI_HANDLED;
}

static bdk_contents_t open_front_end(bdk_contents_t c, dt_log_t* log) {
    big_capture_t cap;
    call("devtools.open", "{}", 0, c, &cap);
    REQUIRE(!cap.has_error);
    REQUIRE(wait_until(&log->calls));
    char handle[32];
    REQUIRE(t_json_string(log->payload, "frontend", handle, sizeof handle));
    return (bdk_contents_t)strtoull(handle, NULL, 10);
}

static int wait_for_url(bdk_contents_t c, const char* prefix) {
    for (int i = 0; i < 100; ++i) {
        bdk_contents_state_t st;
        memset(&st, 0, sizeof st);
        st.struct_size = sizeof st;
        bdk_error_t err = 0;
        REQUIRE(bdk_contents_state(c, &st, &err) == BDK_OK);
        if (st.url.length >= strlen(prefix) && memcmp(st.url.data, prefix, strlen(prefix)) == 0 &&
            !st.loading)
            return 1;
        settle();
    }
    return 0;
}

static void start_devtools(const char* devtools, dt_log_t* log, int handler) {
    t_config_extra = devtools;
    t_start();
    memset(log, 0, sizeof *log);
    bdk_error_t err = 0;
    if (handler)
        REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_DEVTOOLS, on_devtools, log, NULL, &err) ==
                BDK_OK);
    t_freeze();
}

static void test_engine_devtools_default_front_end_opens_and_closes(void) {
    static dt_log_t log;
    start_devtools("\"devtools\":{\"frontend\":\"default\",\"protocol\":true}", &log, 1);
    bdk_profile_t p = t_new_profile("DevTools");
    script_page("https://inspect.bdk.test/", "<title>i</title>");
    bdk_contents_t c = page_at(p, "https://inspect.bdk.test/");
    bdk_contents_t front = open_front_end(c, &log);
    CHECK(strstr(log.payload, "\"preferredDock\":\"undocked\"") != NULL);
    bdk_harness_attach_view(front);
    CHECK(wait_for_url(front, "devtools://devtools/bundled/devtools_app.html"));
    big_capture_t cap;
    call("devtools.state", "{}", 0, c, &cap);
    CHECK(strstr(cap.json, "\"open\":true") != NULL);
    call("devtools.close", "{}", 0, c, &cap);
    CHECK(!cap.has_error);
    CHECK(bdk_contents_destroy(front) == BDK_OK); /* the application placed it, so it owns it */
    settle();
    call("devtools.state", "{}", 0, c, &cap);
    CHECK(strstr(cap.json, "\"open\":false") != NULL);
    CHECK(bdk_contents_destroy(c) == BDK_OK);
}

static void test_engine_devtools_default_presenter_is_an_undocked_window(void) {
    static dt_log_t log;
    start_devtools("\"devtools\":{\"frontend\":\"default\"}", &log, 0);
    char ignored[64];
    REQUIRE(bdk_testing_command(t_runtime, S("present.script"),
                                S("{\"surface\":\"devtools\",\"answer\":{\"none\":true}}"), ignored,
                                sizeof ignored) == BDK_OK);
    bdk_profile_t p = t_new_profile("DevTools");
    script_page("https://inspect.bdk.test/", "<title>i</title>");
    bdk_contents_t c = page_at(p, "https://inspect.bdk.test/");
    big_capture_t cap;
    call("devtools.open", "{}", 0, c, &cap);
    for (int i = 0; i < 40; ++i)
        settle();
    char record[512];
    control("present.record", "{\"surface\":\"devtools\"}", record, sizeof record);
    CHECK(strstr(record, "\"shown\":1") != NULL);
    call("devtools.state", "{}", 0, c, &cap);
    CHECK(strstr(cap.json, "\"open\":true") != NULL);
    call("devtools.close", "{}", 0, c, &cap);
    for (int i = 0; i < 20; ++i)
        settle();
    call("devtools.state", "{}", 0, c, &cap);
    CHECK(strstr(cap.json, "\"open\":false") != NULL); /* nobody owned it: it is gone */
    control("present.clear", "{}", NULL, 0);
    CHECK(bdk_contents_destroy(c) == BDK_OK);
}

static void test_engine_devtools_overlay_loads_into_the_default_front_end(void) {
    static dt_log_t log;
    start_devtools("\"devtools\":{\"frontend\":\"default\",\"overlay\":\"DevTools/Overlay\"}", &log,
                   1);
    bdk_profile_t p = t_new_profile("DevTools");
    script_page("https://inspect.bdk.test/", "<title>i</title>");
    bdk_contents_t c = page_at(p, "https://inspect.bdk.test/");
    bdk_contents_t front = open_front_end(c, &log);
    bdk_harness_attach_view(front);
    CHECK(wait_for_url(front, "devtools://devtools/bundled/devtools_app.html"));
    for (int i = 0; i < 20; ++i)
        settle();
    REQUIRE(bdk_testing_execute_script(
                front,
                S("document.title = (document.documentElement.dataset.bdkOverlay || 'none') + "
                  "(document.querySelector('style[data-bdk-overlay]') ? '+css' : '')"),
                0) == BDK_OK);
    CHECK(wait_for_title(front, "js+css"));
    CHECK(bdk_contents_destroy(front) == BDK_OK);
    CHECK(bdk_contents_destroy(c) == BDK_OK);
}

static void test_engine_devtools_replaced_front_end_is_served_from_the_bundle(void) {
    static dt_log_t log;
    start_devtools("\"devtools\":{\"frontend\":\"DevTools/Frontend\"}", &log, 1);
    bdk_profile_t p = t_new_profile("DevTools");
    script_page("https://inspect.bdk.test/", "<title>i</title>");
    bdk_contents_t c = page_at(p, "https://inspect.bdk.test/");
    bdk_contents_t front = open_front_end(c, &log);
    bdk_harness_attach_view(front);
    CHECK(strstr(log.payload, "devtools://devtools/bundled/index.html") != NULL);
    CHECK(wait_for_title(front, "BDK replacement front end"));
    CHECK(bdk_contents_destroy(front) == BDK_OK);
    CHECK(bdk_contents_destroy(c) == BDK_OK);
}

static void test_engine_devtools_disabled_offers_no_front_end(void) {
    static dt_log_t log;
    start_devtools("\"devtools\":{\"frontend\":\"disabled\",\"protocol\":true}", &log, 1);
    bdk_profile_t p = t_new_profile("DevTools");
    script_page("https://inspect.bdk.test/", "<title>i</title>");
    bdk_contents_t c = page_at(p, "https://inspect.bdk.test/");
    big_capture_t cap;
    call("devtools.open", "{}", 0, c, &cap);
    CHECK(cap.has_error && strcmp(cap.code, "disabled") == 0);
    CHECK(log.calls == 0);
    call("debug.attach", "{}", 0, c, &cap); /* the protocol is independent of the front end */
    CHECK(!cap.has_error);
    CHECK(bdk_contents_destroy(c) == BDK_OK);
}

const t_test_t t_engine_automation_tests[] = {
    {"engine_a_script_drives_two_pages", test_engine_a_script_drives_two_pages},
    {"engine_a_session_without_a_capability_is_refused",
     test_engine_a_session_without_a_capability_is_refused},
    {"engine_cancel_and_revoke_end_a_running_script",
     test_engine_cancel_and_revoke_end_a_running_script},
    {"engine_a_failing_or_tampered_script_finishes_with_why",
     test_engine_a_failing_or_tampered_script_finishes_with_why},
    {"engine_scripts_run_in_worlds_and_frames_on_two_pages",
     test_engine_scripts_run_in_worlds_and_frames_on_two_pages},
    {"engine_css_and_input_reach_two_pages_and_end_with_the_document",
     test_engine_css_and_input_reach_two_pages_and_end_with_the_document},
    {"engine_user_scripts_follow_their_match_patterns",
     test_engine_user_scripts_follow_their_match_patterns},
    {"engine_the_devtools_protocol_attaches_sends_and_detaches",
     test_engine_the_devtools_protocol_attaches_sends_and_detaches},
    {"engine_internal_pages_cannot_be_scripted_or_debugged",
     test_engine_internal_pages_cannot_be_scripted_or_debugged},
    {"engine_screenshots_pdfs_and_saved_pages", test_engine_screenshots_pdfs_and_saved_pages},
    {"engine_devtools_default_front_end_opens_and_closes",
     test_engine_devtools_default_front_end_opens_and_closes},
    {"engine_devtools_default_presenter_is_an_undocked_window",
     test_engine_devtools_default_presenter_is_an_undocked_window},
    {"engine_devtools_overlay_loads_into_the_default_front_end",
     test_engine_devtools_overlay_loads_into_the_default_front_end},
    {"engine_devtools_replaced_front_end_is_served_from_the_bundle",
     test_engine_devtools_replaced_front_end_is_served_from_the_bundle},
    {"engine_devtools_disabled_offers_no_front_end",
     test_engine_devtools_disabled_offers_no_front_end},
    {NULL, NULL},
};
