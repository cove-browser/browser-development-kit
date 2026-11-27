/* Engine scenarios for client certificates (ui-requests.md 5.3, T-UIR-13): a TLS server on the
   loopback address that requires a client certificate (openssl s_server), a client identity the
   engine offers from memory instead of the Keychain (bdk_testing "clientcert.identity"), and the
   client_certificates request that chooses it. The server's own certificate is self-signed, so a
   certificate error is proceeded past first. Built only into the engine conformance app
   (BDK_HARNESS_ENGINE_TESTS). POSIX process and socket calls. */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "bdk_testing.h"
#include "test_support.h"

void bdk_harness_engine_show(bdk_contents_t contents, int visible);
bdk_native_handle_t bdk_harness_engine_gesture(bdk_contents_t contents, int kind);

static char g_dir[256];
static pid_t g_server;
static int g_port;
static bdk_contents_t g_page;

typedef struct {
    int calls;
    char payload[2048];
    const char* completion; /* "accept" chooses certificate "0"; "deny" goes on without one */
} certificate_log_t;

static certificate_log_t g_log;

static void control(const char* verb, const char* args, char* out, size_t n) {
    REQUIRE(bdk_testing_command(t_runtime, S(verb), S(args), out, n) == BDK_OK);
}

/* A free loopback port: bound and released, for the server to take. */
static int free_port(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof a;
    if (s < 0 || bind(s, (struct sockaddr*)&a, sizeof a) != 0 ||
        getsockname(s, (struct sockaddr*)&a, &len) != 0)
        return 0;
    close(s);
    return ntohs(a.sin_port);
}

/* The server's and the client's keys and self-signed certificates, made with openssl. */
static int make_certificates(void) {
    const char* tmp = getenv("TMPDIR");
    snprintf(g_dir, sizeof g_dir, "%s/bdk-client-cert-%ld", tmp && *tmp ? tmp : "/tmp",
             (long)getpid());
    char command[2048];
    snprintf(command, sizeof command,
             "mkdir -p '%s' && cd '%s' && "
             "openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -days 2 "
             "-keyout server.key -out server.pem -subj /CN=127.0.0.1 "
             "-addext subjectAltName=IP:127.0.0.1 >/dev/null 2>&1 && "
             "openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -days 2 "
             "-keyout client.key -out client.pem -subj '/CN=BDK Test Client' >/dev/null 2>&1",
             g_dir, g_dir);
    return system(command) == 0;
}

/* openssl s_server, requiring a client certificate and echoing what it got (-www). */
static int start_server(void) {
    g_port = free_port();
    if (!g_port)
        return 0;
    char accept_at[64];
    snprintf(accept_at, sizeof accept_at, "127.0.0.1:%d", g_port);
    g_server = fork();
    if (g_server == 0) {
        if (chdir(g_dir) != 0)
            _exit(1);
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);
        execlp("openssl", "openssl", "s_server", "-accept", accept_at, "-cert", "server.pem",
               "-key", "server.key", "-Verify", "1", "-www", "-quiet", (char*)NULL);
        _exit(127);
    }
    for (int i = 0; i < 100; ++i) {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons((uint16_t)g_port);
        const int up = connect(s, (struct sockaddr*)&a, sizeof a) == 0;
        close(s);
        if (up)
            return 1;
        bdk_testing_run_for(0.05);
    }
    return 0;
}

static void stop_server(void) {
    if (g_server > 0) {
        kill(g_server, SIGKILL);
        waitpid(g_server, NULL, 0);
        g_server = 0;
    }
    char command[512];
    snprintf(command, sizeof command, "rm -rf '%s'", g_dir);
    (void)system(command);
}

/* A file's text as a JSON string's contents (newlines escaped), in `out`. */
static int json_text_of(const char* name, char* out, size_t n) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", g_dir, name);
    FILE* f = fopen(path, "r");
    if (!f)
        return 0;
    size_t used = 0;
    int c;
    while ((c = fgetc(f)) != EOF && used + 3 < n) {
        if (c == '\n') {
            out[used++] = '\\';
            out[used++] = 'n';
        } else {
            out[used++] = (char)c;
        }
    }
    out[used] = '\0';
    fclose(f);
    return c == EOF;
}

static bdk_ui_response_t proceed(void* ctx, const bdk_ui_request_info_t* info) {
    (void)ctx;
    bdk_error_t err = 0;
    CHECK(bdk_ui_complete(info->request, S("proceed"), S("{}"),
                          bdk_harness_engine_gesture(g_page, 0), &err) == BDK_OK);
    return BDK_UI_HANDLED;
}

static bdk_ui_response_t choose(void* ctx, const bdk_ui_request_info_t* info) {
    certificate_log_t* log = (certificate_log_t*)ctx;
    ++log->calls;
    snprintf(log->payload, sizeof log->payload, "%.*s", (int)info->payload_json.length,
             info->payload_json.data ? info->payload_json.data : "");
    bdk_error_t err = 0;
    bdk_native_handle_t none;
    memset(&none, 0, sizeof none);
    const int accept = strcmp(log->completion, "accept") == 0;
    CHECK(bdk_ui_complete(info->request, S(log->completion),
                          S(accept ? "{\"certificate\":\"0\"}" : "{}"), none, &err) == BDK_OK);
    return BDK_UI_HANDLED;
}

