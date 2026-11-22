/* The external automation endpoint (T-AUT-04): off by default, a user-only Unix-domain socket,
   a hello with a token that the application accepts or refuses, and nothing on a network port.
   The test is the external tool: it connects with a real socket and speaks the wire protocol. Runs
   against every runtime (POSIX). */
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "test_support.h"

#define EXTERNAL_ON "\"automation\":{\"enabled\":false,\"external\":{\"enabled\":true}}"

typedef struct t_hello {
    int count;
    bdk_context_t last;
    char json[256];
    bdk_session_t accept_with; /* 0: refuse */
    int answer_now;
    int released;
} t_hello_t;

static void on_hello(void* ctx, bdk_context_t hello, bdk_string_t json) {
    t_hello_t* h = (t_hello_t*)ctx;
    ++h->count;
    h->last = hello;
    snprintf(h->json, sizeof h->json, "%.*s", (int)json.length, json.data);
    if (!h->answer_now)
        return;
    bdk_error_t err = 0;
    if (h->accept_with)
        CHECK(bdk_automation_hello_accept(hello, h->accept_with, &err) == BDK_OK);
    else
        CHECK(bdk_automation_hello_refuse(hello, S("unknown agent"), &err) == BDK_OK);
}

static void on_hello_release(void* ctx) {
    ++((t_hello_t*)ctx)->released;
}

static void open_endpoint(t_hello_t* h, char* path, size_t n) {
    bdk_buffer_t buf = 0;
    bdk_error_t err = 0;
    REQUIRE(bdk_automation_endpoint_open(t_runtime, S("{}"), on_hello, h, on_hello_release, &buf,
                                         &err) == BDK_OK);
    t_buffer_text(buf, path, n);
    CHECK(bdk_release(buf) == BDK_OK);
}

static int connect_to(const char* path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);
    REQUIRE(connect(fd, (struct sockaddr*)&addr, sizeof addr) == 0);
    return fd;
}

static void write_frame(int fd, const char* json) {
    size_t n = strlen(json);
    unsigned char prefix[4] = {(unsigned char)(n >> 24), (unsigned char)(n >> 16),
                               (unsigned char)(n >> 8), (unsigned char)n};
    REQUIRE(write(fd, prefix, 4) == 4);
    REQUIRE(write(fd, json, n) == (ssize_t)n);
}

/* Reads one frame while the runtime works. 1 with a frame, 0 at end of stream, -1 on timeout. */
static int read_frame(int fd, char* out, size_t cap) {
    unsigned char prefix[4];
    size_t have = 0, need = 4, length = 0;
    for (int spins = 0; spins < 500; ++spins) {
        t_pump();
        struct pollfd p = {fd, POLLIN, 0};
        if (poll(&p, 1, 10) <= 0)
            continue;
        char* target = need == 4 && length == 0 ? (char*)prefix + have : out + have;
        ssize_t r = read(fd, target, need - have);
        if (r == 0)
            return 0;
        if (r < 0)
            return errno == EINTR ? -1 : 0;
        have += (size_t)r;
        if (have < need)
            continue;
        if (length == 0 && need == 4) {
            length = ((size_t)prefix[0] << 24) | ((size_t)prefix[1] << 16) |
                     ((size_t)prefix[2] << 8) | prefix[3];
            REQUIRE(length < cap);
            have = 0;
            need = length;
            if (length == 0) {
                out[0] = '\0';
                return 1;
            }
            continue;
        }
        out[length] = '\0';
        return 1;
    }
    return -1;
}

/* No socket of this process listens on anything but a Unix-domain address. */
static int listens_on_network(void) {
    for (int fd = 0; fd < 1024; ++fd) {
        int listening = 0;
        socklen_t len = sizeof listening;
        if (getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &listening, &len) != 0 || !listening)
            continue;
        struct sockaddr_storage addr;
        socklen_t alen = sizeof addr;
        if (getsockname(fd, (struct sockaddr*)&addr, &alen) == 0 && addr.ss_family != AF_UNIX)
            return 1;
    }
    return 0;
}

static void the_endpoint_is_off_by_default(void) {
    t_start_frozen();
    static t_hello_t h;
    memset(&h, 0, sizeof h);
    bdk_buffer_t buf = 0;
    bdk_error_t err = 0;
    CHECK(bdk_automation_endpoint_open(t_runtime, S("{}"), on_hello, &h, on_hello_release, &buf,
                                       &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "automation", "disabled"));
    CHECK(buf == 0);
    t_pump();
    CHECK(h.released == 0); /* refused: the caller keeps its context */
}

static void the_socket_is_the_users_alone(void) {
    t_config_extra = EXTERNAL_ON;
    t_start_frozen();
    static t_hello_t h;
    memset(&h, 0, sizeof h);
    char path[512];
    open_endpoint(&h, path, sizeof path);
    struct stat st;
    REQUIRE(stat(path, &st) == 0);
    CHECK(S_ISSOCK(st.st_mode));
    CHECK((st.st_mode & 0777) == 0600);
    char dir[512];
    snprintf(dir, sizeof dir, "%s", path);
    *strrchr(dir, '/') = '\0';
    REQUIRE(stat(dir, &st) == 0);
    CHECK((st.st_mode & 0777) == 0700);
    CHECK(st.st_uid == geteuid());
    CHECK(!listens_on_network());
    bdk_buffer_t again = 0;
    bdk_error_t err = 0;
    CHECK(bdk_automation_endpoint_open(t_runtime, S("{}"), on_hello, &h, on_hello_release, &again,
                                       &err) == BDK_ERR_FAILED);
    bdk_error_release(err);
    CHECK(bdk_automation_endpoint_close(t_runtime, &err) == BDK_OK);
    t_pump();
    CHECK(stat(path, &st) != 0);
    CHECK(h.released == 1);
}

