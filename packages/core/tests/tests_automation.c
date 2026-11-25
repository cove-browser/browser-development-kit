/* Automation (M7): sessions, tasks and the wire protocol, the core's contents.* and scripts.*
   commands, and the debug.* and devtools.* checks. The task tests play the script through the
   harness's scripted host (harness_automation.h) and are skipped by a runtime without it; the
   rest run against every runtime. The external endpoint is in tests_endpoint.c. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness_automation.h"
#include "harness_broker.h"
#include "test_support.h"

#define AUTOMATION_ON                                                                              \
    "\"automation\":{\"enabled\":true,\"scripts\":{\"echo\":{\"path\":\"Automation/echo.ts\","     \
    "\"digest\":\"sha256:0\"},\"missing\":{\"path\":\"Automation/missing.ts\",\"digest\":"         \
    "\"sha256:0\"}},\"external\":{\"enabled\":true}}"

#define NEED_HOST()                                                                                \
    do {                                                                                           \
        if (!bdk_harness_automation_hooks()) {                                                     \
            printf("    (skipped: the runtime has no scripted automation host)\n");                \
            return;                                                                                \
        }                                                                                          \
    } while (0)

/* ---- A script the test plays ---- */

typedef struct t_script {
    int frames;
    uint64_t task;
    char last[2048];
    char all[8192];
} t_script_t;

static void on_script_frame(void* ctx, uint64_t task, const char* frame, size_t n) {
    t_script_t* s = (t_script_t*)ctx;
    ++s->frames;
    s->task = task;
    snprintf(s->last, sizeof s->last, "%.*s", (int)n, frame);
    size_t used = strlen(s->all);
    snprintf(s->all + used, sizeof s->all - used, "%.*s\n", (int)n, frame);
}

static void define_echo(t_script_t* s) {
    memset(s, 0, sizeof *s);
    REQUIRE(bdk_harness_script_define(t_runtime, "echo", on_script_frame, s) == BDK_OK);
}

static void script_send(t_script_t* s, const char* frame) {
    REQUIRE(bdk_harness_script_send(s->task, frame) == BDK_OK);
    t_pump();
}

/* ---- Task events ---- */

#define T_TASK_EVENTS 16

typedef struct t_task_log {
    int count, releases;
    uint32_t kind[T_TASK_EVENTS];
    char detail[T_TASK_EVENTS][384];
} t_task_log_t;

static void on_task_event(void* ctx, bdk_task_t task, uint32_t kind, bdk_string_t detail) {
    (void)task;
    t_task_log_t* log = (t_task_log_t*)ctx;
    if (log->count < T_TASK_EVENTS) {
        log->kind[log->count] = kind;
        snprintf(log->detail[log->count], sizeof log->detail[0], "%.*s", (int)detail.length,
                 detail.data ? detail.data : "");
    }
    ++log->count;
}

static void on_task_release(void* ctx) {
    ++((t_task_log_t*)ctx)->releases;
}

static const char* last_detail(const t_task_log_t* log) {
    return log->count ? log->detail[(log->count < T_TASK_EVENTS ? log->count : T_TASK_EVENTS) - 1]
                      : "";
}

/* ---- Named events ---- */

typedef struct t_named {
    int count;
    char names[16][48];
    char payloads[16][384];
} t_named_t;

static void on_named(void* ctx, uint32_t id, bdk_string_t name, bdk_string_t payload) {
    (void)id;
    t_named_t* n = (t_named_t*)ctx;
    if (n->count < 16) {
        snprintf(n->names[n->count], sizeof n->names[0], "%.*s", (int)name.length, name.data);
        snprintf(n->payloads[n->count], sizeof n->payloads[0], "%.*s", (int)payload.length,
                 payload.data);
    }
    ++n->count;
}

static int named_count(const t_named_t* n, const char* name) {
    int hits = 0;
    for (int i = 0; i < n->count && i < 16; ++i)
        hits += strcmp(n->names[i], name) == 0;
    return hits;
}

/* ---- Helpers ---- */

static bdk_runtime_t start_with(const char* extra) {
    t_config_extra = extra;
    return t_start_frozen();
}

static bdk_session_t session(const char* options) {
    bdk_session_t s = 0;
    bdk_error_t err = 0;
    REQUIRE(bdk_session_create(t_runtime, S(options), &s, &err) == BDK_OK);
    return s;
}

static bdk_task_t run(bdk_session_t s, const char* script, const char* args, t_task_log_t* log) {
    memset(log, 0, sizeof *log);
    bdk_task_t task = 0;
    bdk_error_t err = 0;
    REQUIRE(bdk_session_run(s, S(script), S(args), &task, &err) == BDK_OK);
    REQUIRE(bdk_task_subscribe(task, on_task_event, log, on_task_release) != 0);
    t_pump();
    return task;
}

static void host_invoke(const char* command, const char* args, bdk_profile_t profile,
                        bdk_contents_t contents, t_capture_t* cap) {
    bdk_invoke_options_t o;
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.profile = profile;
    o.contents = contents;
    t_cap_init(cap);
    REQUIRE(bdk_invoke_named(t_runtime, S(command), S(args), &o, t_on_result, t_on_progress, cap,
                             t_on_release) != 0);
    t_pump();
}

static void profile_id_of(bdk_profile_t p, char* out, size_t n) {
    t_capture_t cap;
    host_invoke("profiles.current", "{}", p, 0, &cap);
    REQUIRE(t_json_string(cap.json, "id", out, n));
}

static void id_of(bdk_contents_t c, char* out, size_t n) {
    snprintf(out, n, "%llu", (unsigned long long)c);
}

/* ---- Sessions ---- */

static void session_options_are_checked(void) {
    start_with(NULL);
    bdk_profile_t p = t_new_profile("A");
    char pid[64] = "";
    {
        t_capture_t cap;
        host_invoke("profiles.current", "{}", p, 0, &cap);
        REQUIRE(t_json_string(cap.json, "id", pid, sizeof pid));
    }
    bdk_session_t s = 0;
    bdk_error_t err = 0;
    CHECK(bdk_session_create(t_runtime, S("{}"), &s, &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "broker", "invalidArguments"));
    CHECK(bdk_session_create(t_runtime, S("{\"name\":\"a\",\"capabilities\":[\"history.*\"]}"), &s,
                             &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "capability", "denied"));
    CHECK(bdk_session_create(t_runtime,
                             S("{\"name\":\"a\",\"capabilities\":[\"passwords.reveal\"]}"), &s,
                             &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "capability", "denied"));
    CHECK(bdk_session_create(t_runtime, S("{\"name\":\"a\",\"scope\":{\"profiles\":[\"nope\"]}}"),
                             &s, &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "profile", "notFound"));
    CHECK(bdk_session_create(t_runtime, S("{\"name\":\"a\",\"scope\":{\"contents\":\"some\"}}"), &s,
                             &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "broker", "invalidArguments"));
    char ok[256];
    snprintf(ok, sizeof ok,
             "{\"name\":\"a\",\"capabilities\":[\"app.notes.*\"],\"scope\":{\"profiles\":[\"%s\"],"
             "\"contents\":\"explicit\"}}",
             pid);
    REQUIRE(bdk_session_create(t_runtime, S(ok), &s, &err) == BDK_OK);
    CHECK(bdk_session_revoke(s, &err) == BDK_OK);
    CHECK(bdk_session_revoke(s, &err) == BDK_ERR_STALE_HANDLE);
}

