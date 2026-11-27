/* A loopback HTTP server for the engine scenarios (tests_engine_navigation_policy.c,
   tests_engine_web_apps.c). It runs in a child process, so the engine fetches from it as from any
   server: the engine's test interceptor serves navigations only and leaves loopback to the
   network. 127.0.0.1 is a secure context. Built only into the engine conformance app. */
#ifndef BDK_TEST_ENGINE_HTTP_SERVER_H_
#define BDK_TEST_ENGINE_HTTP_SERVER_H_

#include <stddef.h>

/* A path the server answers with `body`, as `content_type`, and with `headers` (header lines, each
   ending in CRLF; NULL for none). */
typedef struct t_http_route {
    const char* path;
    const char* content_type;
    const void* body;
    size_t length;
    const char* headers;
} t_http_route_t;

/* Starts the server with up to 8 routes (copied: they need not outlive the call). Other paths:
   GET /redirect/<path> answers 302 to /<path>, anything else a page titled with its path. Every
   request's path is logged. Returns 0 when there is no loopback socket. */
int t_http_start(const t_http_route_t* routes, int count);
void t_http_stop(void);
/* "http://127.0.0.1:<port>", and that followed by `path`. */
const char* t_http_origin(void);
const char* t_http_url(const char* path);
/* Whether the server was asked for exactly `path`. */
int t_http_requested(const char* path);
/* A 192 by 192 PNG icon for fixtures, decoded into `out`: its length, or 0 when it does not fit. */
size_t t_http_test_icon(unsigned char* out, size_t cap);

#endif /* BDK_TEST_ENGINE_HTTP_SERVER_H_ */
