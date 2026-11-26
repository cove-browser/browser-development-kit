/* The navigation policy on Chromium (T-SWF-11; swift-api.md 6.3; conventions.md 16): an
   application load, a link, a script navigation, a server redirect and the first navigation of a
   window.open page are each asked; a cancel sends no request; a redirect requests only the new URL.
   The pages come from a loopback HTTP server in a child process, which the engine's test
   interceptor leaves to the network, and which writes the path of every request it receives to a
   log the scenarios read. The rules shared with the stub (allow, cancel and redirect now and later,
   stale decisions, the handler's lifetime, reloads and history) are tests_navigation_policy.c,
   which runs here too. Built only into the engine conformance app (BDK_HARNESS_ENGINE_TESTS). */
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "bdk_testing.h"
#include "test_support.h"

/* ---- the loopback server ---- */

static pid_t g_server;
static int g_port;
static char g_log[512];

/* Appends `text` to `buf` (of `cap` bytes, NUL-terminated); the child may call no stdio after
   fork, since another thread of the app may have held its locks. */
static void append(char* buf, size_t cap, const char* text) {
    size_t used = strlen(buf), n = strlen(text);
    if (used + n >= cap)
        n = cap - used - 1;
    memcpy(buf + used, text, n);
    buf[used + n] = '\0';
}

static void append_int(char* buf, size_t cap, int v) {
    char digits[16];
    int i = (int)sizeof digits - 1;
    digits[i] = '\0';
    do {
        digits[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v > 0 && i > 0);
    append(buf, cap, digits + i);
}

/* GET /redirect/<path> answers 302 to /<path>; anything else a page titled with its path. Only
   system calls and string functions: it runs in a child forked from a threaded process. */
static void serve(int listener) {
    for (;;) {
        int fd = accept(listener, NULL, NULL);
        if (fd < 0)
            continue;
        char request[4096];
        ssize_t n = read(fd, request, sizeof request - 1);
        request[n > 0 ? n : 0] = '\0';
        char path[1024] = "/";
        const char* sp = strchr(request, ' ');
        if (sp) {
            size_t len = strcspn(sp + 1, " \r\n");
            if (len > 0 && len < sizeof path) {
                memcpy(path, sp + 1, len);
                path[len] = '\0';
            }
        }
        int log = open(g_log, O_WRONLY | O_APPEND | O_CREAT, 0600);
        if (log >= 0) {
            char line[1100] = "";
            append(line, sizeof line, path);
            append(line, sizeof line, "\n");
            (void)!write(log, line, strlen(line));
            close(log);
        }
        char reply[2048] = "";
        if (strncmp(path, "/redirect/", 10) == 0) {
            append(reply, sizeof reply, "HTTP/1.1 302 Found\r\nLocation: /");
            append(reply, sizeof reply, path + 10);
            append(reply, sizeof reply, "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        } else {
            char body[1200] = "<!doctype html><meta charset=utf-8><title>";
            append(body, sizeof body, path);
            append(body, sizeof body, "</title><body>");
            append(reply, sizeof reply,
                   "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nCache-Control: no-store\r\n"
                   "Connection: close\r\nContent-Length: ");
            append_int(reply, sizeof reply, (int)strlen(body));
            append(reply, sizeof reply, "\r\n\r\n");
            append(reply, sizeof reply, body);
        }
        (void)!write(fd, reply, strlen(reply));
        close(fd);
    }
}

static int start_server(void) {
    const char* tmp = getenv("TMPDIR");
    snprintf(g_log, sizeof g_log, "%s/bdk-policy-requests-%ld.log", tmp && *tmp ? tmp : "/tmp",
             (long)getpid());
    unlink(g_log);
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof a;
    if (listener < 0 || bind(listener, (struct sockaddr*)&a, sizeof a) != 0 ||
        listen(listener, 16) != 0 || getsockname(listener, (struct sockaddr*)&a, &len) != 0)
        return 0;
    g_port = ntohs(a.sin_port);
    g_server = fork();
    if (g_server == 0) {
        serve(listener);
        _exit(0);
    }
    close(listener);
    return g_server > 0;
}

static void stop_server(void) {
    if (g_server > 0) {
        kill(g_server, SIGKILL); /* not SIGTERM: the child has Chromium's handler for it */
        waitpid(g_server, NULL, 0);
        g_server = 0;
    }
    unlink(g_log);
}

/* The server's URL for `path`. */
static const char* at(const char* path) {
    static char buf[4][256];
    static int next;
    char* out = buf[next++ % 4];
    snprintf(out, 256, "http://127.0.0.1:%d%s", g_port, path);
    return out;
}

/* Whether the server was asked for exactly `path`. */
static int requested(const char* path) {
    FILE* f = fopen(g_log, "r");
    if (!f)
        return 0;
    char line[1100];
    int found = 0;
    while (!found && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = '\0';
        found = strcmp(line, path) == 0;
    }
    fclose(f);
    return found;
}

/* ---- the policy ---- */

typedef struct seen {
    int calls;
    uint32_t answer;      /* 0: keep the decision in held */
    const char* redirect; /* the URL of a BDK_NAVIGATION_REDIRECT answer */
    const char* cancel;   /* cancel a navigation whose URL contains this, whatever `answer` is */
    bdk_context_t held;
    bdk_contents_t contents;
    char navigation[1024];
    char all[4096]; /* every navigation asked about, in order */
} seen_t;

static seen_t g_seen;

static void on_policy(void* ctx, bdk_context_t decision, bdk_contents_t contents,
                      bdk_string_t navigation) {
    seen_t* s = (seen_t*)ctx;
    ++s->calls;
    s->contents = contents;
    snprintf(s->navigation, sizeof s->navigation, "%.*s", (int)navigation.length, navigation.data);
    size_t used = strlen(s->all);
    snprintf(s->all + used, sizeof s->all - used, "%s\n", s->navigation);
    uint32_t answer = s->answer;
    if (s->cancel && strstr(s->navigation, s->cancel))
        answer = BDK_NAVIGATION_CANCEL;
    if (!answer) {
        s->held = decision;
        return;
    }
    bdk_error_t err = 0;
    CHECK(bdk_navigation_policy_decide(decision, answer, S(s->redirect ? s->redirect : ""), &err) ==
          BDK_OK);
}

static int has(const char* text, const char* needle) {
    return strstr(text, needle) != NULL;
}

static void current_url(bdk_contents_t c, char* out, size_t n) {
    bdk_contents_state_t st;
    memset(&st, 0, sizeof st);
    st.struct_size = sizeof st;
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_state(c, &st, &err) == BDK_OK);
    snprintf(out, n, "%.*s", (int)st.url.length, st.url.data ? st.url.data : "");
}

static int is_loading(bdk_contents_t c) {
    bdk_contents_state_t st;
    memset(&st, 0, sizeof st);
    st.struct_size = sizeof st;
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_state(c, &st, &err) == BDK_OK);
    return st.loading;
}