static void a_wrong_token_is_refused(void) {
    t_config_extra = EXTERNAL_ON;
    t_start_frozen();
    static t_hello_t h;
    memset(&h, 0, sizeof h);
    h.answer_now = 1; /* refuse */
    char path[512];
    open_endpoint(&h, path, sizeof path);
    int fd = connect_to(path);
    write_frame(fd, "{\"type\":\"hello\",\"protocol\":1,\"token\":\"wrong\"}");
    char frame[1024];
    REQUIRE(read_frame(fd, frame, sizeof frame) == 1);
    CHECK(h.count == 1);
    CHECK(strstr(h.json, "\"token\":\"wrong\"") != NULL);
    CHECK(strstr(frame, "\"type\":\"closed\"") != NULL);
    CHECK(strstr(frame, "\"code\":\"denied\"") != NULL);
    CHECK(strstr(frame, "unknown agent") != NULL);
    CHECK(read_frame(fd, frame, sizeof frame) == 0); /* and the connection ends */
    bdk_error_t err = 0;
    CHECK(bdk_automation_hello_accept(h.last, 0, &err) == BDK_ERR_STALE_HANDLE);
    close(fd);
}

static void an_accepted_tool_calls_commands_as_its_session(void) {
    t_config_extra = EXTERNAL_ON;
    t_start_frozen();
    bdk_session_t s = 0;
    bdk_error_t err = 0;
    REQUIRE(bdk_session_create(t_runtime,
                               S("{\"name\":\"cli\",\"capabilities\":[\"runtime.read\"]}"), &s,
                               &err) == BDK_OK);
    static t_hello_t h;
    memset(&h, 0, sizeof h);
    h.answer_now = 1;
    h.accept_with = s;
    char path[512];
    open_endpoint(&h, path, sizeof path);
    int fd = connect_to(path);
    write_frame(fd, "{\"type\":\"hello\",\"protocol\":1,\"token\":\"good\",\"client\":{\"n\":1}}");
    char frame[2048];
    REQUIRE(read_frame(fd, frame, sizeof frame) == 1);
    CHECK(strstr(frame, "\"type\":\"welcome\",\"protocol\":1") != NULL);
    CHECK(strstr(frame, "\"name\":\"cli\"") != NULL);
    CHECK(strstr(h.json, "\"client\":{\"n\":1}") != NULL);
    write_frame(fd, "{\"type\":\"call\",\"id\":1,\"command\":\"runtime.caller\",\"args\":{}}");
    REQUIRE(read_frame(fd, frame, sizeof frame) == 1);
    CHECK(strstr(frame, "\"type\":\"result\",\"id\":1") != NULL);
    CHECK(strstr(frame, "\"kind\":\"session\"") != NULL);
    write_frame(fd, "{\"type\":\"call\",\"id\":2,\"command\":\"history.search\",\"args\":{}}");
    REQUIRE(read_frame(fd, frame, sizeof frame) == 1);
    CHECK(strstr(frame, "\"type\":\"error\",\"id\":2") != NULL);
    CHECK(strstr(frame, "\"code\":\"denied\"") != NULL);
    /* Revoking the session closes its connections. */
    CHECK(bdk_session_revoke(s, &err) == BDK_OK);
    REQUIRE(read_frame(fd, frame, sizeof frame) == 1);
    CHECK(strstr(frame, "\"type\":\"closed\"") != NULL);
    CHECK(strstr(frame, "\"code\":\"sessionRevoked\"") != NULL);
    CHECK(read_frame(fd, frame, sizeof frame) == 0);
    close(fd);
}

static void the_first_frame_must_be_a_hello(void) {
    t_config_extra = EXTERNAL_ON;
    t_start_frozen();
    static t_hello_t h;
    memset(&h, 0, sizeof h);
    char path[512];
    open_endpoint(&h, path, sizeof path);
    int fd = connect_to(path);
    write_frame(fd, "{\"type\":\"call\",\"id\":1,\"command\":\"runtime.caller\"}");
    char frame[1024];
    REQUIRE(read_frame(fd, frame, sizeof frame) == 1);
    CHECK(strstr(frame, "\"type\":\"closed\"") != NULL);
    CHECK(strstr(frame, "\"code\":\"invalidArguments\"") != NULL);
    CHECK(h.count == 0);
    close(fd);
    /* A hello the application has not answered yet holds the connection; frames wait. */
    fd = connect_to(path);
    write_frame(fd, "{\"type\":\"hello\",\"protocol\":2,\"token\":\"t\"}");
    REQUIRE(read_frame(fd, frame, sizeof frame) == 1);
    CHECK(strstr(frame, "\"code\":\"invalidArguments\"") != NULL); /* protocol 2 is unknown */
    close(fd);
}

const t_test_t t_endpoint_tests[] = {
    {"endpoint.the_endpoint_is_off_by_default", the_endpoint_is_off_by_default},
    {"endpoint.the_socket_is_the_users_alone", the_socket_is_the_users_alone},
    {"endpoint.a_wrong_token_is_refused", a_wrong_token_is_refused},
    {"endpoint.an_accepted_tool_calls_commands_as_its_session",
     an_accepted_tool_calls_commands_as_its_session},
    {"endpoint.the_first_frame_must_be_a_hello", the_first_frame_must_be_a_hello},
    {NULL, NULL},
};