/* Waits until the page's text contains `want`; prints it if not. */
static int wait_for_text(bdk_contents_t c, const char* want) {
    char script[256];
    snprintf(script, sizeof script,
             "document.title = document.body && document.body.innerText.includes('%s')"
             " ? 'found' : 'missing'",
             want);
    for (int i = 0; i < 200; ++i) {
        REQUIRE(bdk_testing_execute_script(c, S(script), 0) == BDK_OK);
        t_pump();
        bdk_testing_run_for(0.05);
        bdk_contents_state_t st;
        memset(&st, 0, sizeof st);
        st.struct_size = sizeof st;
        bdk_error_t err = 0;
        REQUIRE(bdk_contents_state(c, &st, &err) == BDK_OK);
        if (st.title.length == 5 && memcmp(st.title.data, "found", 5) == 0)
            return 1;
    }
    printf("    the page never showed \"%s\"\n", want);
    return 0;
}

/* Starts the runtime with the identity in memory, the server, and handlers. */
static bdk_contents_t start(const char* completion) {
    memset(&g_log, 0, sizeof g_log);
    g_log.completion = completion;
    REQUIRE(make_certificates());
    REQUIRE(start_server());
    t_start();
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_CERTIFICATE_ERRORS, proceed, NULL, NULL,
                               &err) == BDK_OK);
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_CLIENT_CERTIFICATES, choose, &g_log, NULL,
                               &err) == BDK_OK);
    t_freeze();
    static char certificate[4096], key[4096], args[9000];
    REQUIRE(json_text_of("client.pem", certificate, sizeof certificate));
    REQUIRE(json_text_of("client.key", key, sizeof key));
    snprintf(args, sizeof args, "{\"certificate\":\"%s\",\"key\":\"%s\"}", certificate, key);
    char out[128];
    control("clientcert.identity", args, out, sizeof out);
    REQUIRE(strstr(out, "\"set\":true") != NULL);
    g_page = t_new_contents(t_new_profile("Certificates"));
    bdk_harness_engine_show(g_page, 1);
    REQUIRE(bdk_contents_focus(g_page) == BDK_OK);
    t_pump();
    char url[96];
    snprintf(url, sizeof url, "https://127.0.0.1:%d/", g_port);
    REQUIRE(bdk_contents_load(g_page, S(url), &err) == BDK_OK);
    t_pump();
    return g_page;
}

static void finish(bdk_contents_t c) {
    control("clientcert.identity", "{}", NULL, 0);
    bdk_harness_engine_show(c, 0);
    for (int i = 0; i < 10; ++i)
        t_pump();
    stop_server();
}

/* ---- Scenarios ---- */

/* A server that asks for a certificate raises a client_certificates request naming the host and
   the identity; accepting it sends that certificate, which the server reports back. */
static void test_a_handler_sends_the_chosen_certificate(void) {
    bdk_contents_t c = start("accept");
    CHECK(wait_for_text(c, "CN=BDK Test Client"));
    CHECK(g_log.calls == 1);
    char host[64];
    snprintf(host, sizeof host, "\"host\":\"127.0.0.1:%d\"", g_port);
    CHECK(strstr(g_log.payload, host) != NULL);
    CHECK(strstr(g_log.payload, "\"subject\":\"BDK Test Client\"") != NULL);
    finish(c);
}

/* A deny goes on without a certificate, as Chrome's Cancel does: this server then refuses the
   connection, and the page shows Chromium's error page instead of the server's report. */
static void test_a_deny_sends_no_certificate(void) {
    bdk_contents_t c = start("deny");
    for (int i = 0; i < 100 && g_log.calls == 0; ++i) {
        t_pump();
        bdk_testing_run_for(0.05);
    }
    CHECK(g_log.calls == 1);
    for (int i = 0; i < 40; ++i) {
        t_pump();
        bdk_testing_run_for(0.05);
    }
    REQUIRE(bdk_testing_execute_script(
                c,
                S("document.title = document.body && document.body.innerText.includes("
                  "'BDK Test Client') ? 'sent' : 'not sent'"),
                0) == BDK_OK);
    for (int i = 0; i < 20; ++i)
        t_pump();
    bdk_contents_state_t st;
    memset(&st, 0, sizeof st);
    st.struct_size = sizeof st;
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_state(c, &st, &err) == BDK_OK);
    CHECK(st.title.length != 4 || memcmp(st.title.data, "sent", 4) != 0);
    finish(c);
}

const t_test_t t_engine_client_certificate_tests[] = {
    {"engine_client_certificates_a_handler_sends_the_chosen_certificate",
     test_a_handler_sends_the_chosen_certificate},
    {"engine_client_certificates_a_deny_sends_no_certificate", test_a_deny_sends_no_certificate},
    {NULL, NULL},
};