static void run_needs_enabled_and_listed(void) {
    start_with(NULL);
    bdk_session_t s = session("{\"name\":\"a\"}");
    bdk_task_t task = 0;
    bdk_error_t err = 0;
    CHECK(bdk_session_run(s, S("echo"), S("{}"), &task, &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "automation", "disabled"));
    t_stop();
    start_with(AUTOMATION_ON);
    s = session("{\"name\":\"a\"}");
    CHECK(bdk_session_run(s, S("nope"), S("{}"), &task, &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "automation", "scriptNotFound"));
    CHECK(bdk_session_run(s, S("echo"), S("{not json"), &task, &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "broker", "invalidArguments"));
    CHECK(task == 0);
}

/* ---- Tasks ---- */

static void task_finishes_with_the_scripts_result(void) {
    NEED_HOST();
    start_with(AUTOMATION_ON);
    static t_script_t script;
    define_echo(&script);
    bdk_session_t s = session("{\"name\":\"agent\",\"capabilities\":[\"runtime.read\"]}");
    static t_task_log_t log;
    bdk_task_t task = run(s, "echo", "{\"url\":\"https://example.com/\"}", &log);
    REQUIRE(script.frames == 1);
    CHECK(strstr(script.last, "\"type\":\"start\"") != NULL);
    CHECK(strstr(script.last, "\"arguments\":{\"url\":\"https://example.com/\"}") != NULL);
    CHECK(strstr(script.last,
                 "\"session\":{\"name\":\"agent\",\"capabilities\":[\"runtime.read\"]}") != NULL);
    CHECK(log.count == 0);
    script_send(&script, "{\"type\":\"done\",\"result\":{\"title\":\"Example\"}}");
    REQUIRE(log.count == 1);
    CHECK(log.kind[0] == BDK_TASK_EVENT_FINISHED);
    CHECK(strcmp(log.detail[0], "{\"result\":{\"title\":\"Example\"}}") == 0);
    CHECK(log.releases == 1);
    CHECK(bdk_harness_automation_count(t_runtime, BDK_HARNESS_AUTOMATION_ENDED) == 1);
    /* A late subscription gets FINISHED alone. */
    static t_task_log_t late;
    memset(&late, 0, sizeof late);
    REQUIRE(bdk_task_subscribe(task, on_task_event, &late, on_task_release) != 0);
    t_pump();
    CHECK(late.count == 1 && late.kind[0] == BDK_TASK_EVENT_FINISHED && late.releases == 1);
    /* The script is gone: frames from it are refused. */
    CHECK(bdk_harness_script_send(script.task, "{\"type\":\"log\",\"message\":\"x\"}") ==
          BDK_ERR_STALE_HANDLE);
    CHECK(bdk_release(task) == BDK_OK);
    CHECK(bdk_release(task) == BDK_ERR_STALE_HANDLE);
}

static void task_calls_commands_as_the_session(void) {
    NEED_HOST();
    start_with(AUTOMATION_ON);
    static t_script_t script;
    define_echo(&script);
    static t_named_t audit;
    memset(&audit, 0, sizeof audit);
    REQUIRE(bdk_subscribe_named(t_runtime, S("automation.commandInvoked"), S(""), NULL, on_named,
                                &audit, NULL) != 0);
    bdk_session_t s = session("{\"name\":\"agent\",\"capabilities\":[\"runtime.read\"]}");
    static t_task_log_t log;
    (void)run(s, "echo", "null", &log);
    script_send(&script, "{\"type\":\"call\",\"id\":1,\"command\":\"runtime.caller\"}");
    CHECK(strstr(script.last, "\"type\":\"result\",\"id\":1") != NULL);
    CHECK(strstr(script.last, "\"kind\":\"session\"") != NULL);
    CHECK(strstr(script.last, "\"name\":\"agent\"") != NULL);
    /* Without the capability: refused, and the refusal is audited. */
    script_send(&script, "{\"type\":\"call\",\"id\":2,\"command\":\"history.search\",\"args\":{}}");
    CHECK(strstr(script.last, "\"type\":\"error\",\"id\":2") != NULL);
    CHECK(strstr(script.last, "\"code\":\"denied\"") != NULL);
    script_send(&script, "{\"type\":\"call\",\"id\":3,\"command\":\"no.such\"}");
    CHECK(strstr(script.last, "\"code\":\"unknownCommand\"") != NULL);
    /* An id in use is a protocol error that ends the connection, and with it the task's calls. */
    CHECK(named_count(&audit, "automation.commandInvoked") == 3);
    CHECK(strstr(audit.payloads[1], "\"command\":\"history.search\"") != NULL);
    CHECK(strstr(audit.payloads[1], "\"error\":{\"domain\":\"capability\",\"code\":\"denied\"}") !=
          NULL);
    CHECK(strstr(audit.payloads[0], "\"session\":\"agent\"") != NULL);
    CHECK(strstr(audit.payloads[0], "\"task\":\"task-") != NULL);
    int commands = 0;
    for (int i = 0; i < log.count; ++i)
        commands += log.kind[i] == BDK_TASK_EVENT_COMMAND;
    CHECK(commands == 3);
}