/* Pumps until the page has stopped loading at `url` (with its title, the server's page). */
static int wait_at(bdk_contents_t c, const char* url) {
    char now[256] = "";
    for (int i = 0; i < 400; ++i) {
        current_url(c, now, sizeof now);
        if (strcmp(now, url) == 0 && !is_loading(c))
            return 1;
        t_pump();
    }
    printf("    the page is at %s, not %s\n", now, url);
    return 0;
}

/* Pumps until the policy has been asked `calls` times. */
static int wait_calls(int calls) {
    for (int i = 0; i < 400 && g_seen.calls < calls; ++i)
        t_pump();
    if (g_seen.calls != calls)
        printf("    asked %d times, not %d\n", g_seen.calls, calls);
    return g_seen.calls == calls;
}

/* Pumps a little more, so a request the page should not have sent would have reached the server.
 */
static void settle(void) {
    for (int i = 0; i < 40; ++i)
        t_pump();
}

static t_events_t g_events;

static int failed_with(const char* code) {
    char want[64];
    snprintf(want, sizeof want, "\"code\":\"%s\"", code);
    for (int i = 0; i < g_events.count; ++i) {
        if (g_events.kind[i] == BDK_CONTENTS_EVENT_NAVIGATION_FAILED &&
            has(g_events.detail[i], want))
            return 1;
    }
    return 0;
}

