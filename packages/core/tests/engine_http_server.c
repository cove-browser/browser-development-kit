/* The loopback HTTP server of engine_http_server.h. */
#include "engine_http_server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_ROUTES 8

typedef struct {
    char path[256];
    char reply[4096];
    size_t length;
} route_t;

static pid_t g_server;
static int g_port;
static char g_log[512];
static char g_origin[64];
static route_t g_routes[MAX_ROUTES];
static int g_route_count;

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

/* Only system calls and string functions: it runs in a child forked from a threaded process. */
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
        const route_t* found = NULL;
        for (int i = 0; i < g_route_count; ++i) {
            if (strcmp(path, g_routes[i].path) == 0)
                found = &g_routes[i];
        }
        if (found) {
            (void)!write(fd, found->reply, found->length);
            close(fd);
            continue;
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

int t_http_start(const t_http_route_t* routes, int count) {
    g_route_count = 0;
    for (int i = 0; i < count && i < MAX_ROUTES; ++i) {
        route_t* r = &g_routes[g_route_count];
        snprintf(r->path, sizeof r->path, "%s", routes[i].path);
        int head = snprintf(r->reply, sizeof r->reply,
                            "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nCache-Control: no-store\r\n"
                            "%sConnection: close\r\nContent-Length: %zu\r\n\r\n",
                            routes[i].content_type, routes[i].headers ? routes[i].headers : "",
                            routes[i].length);
        if (head <= 0 || (size_t)head + routes[i].length > sizeof r->reply)
            return 0;
        memcpy(r->reply + head, routes[i].body, routes[i].length);
        r->length = (size_t)head + routes[i].length;
        ++g_route_count;
    }
    const char* tmp = getenv("TMPDIR");
    snprintf(g_log, sizeof g_log, "%s/bdk-engine-requests-%ld.log", tmp && *tmp ? tmp : "/tmp",
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
    snprintf(g_origin, sizeof g_origin, "http://127.0.0.1:%d", g_port);
    g_server = fork();
    if (g_server == 0) {
        /* Off the suite's output, which a pipe may be waiting to see closed. */
        int null = open("/dev/null", O_RDWR);
        dup2(null, 1);
        dup2(null, 2);
        serve(listener);
        _exit(0);
    }
    close(listener);
    return g_server > 0;
}

void t_http_stop(void) {
    if (g_server > 0) {
        kill(g_server, SIGKILL); /* not SIGTERM: the child has Chromium's handler for it */
        waitpid(g_server, NULL, 0);
        g_server = 0;
    }
    unlink(g_log);
}

const char* t_http_origin(void) {
    return g_origin;
}

const char* t_http_url(const char* path) {
    static char buf[4][256];
    static int next;
    char* out = buf[next++ % 4];
    snprintf(out, 256, "%s%s", g_origin, path);
    return out;
}

int t_http_requested(const char* path) {
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

/* A 192 by 192 PNG. */
static const char* kIcon =
    "iVBORw0KGgoAAAANSUhEUgAAAMAAAADACAIAAADdvvtQAAABZElEQVR42u3SQQ0AAAjEsPOEJzzhFguEd5MqWJbqgbdI"
    "gIEwEAbCQGAgDISBMBAYCANhIAwEBsJAGAgDgYEwEAbCQBgIDISBMBAGAgNhIAyEgcBAGAgDYSAwEAbCQBgIA4GBMBAG"
    "wkBgIAyEgTAQGAgDYSAMBAbCQBgIA4GBMBAGwkAYCAyEgTAQBgIDYSAMhIHAQBgIA2EgMBAGwkAYCAOBgTAQBsJAYCAM"
    "hIEwEBgIA2EgDAQGwkAYCANhIDAQBsJAGAgMhIEwEAYCA2EgDISBwEAYCANhIDAQBsJAGAgDgYEwEAbCQGAgDISBMBAY"
    "CANhIAwEBsJAGAgDYSAwEAbCQBgIDISBMBAGAgNhIAyEgcBAGAgDYSAMpAIGwkAYCAOBgTAQBsJAYCAMhIEwEBgIA2Eg"
    "DAQGwkAYCANhIDAQBsJAGAgMhIEwEAYCA2EgDISBwEAYCANhIAwEBsJAGAgDwdUCCpUqFQe2NBgAAAAASUVORK5CYII=";

static size_t decode_base64(const char* in, unsigned char* out, size_t cap) {
    static const char* abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t n = 0;
    unsigned value = 0;
    int bits = 0;
    for (const char* c = in; *c && *c != '='; ++c) {
        const char* at = strchr(abc, *c);
        if (!at)
            continue;
        value = (value << 6) | (unsigned)(at - abc);
        bits += 6;
        if (bits >= 8 && n < cap) {
            bits -= 8;
            out[n++] = (unsigned char)((value >> bits) & 0xff);
        }
    }
    return n;
}

size_t t_http_test_icon(unsigned char* out, size_t cap) {
    return decode_base64(kIcon, out, cap);
}