static void refusals_before_the_broker_are_audited(void) {
    NEED_HOST();
    start_with(AUTOMATION_ON);
    static t_script_t script;
    define_echo(&script);
    static t_named_t audit;
    memset(&audit, 0, sizeof audit);
    REQUIRE(bdk_subscribe_named(t_runtime, S("automation.commandInvoked"), S(""), NULL, on_named,
                                &audit, NULL) != 0);
    bdk_session_t s = session("{\"name\":\"agent\",\"capabilities\":[\"contents.read\"]}");
    static t_task_log_t log;
    (void)run(s, "echo", "null", &log);
    script_send(&script, "{\"type\":\"call\",\"id\":1,\"command\":\"contents.get\",\"args\":[]}");
    CHECK(strstr(script.last, "\"type\":\"error\",\"id\":1") != NULL);
    CHECK(strstr(script.last, "\"code\":\"invalidArguments\"") != NULL);
    script_send(&script,
                "{\"type\":\"call\",\"id\":2,\"command\":\"contents.get\",\"profile\":\"nope\"}");
    CHECK(strstr(script.last, "\"type\":\"error\",\"id\":2") != NULL);
    CHECK(strstr(script.last, "\"code\":\"notFound\"") != NULL);
    /* Twenty digits, past a handle's range: refused as a stale page, not a crash. */
    script_send(&script, "{\"type\":\"call\",\"id\":3,\"command\":\"contents.get\","
                         "\"contents\":\"99999999999999999999\"}");
    CHECK(strstr(script.last, "\"type\":\"error\",\"id\":3") != NULL);
    CHECK(strstr(script.last, "\"code\":\"stale\"") != NULL);
    REQUIRE(named_count(&audit, "automation.commandInvoked") == 3);
    CHECK(strstr(audit.payloads[0], "\"code\":\"invalidArguments\"") != NULL);
    CHECK(strstr(audit.payloads[1], "\"profile\":\"nope\"") != NULL);
    CHECK(strstr(audit.payloads[1], "\"domain\":\"profile\",\"code\":\"notFound\"") != NULL);
    CHECK(strstr(audit.payloads[2], "\"contents\":\"99999999999999999999\"") != NULL);
    CHECK(strstr(audit.payloads[2], "\"domain\":\"contents\",\"code\":\"stale\"") != NULL);
    int commands = 0;
    for (int i = 0; i < log.count && i < T_TASK_EVENTS; ++i)
        commands +=
            log.kind[i] == BDK_TASK_EVENT_COMMAND && strstr(log.detail[i], "\"error\":") != NULL;
    CHECK(commands == 3);
}

static void task_progress_and_log_come_before_finished(void) {
    NEED_HOST();
    start_with(AUTOMATION_ON);
    static t_script_t script;
    define_echo(&script);
    bdk_session_t s = session("{\"name\":\"agent\"}");
    static t_task_log_t log;
    (void)run(s, "echo", "null", &log);
    script_send(&script, "{\"type\":\"progress\",\"fraction\":2,\"message\":\"half\"}");
    script_send(&script, "{\"type\":\"log\",\"level\":\"warn\",\"message\":\"careful\"}");
    script_send(&script, "{\"type\":\"done\",\"result\":null}");
    REQUIRE(log.count == 3);
    CHECK(log.kind[0] == BDK_TASK_EVENT_PROGRESS);
    CHECK(strcmp(log.detail[0], "{\"fraction\":1,\"message\":\"half\"}") == 0);
    CHECK(log.kind[1] == BDK_TASK_EVENT_LOG);
    CHECK(strcmp(log.detail[1], "{\"level\":\"warn\",\"message\":\"careful\"}") == 0);
    CHECK(log.kind[2] == BDK_TASK_EVENT_FINISHED);
}

static void task_cancel_ends_it(void) {
    NEED_HOST();
    start_with(AUTOMATION_ON);
    static t_script_t script;
    define_echo(&script);
    bdk_session_t s = session("{\"name\":\"agent\"}");
    static t_task_log_t log;
    bdk_task_t task = run(s, "echo", "null", &log);
    bdk_error_t err = 0;
    CHECK(bdk_task_cancel(task, &err) == BDK_OK);
    t_pump();
    CHECK(strstr(script.last, "\"type\":\"abort\"") != NULL);
    REQUIRE(log.count == 1);
    CHECK(strstr(log.detail[0], "\"code\":\"cancelled\"") != NULL);
    CHECK(bdk_harness_automation_count(t_runtime, BDK_HARNESS_AUTOMATION_ENDED) == 1);
    CHECK(bdk_task_cancel(task, &err) == BDK_OK); /* finished: nothing more happens */
    t_pump();
    CHECK(log.count == 1);
    /* Releasing a running task's handle cancels it. */
    static t_task_log_t second;
    bdk_task_t other = run(s, "echo", "null", &second);
    CHECK(bdk_release(other) == BDK_OK);
    t_pump();
    CHECK(second.count == 1 && strstr(second.detail[0], "\"code\":\"cancelled\"") != NULL);
    /* The host is told to end it although the handle, and with it the task, is gone. */
    CHECK(bdk_harness_automation_count(t_runtime, BDK_HARNESS_AUTOMATION_ENDED) == 2);
}

static void revoking_the_session_stops_its_tasks_and_calls(void) {
    NEED_HOST();
    start_with(AUTOMATION_ON);
    static t_script_t script;
    define_echo(&script);
    bdk_profile_t p = t_new_profile("A");
    char pid[64];
    profile_id_of(p, pid, sizeof pid);
    bdk_session_t s = session("{\"name\":\"agent\",\"capabilities\":[\"history.read\"]}");
    static t_named_t audit;
    memset(&audit, 0, sizeof audit);
    REQUIRE(bdk_subscribe_named(t_runtime, S("automation.sessionRevoked"), S(""), NULL, on_named,
                                &audit, NULL) != 0);
    static t_task_log_t log;
    (void)run(s, "echo", "null", &log);
    CHECK(bdk_harness_hold_services(t_runtime, 1) == BDK_OK);
    char call[192];
    snprintf(call, sizeof call,
             "{\"type\":\"call\",\"id\":1,\"command\":\"history.search\",\"args\":{},"
             "\"profile\":\"%s\"}",
             pid);
    script_send(&script, call);
    CHECK(bdk_harness_service_count(t_runtime, BDK_HARNESS_COUNT_HELD) == 1);
    bdk_error_t err = 0;
    CHECK(bdk_session_revoke(s, &err) == BDK_OK);
    t_pump();
    REQUIRE(log.count >= 1);
    CHECK(strstr(last_detail(&log), "\"code\":\"sessionRevoked\"") != NULL);
    CHECK(bdk_harness_service_count(t_runtime, BDK_HARNESS_COUNT_STOPPED) >= 1);
    CHECK(named_count(&audit, "automation.sessionRevoked") == 1);
    bdk_task_t task = 0;
    CHECK(bdk_session_run(s, S("echo"), S("null"), &task, &err) == BDK_ERR_STALE_HANDLE);
    CHECK(bdk_harness_hold_services(t_runtime, 0) == BDK_OK);
}