/* A runtime with the policy handler (none when `with_handler` is 0) and a watched page. */
static bdk_contents_t start(int with_handler, uint32_t answer) {
    memset(&g_seen, 0, sizeof g_seen);
    g_seen.answer = answer;
    t_start();
    if (with_handler) {
        bdk_error_t err = 0;
        REQUIRE(bdk_navigation_policy_set_handler(t_runtime, on_policy, &g_seen, NULL, &err) ==
                BDK_OK);
    }
    t_freeze();
    bdk_contents_t c = t_new_contents(t_new_profile("Policy"));
    t_ev_init(&g_events);
    REQUIRE(bdk_contents_subscribe(c, t_on_contents_event, &g_events, t_on_event_release) != 0);
    return c;
}

static void load(bdk_contents_t c, const char* url) {
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_load(c, S(url), &err) == BDK_OK);
    t_pump();
}

static void run(bdk_contents_t c, const char* js, int gesture) {
    REQUIRE(bdk_testing_execute_script(c, S(js), (uint8_t)gesture) == BDK_OK);
    t_pump();
}

#define NEEDS_SERVER()                                                                             \
    do {                                                                                           \
        if (!start_server()) {                                                                     \
            printf("    skipped: no loopback socket\n");                                           \
            return;                                                                                \
        }                                                                                          \
    } while (0)

/* ---- scenarios ---- */

static void test_with_no_handler_the_page_loads_unasked(void) {
    NEEDS_SERVER();
    bdk_contents_t c = start(0, 0);
    load(c, at("/plain"));
    CHECK(wait_at(c, at("/plain")));
    CHECK(requested("/plain"));
    CHECK(g_seen.calls == 0);
    stop_server();
}

static void test_an_application_load_is_asked_and_a_cancel_sends_no_request(void) {
    NEEDS_SERVER();
    bdk_contents_t c = start(1, BDK_NAVIGATION_ALLOW);
    load(c, at("/allowed"));
    CHECK(wait_at(c, at("/allowed")));
    CHECK(g_seen.calls == 1);
    CHECK(g_seen.contents == c);
    char want[160];
    snprintf(want, sizeof want, "\"url\":\"%s\"", at("/allowed"));
    CHECK(has(g_seen.navigation, want));
    CHECK(has(g_seen.navigation, "\"type\":\"load\""));
    CHECK(has(g_seen.navigation, "\"initiator\":{\"kind\":\"application\"}"));
    CHECK(has(g_seen.navigation, "\"isUserInitiated\":true"));
    CHECK(requested("/allowed"));

    g_seen.answer = BDK_NAVIGATION_CANCEL;
    t_ev_init(&g_events);
    load(c, at("/cancelled"));
    CHECK(wait_calls(2));
    settle();
    CHECK(!requested("/cancelled"));
    CHECK(failed_with("cancelledByPolicy"));
    CHECK(wait_at(c, at("/allowed")));
    stop_server();
}

static void test_a_redirect_requests_only_the_new_url(void) {
    NEEDS_SERVER();
    bdk_contents_t c = start(1, BDK_NAVIGATION_REDIRECT);
    char instead[128];
    snprintf(instead, sizeof instead, "%s", at("/instead"));
    g_seen.redirect = instead;
    load(c, at("/asked"));
    CHECK(wait_at(c, instead));
    /* The application chose the target: its start is not asked again. */
    CHECK(g_seen.calls == 1);
    CHECK(failed_with("redirectedByPolicy"));
    CHECK(requested("/instead"));
    CHECK(!requested("/asked"));
    stop_server();
}

static void test_links_and_script_navigations_are_asked(void) {
    NEEDS_SERVER();
    bdk_contents_t c = start(1, BDK_NAVIGATION_ALLOW);
    load(c, at("/start"));
    REQUIRE(wait_at(c, at("/start")));
    char origin[64];
    snprintf(origin, sizeof origin, "\"origin\":\"http://127.0.0.1:%d\"", g_port);

    /* A link the user follows. */
    run(c,
        "const a = document.createElement('a'); a.href = '/linked';"
        "document.body.appendChild(a); a.click();",
        1);
    CHECK(wait_at(c, at("/linked")));
    CHECK(g_seen.calls == 2);
    CHECK(has(g_seen.navigation, "\"initiator\":{\"kind\":\"page\""));
    CHECK(has(g_seen.navigation, origin));
    CHECK(has(g_seen.navigation, "\"isUserInitiated\":true"));

    /* A script navigation with no gesture, cancelled. */
    g_seen.answer = BDK_NAVIGATION_CANCEL;
    t_ev_init(&g_events);
    run(c, "location.assign('/scripted')", 0);
    CHECK(wait_calls(3));
    CHECK(has(g_seen.navigation, "/scripted\""));
    CHECK(has(g_seen.navigation, "\"initiator\":{\"kind\":\"page\""));
    CHECK(has(g_seen.navigation, "\"isUserInitiated\":false"));
    settle();
    CHECK(!requested("/scripted"));
    CHECK(failed_with("cancelledByPolicy"));
    CHECK(wait_at(c, at("/linked")));
    stop_server();
}

