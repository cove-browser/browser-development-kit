/* bdk_stub_automation_server: the stub runtime with its external automation endpoint open, for
   tests of external tools written in other languages (packages/js/automation's client test). It
   is an application in the smallest form: it opens a profile and a page, creates one session
   with the capabilities given on the command line, accepts the token "good-token" and refuses
   every other, prints the socket's path on one line, and runs until its standard input closes.

   usage: bdk_stub_automation_server <capability>... */
#define _POSIX_C_SOURCE 200809L
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bdk/bdk.h"
#include "bdk_stub.h"

static bdk_session_t g_session;

static bdk_string_t text(const char* s) {
    bdk_string_t t = {s, strlen(s)};
    return t;
}

static void on_hello(void* ctx, bdk_context_t hello, bdk_string_t json) {
    (void)ctx;
    bdk_error_t err = 0;
    char hello_text[512];
    snprintf(hello_text, sizeof hello_text, "%.*s", (int)json.length, json.data);
    if (strstr(hello_text, "\"token\":\"good-token\"") != NULL)
        bdk_automation_hello_accept(hello, g_session, &err);
    else
        bdk_automation_hello_refuse(hello, text("unknown token"), &err);
}

static void pump(void) {
    while (bdk_stub_pump() != 0) {
    }
}

static void fail(const char* what) {
    fprintf(stderr, "bdk_stub_automation_server: %s\n", what);
    exit(1);
}

int main(int argc, char** argv) {
    bdk_start_config_t c;
    memset(&c, 0, sizeof c);
    c.struct_size = sizeof c;
    c.resolved_config_json =
        text("{\"automation\":{\"enabled\":true,\"scripts\":{},\"external\":{\"enabled\":true}}}");
    c.bundle_path = text("/nonexistent");
    c.host_abi_version = bdk_abi_version();
    c.host_abi_hash = bdk_abi_hash();
    bdk_runtime_t rt = 0;
    bdk_error_t err = 0;
    if (bdk_runtime_start(&c, &rt, &err) != BDK_OK)
        fail("the runtime did not start");
    if (bdk_runtime_freeze_registrations(rt, &err) != BDK_OK)
        fail("the registrations did not freeze");
    bdk_profile_t profile = 0;
    if (bdk_profile_open_private(rt, &profile, &err) != BDK_OK)
        fail("the private profile did not open");
    pump();

    char options[2048];
    size_t used = (size_t)snprintf(options, sizeof options,
                                   "{\"name\":\"external-test\",\"scope\":{\"contents\":\"all\"},"
                                   "\"capabilities\":[");
    for (int i = 1; i < argc && used < sizeof options - 64; ++i)
        used += (size_t)snprintf(options + used, sizeof options - used, "%s\"%s\"",
                                 i > 1 ? "," : "", argv[i]);
    snprintf(options + used, sizeof options - used, "]}");
    if (bdk_session_create(rt, text(options), &g_session, &err) != BDK_OK)
        fail("the session was refused");

    bdk_contents_options_t o;
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.profile = profile;
    o.initial_url = text("https://example.com/");
    bdk_contents_t page = 0;
    if (bdk_contents_create(rt, &o, &page, &err) != BDK_OK)
        fail("the page was not created");

    bdk_buffer_t path = 0;
    if (bdk_automation_endpoint_open(rt, text("{}"), on_hello, NULL, NULL, &path, &err) != BDK_OK)
        fail("the endpoint did not open");
    bdk_bytes_t p = bdk_buffer_bytes(path);
    printf("%.*s\n", (int)p.length, (const char*)p.data);
    fflush(stdout);
    bdk_release(path);

    for (;;) {
        pump();
        struct pollfd in = {0, POLLIN, 0};
        if (poll(&in, 1, 5) > 0) {
            char buf[64];
            if (read(0, buf, sizeof buf) <= 0)
                break;
        }
    }
    bdk_automation_endpoint_close(rt, &err);
    bdk_contents_destroy(page);
    return 0;
}