static void a_host_crash_ends_every_task(void) {
    NEED_HOST();
    start_with(AUTOMATION_ON);
    static t_script_t script;
    define_echo(&script);
    bdk_session_t s = session("{\"name\":\"agent\"}");
    static t_task_log_t a, b;
    (void)run(s, "echo", "null", &a);
    (void)run(s, "echo", "null", &b);
    CHECK(bdk_harness_automation_host_crash(t_runtime) == BDK_OK);
    t_pump();
    CHECK(a.count == 1 && strstr(a.detail[0], "\"code\":\"hostGone\"") != NULL);
    CHECK(b.count == 1 && strstr(b.detail[0], "\"code\":\"hostGone\"") != NULL);
    /* The session survives; the next task gets a new host. */
    static t_task_log_t c;
    (void)run(s, "echo", "null", &c);
    CHECK(c.count == 0);
}

#define AUTOMATION_BUDGET                                                                          \
    "\"automation\":{\"enabled\":true,\"cpu_budget_ms\":500,\"scripts\":{\"echo\":{\"path\":"      \
    "\"Automation/echo.ts\",\"digest\":\"sha256:0\"}}}"

/* T-AUT-07: the host is given each task's CPU budget and stops a script that uses it; the task
   finishes with automation.budgetExceeded, which a script cannot claim for itself. */
static void a_script_that_uses_its_cpu_budget_is_stopped(void) {
    NEED_HOST();
    start_with(AUTOMATION_ON);
    static t_script_t script;
    define_echo(&script);
    bdk_session_t s = session("{\"name\":\"agent\"}");
    static t_task_log_t defaulted;
    (void)run(s, "echo", "null", &defaulted);
    CHECK(bdk_harness_script_budget(script.task) == 60000);
    t_stop();

    start_with(AUTOMATION_BUDGET);
    define_echo(&script);
    s = session("{\"name\":\"agent\"}");
    static t_task_log_t busy;
    (void)run(s, "echo", "null", &busy);
    CHECK(bdk_harness_script_budget(script.task) == 500);
    REQUIRE(bdk_harness_script_exhaust_budget(script.task) == BDK_OK);
    t_pump();
    REQUIRE(busy.count == 1);
    CHECK(strstr(busy.detail[0], "\"code\":\"budgetExceeded\"") != NULL);
    CHECK(strstr(busy.detail[0], "500 ms") != NULL);
    CHECK(bdk_harness_script_budget(script.task) == 0);
    /* A failed frame naming the code is still a script failure. */
    static t_task_log_t forged;
    (void)run(s, "echo", "null", &forged);
    script_send(&script, "{\"type\":\"failed\",\"error\":{\"code\":\"budgetExceeded\","
                         "\"message\":\"x\"}}");
    REQUIRE(forged.count == 1);
    CHECK(strstr(forged.detail[0], "\"code\":\"scriptFailed\"") != NULL);
}

static void a_cpu_budget_that_is_not_whole_milliseconds_is_refused(void) {
    static const char* const bad[] = {"0", "1.5", "\"60s\"", "3600001"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        char extra[256];
        snprintf(extra, sizeof extra,
                 "\"automation\":{\"enabled\":true,\"cpu_budget_ms\":%s,\"scripts\":{}}", bad[i]);
        t_config_extra = extra;
        bdk_start_config_t c;
        t_fill_start_config(&c);
        bdk_runtime_t rt = 0;
        bdk_error_t err = 0;
        CHECK(bdk_runtime_start(&c, &rt, &err) != BDK_OK);
        CHECK(t_error_is(err, "config", "resolvedInvalid"));
        CHECK(rt == 0);
    }
}

static void a_failing_script_finishes_with_its_message(void) {
    NEED_HOST();
    start_with(AUTOMATION_ON);
    static t_script_t script;
    define_echo(&script);
    bdk_session_t s = session("{\"name\":\"agent\"}");
    static t_task_log_t log;
    (void)run(s, "echo", "null", &log);
    script_send(&script,
                "{\"type\":\"failed\",\"error\":{\"message\":\"boom\",\"stack\":\"at x\"}}");
    REQUIRE(log.count == 1);
    CHECK(strstr(log.detail[0], "\"code\":\"scriptFailed\"") != NULL);
    CHECK(strstr(log.detail[0], "\"message\":\"boom\"") != NULL);
    /* Listed in the configuration but unknown to the host: scriptFailed too. */
    static t_task_log_t missing;
    (void)run(s, "missing", "null", &missing);
    CHECK(missing.count == 1 && strstr(missing.detail[0], "\"code\":\"scriptFailed\"") != NULL);
    /* A malformed frame ends the task's connection; the task waits for nothing more. */
    static t_task_log_t bad;
    (void)run(s, "echo", "null", &bad);
    script_send(&script, "not json");
    CHECK(bad.count == 0);
}

/* ---- Scope ---- */

static void pages_a_session_creates_are_in_its_scope(void) {
    NEED_HOST();
    start_with(AUTOMATION_ON);
    static t_script_t script;
    define_echo(&script);
    bdk_profile_t p = t_new_profile("A");
    bdk_contents_t mine = t_new_contents(p);
    char mine_id[32];
    id_of(mine, mine_id, sizeof mine_id);
    static t_named_t created;
    memset(&created, 0, sizeof created);
    REQUIRE(bdk_subscribe_named(t_runtime, S("contents.created"), S(""), NULL, on_named, &created,
                                NULL) != 0);
    bdk_session_t s =
        session("{\"name\":\"agent\",\"capabilities\":[\"contents.read\",\"contents.control\"]}");
    static t_task_log_t log;
    (void)run(s, "echo", "null", &log);
    char frame[256];
    /* The application's page is outside a "created" scope. */
    snprintf(frame, sizeof frame,
             "{\"type\":\"call\",\"id\":1,\"command\":\"contents.get\",\"contents\":\"%s\"}",
             mine_id);
    script_send(&script, frame);
    CHECK(strstr(script.last, "\"code\":\"outOfScope\"") != NULL);
    script_send(&script, "{\"type\":\"call\",\"id\":2,\"command\":\"contents.list\"}");
    CHECK(strstr(script.last, "{\"items\":[]}") != NULL);
    char pid[64];
    profile_id_of(p, pid, sizeof pid);
    snprintf(frame, sizeof frame,
             "{\"type\":\"call\",\"id\":3,\"command\":\"contents.create\",\"args\":{},"
             "\"profile\":\"%s\"}",
             pid);
    script_send(&script, frame);
    REQUIRE(strstr(script.last, "\"type\":\"result\",\"id\":3") != NULL);
    char created_id[32] = "";
    {
        const char* at = strstr(script.last, "\"value\":{\"id\":\"");
        REQUIRE(at != NULL);
        sscanf(at + strlen("\"value\":{\"id\":\""), "%31[0-9]", created_id);
    }
    snprintf(frame, sizeof frame,
             "{\"type\":\"call\",\"id\":4,\"command\":\"contents.get\",\"contents\":\"%s\"}",
             created_id);
    script_send(&script, frame);
    CHECK(strstr(script.last, "\"type\":\"result\",\"id\":4") != NULL);
    REQUIRE(named_count(&created, "contents.created") == 1);
    CHECK(strstr(created.payloads[0],
                 "\"createdBy\":{\"kind\":\"session\",\"session\":\"agent\"}") != NULL);
    /* Granting widens the scope. */
    bdk_error_t err = 0;
    CHECK(bdk_session_grant_contents(s, mine, &err) == BDK_OK);
    snprintf(frame, sizeof frame,
             "{\"type\":\"call\",\"id\":5,\"command\":\"contents.get\",\"contents\":\"%s\"}",
             mine_id);
    script_send(&script, frame);
    CHECK(strstr(script.last, "\"type\":\"result\",\"id\":5") != NULL);
    /* The page belongs to the application: it destroys it like any other. */
    bdk_contents_t session_page = (bdk_contents_t)strtoull(created_id, NULL, 10);
    CHECK(bdk_contents_destroy(session_page) == BDK_OK);
}