static void test_a_server_redirect_is_asked_and_can_be_cancelled(void) {
    NEEDS_SERVER();
    bdk_contents_t c = start(1, BDK_NAVIGATION_ALLOW);
    load(c, at("/redirect/target"));
    CHECK(wait_at(c, at("/target")));
    CHECK(g_seen.calls == 2);
    char want[160];
    snprintf(want, sizeof want, "\"redirectedFrom\":\"%s\"", at("/redirect/target"));
    CHECK(has(g_seen.navigation, "\"isRedirect\":true"));
    CHECK(has(g_seen.navigation, want));
    CHECK(requested("/redirect/target") && requested("/target"));

    /* Cancelled at the redirect: the redirect's target is never requested. */
    g_seen.cancel = "\"isRedirect\":true";
    t_ev_init(&g_events);
    load(c, at("/redirect/elsewhere"));
    CHECK(wait_calls(4));
    settle();
    CHECK(requested("/redirect/elsewhere"));
    CHECK(!requested("/elsewhere"));
    CHECK(failed_with("cancelledByPolicy"));
    CHECK(wait_at(c, at("/target")));
    stop_server();
}

/* Places every new page the engine asks about where the request says (accepts it). */
static bdk_ui_response_t accept_new_window(void* ctx, const bdk_ui_request_info_t* info) {
    (void)ctx;
    bdk_native_handle_t none;
    memset(&none, 0, sizeof none);
    bdk_error_t err = 0;
    CHECK(bdk_ui_complete(info->request, S("accept"), S(""), none, &err) == BDK_OK);
    return BDK_UI_HANDLED;
}

static void test_the_first_navigation_of_a_window_open_page_is_asked(void) {
    NEEDS_SERVER();
    memset(&g_seen, 0, sizeof g_seen);
    g_seen.answer = BDK_NAVIGATION_ALLOW;
    t_start();
    bdk_error_t err = 0;
    REQUIRE(bdk_navigation_policy_set_handler(t_runtime, on_policy, &g_seen, NULL, &err) == BDK_OK);
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_NEW_WINDOWS, accept_new_window, NULL, NULL,
                               &err) == BDK_OK);
    t_freeze();
    bdk_contents_t c = t_new_contents(t_new_profile("Policy"));
    load(c, at("/opener"));
    REQUIRE(wait_at(c, at("/opener")));
    g_seen.cancel = "/opened\"";
    run(c, "window.open('/opened')", 1);
    CHECK(wait_calls(2));
    CHECK(g_seen.contents != 0 && g_seen.contents != c);
    CHECK(has(g_seen.navigation, "/opened\""));
    CHECK(has(g_seen.navigation, "\"initiator\":{\"kind\":\"page\""));
    settle();
    CHECK(!requested("/opened"));
    stop_server();
}

const t_test_t t_engine_navigation_policy_tests[] = {
    {"engine_policy_with_no_handler_the_page_loads_unasked",
     test_with_no_handler_the_page_loads_unasked},
    {"engine_policy_an_application_load_is_asked_and_a_cancel_sends_no_request",
     test_an_application_load_is_asked_and_a_cancel_sends_no_request},
    {"engine_policy_a_redirect_requests_only_the_new_url",
     test_a_redirect_requests_only_the_new_url},
    {"engine_policy_links_and_script_navigations_are_asked",
     test_links_and_script_navigations_are_asked},
    {"engine_policy_a_server_redirect_is_asked_and_can_be_cancelled",
     test_a_server_redirect_is_asked_and_can_be_cancelled},
    {"engine_policy_the_first_navigation_of_a_window_open_page_is_asked",
     test_the_first_navigation_of_a_window_open_page_is_asked},
    {NULL, NULL},
};