static void session_events_follow_its_scope(void) {
    NEED_HOST();
    start_with(AUTOMATION_ON);
    static t_script_t script;
    define_echo(&script);
    bdk_profile_t p = t_new_profile("A");
    bdk_contents_t outside = t_new_contents(p);
    bdk_contents_t inside = t_new_contents(p);
    bdk_session_t s = session("{\"name\":\"agent\",\"capabilities\":[\"contents.read\"],"
                              "\"scope\":{\"contents\":\"explicit\"}}");
    bdk_error_t err = 0;
    CHECK(bdk_session_grant_contents(s, inside, &err) == BDK_OK);
    static t_task_log_t log;
    (void)run(s, "echo", "null", &log);
    script_send(&script, "{\"type\":\"subscribe\",\"id\":7,\"event\":\"contents.navigated\"}");
    CHECK(strstr(script.last, "\"type\":\"result\",\"id\":7") != NULL);
    script_send(&script, "{\"type\":\"subscribe\",\"id\":8,\"event\":\"history.visited\"}");
    CHECK(strstr(script.last, "\"code\":\"denied\"") != NULL);
    int before = script.frames;
    REQUIRE(bdk_contents_load(outside, S("https://outside.example/"), &err) == BDK_OK);
    t_pump();
    CHECK(script.frames == before);
    REQUIRE(bdk_contents_load(inside, S("https://inside.example/"), &err) == BDK_OK);
    t_pump();
    CHECK(script.frames == before + 1);
    CHECK(strstr(script.last, "\"type\":\"event\",\"id\":7,\"name\":\"contents.navigated\"") !=
          NULL);
    CHECK(strstr(script.last, "\"url\":\"https://inside.example/\"") != NULL);
    script_send(&script, "{\"type\":\"unsubscribe\",\"id\":7}");
    REQUIRE(bdk_contents_load(inside, S("https://inside.example/2"), &err) == BDK_OK);
    t_pump();
    CHECK(script.frames == before + 1);
}

/* ---- contents.* answered by the core, from the host ---- */

static void the_host_drives_pages_with_contents_commands(void) {
    start_with(NULL);
    bdk_profile_t p = t_new_profile("A");
    REQUIRE(bdk_harness_script_navigation(t_runtime, "https://a.example/", 0, "A") == BDK_OK);
    REQUIRE(bdk_harness_script_navigation(t_runtime, "https://b.example/", 0, "B") == BDK_OK);
    t_capture_t cap;
    host_invoke("contents.create", "{\"url\":\"https://a.example/\"}", p, 0, &cap);
    REQUIRE(!cap.has_error);
    char id[32] = "";
    REQUIRE(t_json_string(cap.json, "id", id, sizeof id));
    bdk_contents_t page = (bdk_contents_t)strtoull(id, NULL, 10);
    host_invoke("contents.waitFor", "{\"load\":\"finished\",\"timeoutMs\":5000}", 0, page, &cap);
    CHECK(cap.results == 1 && !cap.has_error);
    host_invoke("contents.list", "{}", p, 0, &cap);
    CHECK(strstr(cap.json, id) != NULL);
    host_invoke("contents.navigate", "{\"url\":\"https://b.example/\"}", 0, page, &cap);
    CHECK(!cap.has_error);
    CHECK(strstr(cap.json, "\"url\":\"https://b.example/\"") != NULL);
    host_invoke("contents.waitFor", "{\"url\":\"https://b.*\",\"load\":\"finished\"}", 0, page,
                &cap);
    CHECK(!cap.has_error);
    host_invoke("contents.get", "{}", 0, page, &cap);
    CHECK(strstr(cap.json, "\"canGoBack\":true") != NULL);
    CHECK(strstr(cap.json, "\"canGoForward\":false") != NULL);
    host_invoke("contents.goForward", "{}", 0, page, &cap);
    CHECK(cap.has_error && strcmp(cap.code, "invalidArguments") == 0);
    host_invoke("contents.goBack", "{}", 0, page, &cap);
    CHECK(!cap.has_error);
    host_invoke("contents.waitFor", "{\"url\":\"https://a.example/\",\"load\":\"finished\"}", 0,
                page, &cap);
    CHECK(!cap.has_error);
    host_invoke("contents.setZoom", "{\"zoom\":1.5}", 0, page, &cap);
    CHECK(!cap.has_error);
    host_invoke("contents.get", "{}", 0, page, &cap);
    CHECK(strstr(cap.json, "\"zoom\":1.5") != NULL);
    host_invoke("contents.setMuted", "{\"muted\":true}", 0, page, &cap);
    CHECK(!cap.has_error);
    /* A condition that never holds ends at its deadline. */
    host_invoke("contents.waitFor", "{\"url\":\"https://never/*\",\"timeoutMs\":50}", 0, page,
                &cap);
    for (int i = 0; i < 200 && cap.results == 0; ++i)
        t_pump();
    CHECK(cap.has_error && strcmp(cap.code, "timeout") == 0);
    host_invoke("contents.waitFor", "{}", 0, page, &cap);
    CHECK(cap.has_error && strcmp(cap.code, "invalidArguments") == 0);
    host_invoke("contents.close", "{}", 0, page, &cap);
    CHECK(!cap.has_error);
    CHECK(bdk_contents_destroy(page) == BDK_OK);
}

static void a_failed_navigation_rejects(void) {
    start_with(NULL);
    bdk_profile_t p = t_new_profile("A");
    bdk_contents_t page = t_new_contents(p);
    REQUIRE(bdk_harness_script_navigation(t_runtime, "https://down.example/", -105, NULL) ==
            BDK_OK);
    t_capture_t cap;
    host_invoke("contents.navigate", "{\"url\":\"https://down.example/\"}", 0, page, &cap);
    for (int i = 0; i < 200 && cap.results == 0; ++i)
        t_pump();
    CHECK(cap.results == 1);
    CHECK(cap.has_error && strcmp(cap.domain, "navigation") == 0);
    host_invoke("contents.navigate", "{\"url\":\"not a url\"}", 0, page, &cap);
    CHECK(cap.has_error);
}

static void page_commands_need_a_page(void) {
    start_with(NULL);
    t_capture_t cap;
    host_invoke("contents.get", "{}", 0, 0, &cap);
    CHECK(cap.has_error && strcmp(cap.code, "invalidArguments") == 0);
    host_invoke("contents.evaluate", "{\"expression\":\"1\"}", 0, 0, &cap);
    CHECK(cap.has_error && strcmp(cap.code, "invalidArguments") == 0);
    host_invoke("contents.create", "{}", 0, 0, &cap);
    CHECK(cap.has_error && strcmp(cap.code, "invalidArguments") == 0); /* needs a profile */
}

/* ---- scripts.* ---- */

static void a_script_predicate_needs_contents_script(void) {
    NEED_HOST();
    start_with(AUTOMATION_ON);
    static t_script_t script;
    define_echo(&script);
    bdk_profile_t p = t_new_profile("A");
    bdk_contents_t page = t_new_contents(p);
    char id[32], frame[256];
    id_of(page, id, sizeof id);
    bdk_session_t s =
        session("{\"name\":\"reader\",\"capabilities\":[\"contents.read\"],\"scope\":{\"contents\":"
                "\"all\"}}");
    static t_task_log_t log;
    (void)run(s, "echo", "null", &log);
    snprintf(frame, sizeof frame,
             "{\"type\":\"call\",\"id\":1,\"command\":\"contents.waitFor\",\"contents\":"
             "\"%s\",\"args\":{\"script\":\"document.forms[0].submit(); true\"}}",
             id);
    script_send(&script, frame);
    CHECK(strstr(script.last, "\"type\":\"error\",\"id\":1") != NULL);
    CHECK(strstr(script.last, "\"domain\":\"capability\",\"code\":\"denied\"") != NULL);
    /* A URL wait is still a read: it runs, and here ends at its deadline. */
    snprintf(frame, sizeof frame,
             "{\"type\":\"call\",\"id\":2,\"command\":\"contents.waitFor\",\"contents\":"
             "\"%s\",\"args\":{\"url\":\"https://never/*\",\"timeoutMs\":50}}",
             id);
    script_send(&script, frame);
    for (int i = 0; i < 200 && strstr(script.last, "\"id\":2") == NULL; ++i)
        t_pump();
    CHECK(strstr(script.last, "\"type\":\"error\",\"id\":2") != NULL);
    CHECK(strstr(script.last, "\"code\":\"timeout\"") != NULL);
}

static void the_core_keeps_user_scripts(void) {
    start_with(NULL);
    bdk_profile_t p = t_new_profile("A");
    unsigned changes =
        bdk_harness_automation_count(t_runtime, BDK_HARNESS_AUTOMATION_SCRIPTS_CHANGED);
    t_capture_t cap;
    host_invoke("scripts.register",
                "{\"script\":{\"id\":\"reader\",\"matches\":[\"https://*/*\"],\"runAt\":"
                "\"documentEnd\",\"javascript\":[\"document.body.dataset.r=1\"]}}",
                p, 0, &cap);
    CHECK(!cap.has_error);
    host_invoke("scripts.register", "{\"script\":{\"id\":\"x\",\"matches\":[],\"css\":[\"a{}\"]}}",
                p, 0, &cap);
    CHECK(cap.has_error && strcmp(cap.code, "invalidArguments") == 0);
    host_invoke("scripts.register", "{\"script\":{\"id\":\"x\",\"matches\":[\"<all_urls>\"]}}", p,
                0, &cap);
    CHECK(cap.has_error && strcmp(cap.code, "invalidArguments") == 0);
    host_invoke("scripts.register", "{\"script\":{\"id\":\"x\",\"runAt\":\"later\"}}", p, 0, &cap);
    CHECK(cap.has_error && strcmp(cap.code, "invalidArguments") == 0);
    host_invoke("scripts.list", "{}", p, 0, &cap);
    CHECK(strstr(cap.json, "\"id\":\"reader\"") != NULL);
    CHECK(strstr(cap.json, "\"world\":\"reader\"") != NULL); /* an isolated world of its own */
    CHECK(strstr(cap.json, "\"runAt\":\"documentEnd\"") != NULL);
    host_invoke("scripts.unregister", "{\"id\":\"nope\"}", p, 0, &cap);
    CHECK(cap.has_error);
    host_invoke("scripts.unregister", "{\"id\":\"reader\"}", p, 0, &cap);
    CHECK(!cap.has_error);
    host_invoke("scripts.list", "{}", p, 0, &cap);
    CHECK(strcmp(cap.json, "{\"scripts\":[]}") == 0);
    if (bdk_harness_automation_hooks())
        CHECK(bdk_harness_automation_count(t_runtime, BDK_HARNESS_AUTOMATION_SCRIPTS_CHANGED) ==
              changes + 2);
}

static void a_sessions_user_scripts_go_with_it(void) {
    NEED_HOST();
    start_with(AUTOMATION_ON);
    static t_script_t script;
    define_echo(&script);
    bdk_profile_t p = t_new_profile("A");
    t_capture_t cap;
    host_invoke("scripts.register",
                "{\"script\":{\"id\":\"app\",\"matches\":[\"https://*/*\"],\"css\":[\"a{}\"]}}", p,
                0, &cap);
    bdk_session_t s = session("{\"name\":\"agent\",\"capabilities\":[\"contents.script\"],"
                              "\"scope\":{\"contents\":\"all\"}}");
    static t_task_log_t log;
    (void)run(s, "echo", "null", &log);
    char pid[64], frame[320];
    profile_id_of(p, pid, sizeof pid);
    snprintf(frame, sizeof frame,
             "{\"type\":\"call\",\"id\":1,\"command\":\"scripts.register\",\"profile\":"
             "\"%s\",\"args\":{\"script\":{\"id\":\"agent\",\"matches\":[\"https://*/*\"],"
             "\"javascript\":[\"1\"]}}}",
             pid);
    script_send(&script, frame);
    CHECK(strstr(script.last, "\"type\":\"result\",\"id\":1") != NULL);
    /* The application's script is not the session's to remove or replace. */
    snprintf(frame, sizeof frame,
             "{\"type\":\"call\",\"id\":2,\"command\":\"scripts.unregister\",\"profile\":"
             "\"%s\",\"args\":{\"id\":\"app\"}}",
             pid);
    script_send(&script, frame);
    CHECK(strstr(script.last, "\"code\":\"denied\"") != NULL);
    bdk_error_t err = 0;
    CHECK(bdk_session_revoke(s, &err) == BDK_OK);
    t_pump();
    host_invoke("scripts.list", "{}", p, 0, &cap);
    CHECK(strstr(cap.json, "\"id\":\"app\"") != NULL);
    CHECK(strstr(cap.json, "\"id\":\"agent\"") == NULL);
}

static void a_session_limited_to_some_pages_cannot_register_scripts(void) {
    NEED_HOST();
    start_with(AUTOMATION_ON);
    static t_script_t script;
    define_echo(&script);
    bdk_profile_t p = t_new_profile("A");
    char pid[64], frame[320];
    profile_id_of(p, pid, sizeof pid);
    snprintf(frame, sizeof frame,
             "{\"type\":\"call\",\"id\":1,\"command\":\"scripts.register\",\"profile\":"
             "\"%s\",\"args\":{\"script\":{\"id\":\"agent\",\"matches\":[\"https://*/*\"],"
             "\"javascript\":[\"1\"]}}}",
             pid);
    const char* scopes[] = {"created", "explicit"};
    for (size_t i = 0; i < sizeof scopes / sizeof scopes[0]; ++i) {
        char options[160];
        snprintf(options, sizeof options,
                 "{\"name\":\"agent\",\"capabilities\":[\"contents.script\"],\"scope\":{"
                 "\"contents\":\"%s\"}}",
                 scopes[i]);
        bdk_session_t s = session(options);
        static t_task_log_t log;
        (void)run(s, "echo", "null", &log);
        script_send(&script, frame);
        CHECK(strstr(script.last, "\"type\":\"error\",\"id\":1") != NULL);
        CHECK(strstr(script.last, "\"code\":\"outOfScope\"") != NULL);
        bdk_error_t err = 0;
        CHECK(bdk_session_revoke(s, &err) == BDK_OK);
        t_pump();
    }
    t_capture_t cap;
    host_invoke("scripts.list", "{}", p, 0, &cap);
    CHECK(strcmp(cap.json, "{\"scripts\":[]}") == 0);
}

static void sessions_sharing_a_name_own_their_scripts_apart(void) {
    NEED_HOST();
    start_with(AUTOMATION_ON);
    static t_script_t script;
    define_echo(&script);
    bdk_profile_t p = t_new_profile("A");
    char pid[64], frame[320];
    profile_id_of(p, pid, sizeof pid);
    const char* options =
        "{\"name\":\"agent\",\"capabilities\":[\"contents.script\"],\"scope\":{\"contents\":"
        "\"all\"}}";
    bdk_session_t first = session(options);
    static t_task_log_t first_log;
    (void)run(first, "echo", "null", &first_log);
    snprintf(frame, sizeof frame,
             "{\"type\":\"call\",\"id\":1,\"command\":\"scripts.register\",\"profile\":"
             "\"%s\",\"args\":{\"script\":{\"id\":\"first\",\"matches\":[\"https://*/*\"],"
             "\"javascript\":[\"1\"]}}}",
             pid);
    script_send(&script, frame);
    CHECK(strstr(script.last, "\"type\":\"result\",\"id\":1") != NULL);

    bdk_session_t second = session(options);
    static t_task_log_t second_log;
    (void)run(second, "echo", "null", &second_log);
    snprintf(frame, sizeof frame,
             "{\"type\":\"call\",\"id\":1,\"command\":\"scripts.register\",\"profile\":"
             "\"%s\",\"args\":{\"script\":{\"id\":\"second\",\"matches\":[\"https://*/*\"],"
             "\"javascript\":[\"2\"]}}}",
             pid);
    script_send(&script, frame);
    CHECK(strstr(script.last, "\"type\":\"result\",\"id\":1") != NULL);
    /* The same name is not the same session: the first's script is not the second's. */
    snprintf(frame, sizeof frame,
             "{\"type\":\"call\",\"id\":2,\"command\":\"scripts.unregister\",\"profile\":"
             "\"%s\",\"args\":{\"id\":\"first\"}}",
             pid);
    script_send(&script, frame);
    CHECK(strstr(script.last, "\"code\":\"denied\"") != NULL);
    bdk_error_t err = 0;
    CHECK(bdk_session_revoke(first, &err) == BDK_OK);
    t_pump();
    t_capture_t cap;
    host_invoke("scripts.list", "{}", p, 0, &cap);
    CHECK(strstr(cap.json, "\"id\":\"first\"") == NULL);
    CHECK(strstr(cap.json, "\"id\":\"second\"") != NULL);
}

static void a_subscription_for_a_page_gets_that_pages_events(void) {
    start_with(NULL);
    bdk_profile_t p = t_new_profile("A");
    bdk_contents_t a = t_new_contents(p), b = t_new_contents(p);
    bdk_invoke_options_t o;
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.contents = a;
    static t_named_t only_a, every;
    memset(&only_a, 0, sizeof only_a);
    memset(&every, 0, sizeof every);
    REQUIRE(bdk_subscribe_named(t_runtime, S("contents.attentionRequested"), S(""), &o, on_named,
                                &only_a, NULL) != 0);
    REQUIRE(bdk_subscribe_named(t_runtime, S("contents.attentionRequested"), S(""), NULL, on_named,
                                &every, NULL) != 0);
    t_capture_t cap;
    host_invoke("contents.requestAttention", "{}", 0, b, &cap);
    CHECK(!cap.has_error);
    host_invoke("contents.requestAttention", "{}", 0, a, &cap);
    CHECK(!cap.has_error);
    t_pump();
    CHECK(every.count == 2);
    REQUIRE(only_a.count == 1);
    char id[32], want[48];
    id_of(a, id, sizeof id);
    snprintf(want, sizeof want, "\"contents\":\"%s\"", id);
    CHECK(strstr(only_a.payloads[0], want) != NULL);
}

/* ---- debug.* and devtools.* ---- */

static void the_protocol_follows_the_configuration(void) {
    start_with("\"devtools\":{\"frontend\":\"disabled\",\"protocol\":false}");
    bdk_profile_t p = t_new_profile("A");
    bdk_contents_t page = t_new_contents(p);
    t_capture_t cap;
    host_invoke("debug.attach", "{}", 0, page, &cap);
    CHECK(cap.has_error && strcmp(cap.code, "protocolDisabled") == 0);
    host_invoke("devtools.open", "{}", 0, page, &cap);
    CHECK(cap.has_error && strcmp(cap.domain, "devtools") == 0 &&
          strcmp(cap.code, "disabled") == 0);
    host_invoke("devtools.open", "{}", 0, 0, &cap);
    CHECK(cap.has_error && strcmp(cap.code, "invalidArguments") == 0);
}

static void sessions_cannot_reach_beyond_a_page(void) {
    NEED_HOST();
    start_with(AUTOMATION_ON);
    static t_script_t script;
    define_echo(&script);
    bdk_session_t s = session("{\"name\":\"agent\",\"capabilities\":[\"contents.debug\"]}");
    static t_task_log_t log;
    (void)run(s, "echo", "null", &log);
    script_send(&script, "{\"type\":\"call\",\"id\":1,\"command\":\"debug.send\",\"args\":"
                         "{\"session\":\"d1\",\"method\":\"Browser.close\"}}");
    CHECK(strstr(script.last, "\"code\":\"denied\"") != NULL);
    script_send(&script, "{\"type\":\"call\",\"id\":2,\"command\":\"debug.send\",\"args\":"
                         "{\"session\":\"d1\",\"method\":\"Target.getTargets\"}}");
    CHECK(strstr(script.last, "\"code\":\"denied\"") != NULL);
    /* devtools.* is Swift's only. */
    script_send(&script, "{\"type\":\"call\",\"id\":3,\"command\":\"devtools.open\"}");
    CHECK(strstr(script.last, "\"code\":\"denied\"") != NULL);
}

/* ---- Binary results ---- */

static void captures_carry_bytes_outside_the_json(void) {
    NEED_HOST(); /* the stub's fixed bytes; a real page's capture is an engine scenario */
    start_with(AUTOMATION_ON);
    static t_script_t script;
    define_echo(&script);
    bdk_profile_t p = t_new_profile("A");
    bdk_contents_t page = t_new_contents(p);
    t_capture_t cap;
    host_invoke("contents.screenshot", "{}", 0, page, &cap);
    CHECK(!cap.has_error && cap.has_binary);
    CHECK(strstr(cap.json, "\"mimeType\":\"image/png\"") != NULL);
    host_invoke("contents.pdf", "{}", 0, page, &cap);
    CHECK(!cap.has_error && cap.has_binary);
    CHECK(cap.progress >= 1);
    bdk_session_t s = session(
        "{\"name\":\"agent\",\"capabilities\":[\"contents.capture\"],\"scope\":{\"contents\":"
        "\"all\"}}");
    static t_task_log_t log;
    (void)run(s, "echo", "null", &log);
    char frame[256];
    snprintf(frame, sizeof frame,
             "{\"type\":\"call\",\"id\":1,\"command\":\"contents.screenshot\",\"contents\":"
             "\"%llu\"}",
             (unsigned long long)page);
    script_send(&script, frame);
    CHECK(strstr(script.last, "\"binary\":\"iVBORw0KGgo") != NULL); /* a PNG, base64 */
}

const t_test_t t_automation_tests[] = {
    {"automation.session_options_are_checked", session_options_are_checked},
    {"automation.run_needs_enabled_and_listed", run_needs_enabled_and_listed},
    {"automation.task_finishes_with_the_scripts_result", task_finishes_with_the_scripts_result},
    {"automation.task_calls_commands_as_the_session", task_calls_commands_as_the_session},
    {"automation.refusals_before_the_broker_are_audited", refusals_before_the_broker_are_audited},
    {"automation.task_progress_and_log_come_before_finished",
     task_progress_and_log_come_before_finished},
    {"automation.task_cancel_ends_it", task_cancel_ends_it},
    {"automation.revoking_the_session_stops_its_tasks_and_calls",
     revoking_the_session_stops_its_tasks_and_calls},
    {"automation.a_host_crash_ends_every_task", a_host_crash_ends_every_task},
    {"automation.a_failing_script_finishes_with_its_message",
     a_failing_script_finishes_with_its_message},
    {"automation.a_script_that_uses_its_cpu_budget_is_stopped",
     a_script_that_uses_its_cpu_budget_is_stopped},
    {"automation.a_cpu_budget_that_is_not_whole_milliseconds_is_refused",
     a_cpu_budget_that_is_not_whole_milliseconds_is_refused},
    {"automation.pages_a_session_creates_are_in_its_scope",
     pages_a_session_creates_are_in_its_scope},
    {"automation.session_events_follow_its_scope", session_events_follow_its_scope},
    {"automation.the_host_drives_pages_with_contents_commands",
     the_host_drives_pages_with_contents_commands},
    {"automation.a_failed_navigation_rejects", a_failed_navigation_rejects},
    {"automation.page_commands_need_a_page", page_commands_need_a_page},
    {"automation.a_script_predicate_needs_contents_script",
     a_script_predicate_needs_contents_script},
    {"automation.the_core_keeps_user_scripts", the_core_keeps_user_scripts},
    {"automation.a_sessions_user_scripts_go_with_it", a_sessions_user_scripts_go_with_it},
    {"automation.a_session_limited_to_some_pages_cannot_register_scripts",
     a_session_limited_to_some_pages_cannot_register_scripts},
    {"automation.sessions_sharing_a_name_own_their_scripts_apart",
     sessions_sharing_a_name_own_their_scripts_apart},
    {"automation.a_subscription_for_a_page_gets_that_pages_events",
     a_subscription_for_a_page_gets_that_pages_events},
    {"automation.the_protocol_follows_the_configuration", the_protocol_follows_the_configuration},
    {"automation.sessions_cannot_reach_beyond_a_page", sessions_cannot_reach_beyond_a_page},
    {"automation.captures_carry_bytes_outside_the_json", captures_carry_bytes_outside_the_json},
    {NULL, NULL},
};
