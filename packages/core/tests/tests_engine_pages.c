/* Scenarios for M4 that only a real engine can run: internal pages calling the broker through
   Mojo (T-WUI-03, T-WUI-04), the page broker refused to other pages, branded links, the bundled
   pages' content security policy, the search service on Chromium (T-SVC-10), and the M3
   verification left open (T-UIR-07): HTTP authentication end to end, a certificate error that
   cannot be overridden, and the card autofill prompt. Built only into the engine conformance app
   (BDK_HARNESS_ENGINE_TESTS). The fixture pages are written by harness_engine.mm: each sets its
   title to "page:<route>" and exposes bdkInvoke(command, args), which calls the broker and puts
   "ok:<result>" or "err:<error>" in the title. */
#include <netinet/in.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "bdk_testing.h"
#include "test_support.h"

/* From harness_engine.mm: the event a person's click or key press leaves. */
bdk_native_handle_t bdk_harness_engine_gesture(bdk_contents_t contents, int kind);

static const char* kPageRoutes =
    "{\"browser\":{\"name\":\"BDK Conformance\",\"internal_scheme\":\"conf\"},"
    "\"routes\":{"
    "\"settings\":{\"mode\":\"bundled\",\"path\":\"Pages/Settings\",\"host\":\"bdk-settings\","
    "\"capabilities\":[\"settings.read\",\"settings.write\"]},"
    "\"agents\":{\"mode\":\"bundled\",\"path\":\"Pages/Agents\",\"host\":\"bdk-agents\","
    "\"capabilities\":[]}}}";

/* ---- helpers ---- */

static void title_of(bdk_contents_t c, char* out, size_t n) {
    bdk_contents_state_t st;
    memset(&st, 0, sizeof st);
    st.struct_size = sizeof st;
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_state(c, &st, &err) == BDK_OK);
    snprintf(out, n, "%.*s", (int)st.title.length, st.title.data ? st.title.data : "");
}

static void url_of(bdk_contents_t c, char* out, size_t n) {
    bdk_contents_state_t st;
    memset(&st, 0, sizeof st);
    st.struct_size = sizeof st;
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_state(c, &st, &err) == BDK_OK);
    snprintf(out, n, "%.*s", (int)st.url.length, st.url.data ? st.url.data : "");
}

/* Pumps until the title starts with `prefix`; the whole title goes to out. */
static int wait_for_title_prefix(bdk_contents_t c, const char* prefix, char* out, size_t n) {
    for (int i = 0; i < 80; ++i) {
        title_of(c, out, n);
        if (strncmp(out, prefix, strlen(prefix)) == 0)
            return 1;
        t_pump();
    }
    return 0;
}

static int wait_for_url(bdk_contents_t c, const char* want) {
    char url[256];
    for (int i = 0; i < 80; ++i) {
        url_of(c, url, sizeof url);
        if (strcmp(url, want) == 0)
            return 1;
        t_pump();
    }
    return 0;
}

static void run_script(bdk_contents_t c, const char* script, int gesture) {
    REQUIRE(bdk_testing_execute_script(c, S(script), (uint8_t)gesture) == BDK_OK);
    t_pump();
}

/* Opens a page on url and waits until it is ready ("page:<route>" for a fixture page). */
static bdk_contents_t open_page(bdk_profile_t p, const char* url, const char* ready) {
    bdk_contents_t c = t_new_contents(p);
    bdk_harness_attach_view(c);
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_load(c, S(url), &err) == BDK_OK);
    char title[256];
    REQUIRE(wait_for_title_prefix(c, ready, title, sizeof title));
    return c;
}

/* Calls the broker as the fixture page and waits for the answer in its title. */
static void page_invoke(bdk_contents_t c, const char* command, const char* args, char* out,
                        size_t n) {
    char script[512];
    snprintf(script, sizeof script, "document.title = 'calling'; bdkInvoke('%s', %s);", command,
             args);
    run_script(c, script, 0);
    for (int i = 0; i < 80; ++i) {
        title_of(c, out, n);
        if (strncmp(out, "ok:", 3) == 0 || strncmp(out, "err:", 4) == 0)
            return;
        t_pump();
    }
    REQUIRE(!"the page's broker call did not answer");
}

static void host_invoke(bdk_profile_t profile, const char* command, const char* args,
                        t_capture_t* cap) {
    bdk_invoke_options_t o;
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.profile = profile;
    t_cap_init(cap);
    REQUIRE(bdk_invoke_named(t_runtime, S(command), S(args), &o, t_on_result, NULL, cap,
                             t_on_release) != 0);
    for (int i = 0; i < 80 && cap->results == 0; ++i)
        t_pump();
    REQUIRE(cap->results == 1);
}

/* ---- internal pages and the page broker (T-WUI-03, T-WUI-04) ---- */

static void test_engine_a_bundled_page_calls_the_broker_as_its_route(void) {
    t_start_with(kPageRoutes);
    t_freeze();
    bdk_profile_t p = t_new_profile("Pages");
    bdk_contents_t c = open_page(p, "conf://settings", "page:settings");
    char url[128];
    url_of(c, url, sizeof url);
    CHECK(strcmp(url, "conf://settings/") == 0);

    char answer[512];
    page_invoke(c, "runtime.caller", "{}", answer, sizeof answer);
    CHECK(strncmp(answer, "ok:", 3) == 0);
    CHECK(strstr(answer, "\"name\":\"settings\"") != NULL);
    CHECK(strstr(answer, "\"internalScheme\":\"conf\"") != NULL);
    CHECK(strstr(answer, "settings.write") != NULL);

    page_invoke(c, "history.search", "{}", answer, sizeof answer);
    CHECK(strncmp(answer, "err:", 4) == 0 && strstr(answer, "denied") != NULL);

    /* What the page writes is what Swift reads: there is one settings store. */
    page_invoke(c, "settings.set", "{key:'privacy.doNotTrack',value:true}", answer, sizeof answer);
    CHECK(strncmp(answer, "ok:", 3) == 0);
    t_capture_t cap;
    host_invoke(p, "settings.get", "{\"key\":\"privacy.doNotTrack\"}", &cap);
    CHECK(!cap.has_error && strstr(cap.json, "true") != NULL);
}

static void test_engine_a_page_with_no_grants_is_refused_everything_but_who_it_is(void) {
    t_start_with(kPageRoutes);
    t_freeze();
    bdk_contents_t c = open_page(t_new_profile("No grants"), "conf://agents", "page:agents");
    char answer[512];
    page_invoke(c, "runtime.caller", "{}", answer, sizeof answer);
    CHECK(strstr(answer, "\"name\":\"agents\"") != NULL);
    page_invoke(c, "settings.get", "{key:'privacy.doNotTrack'}", answer, sizeof answer);
    CHECK(strncmp(answer, "err:", 4) == 0 && strstr(answer, "denied") != NULL);
}

static void test_engine_another_page_asking_for_the_broker_loses_its_renderer(void) {
    t_start_with(kPageRoutes);
    t_freeze();
    bdk_profile_t p = t_new_profile("Other WebUI");
    bdk_contents_t c = t_new_contents(p);
    bdk_harness_attach_view(c);
    t_events_t ev;
    t_ev_init(&ev);
    REQUIRE(bdk_contents_subscribe(c, t_on_contents_event, &ev, t_on_event_release) != 0);
    bdk_error_t err = 0;
    /* Chromium's history page is a WebUI with Mojo, but not BundledPageUI. */
    REQUIRE(bdk_contents_load(c, S("conf://history"), &err) == BDK_OK);
    REQUIRE(wait_for_url(c, "conf://history/"));
    run_script(c, "Mojo.bindInterface('bdk.mojom.PageBroker', Mojo.createMessagePipe().handle0);",
               0);
    int gone = 0;
    for (int i = 0; i < 80 && !gone; ++i) {
        t_pump();
        for (int k = 0; k < ev.count; ++k)
            gone |= ev.kind[k] == BDK_CONTENTS_EVENT_RENDERER_GONE;
    }
    CHECK(gone);
}

static void test_engine_links_to_branded_urls_follow_who_asks(void) {
    t_start_with(kPageRoutes);
    t_freeze();
    bdk_profile_t p = t_new_profile("Links");
    REQUIRE(bdk_testing_command(t_runtime, S("script_page"),
                                S("{\"url\":\"https://web.example/\",\"body\":\"<title>web</title>"
                                  "\",\"contentType\":\"text/html\"}"),
                                NULL, 0) == BDK_OK);
    bdk_contents_t web = open_page(p, "https://web.example/", "web");
    run_script(web, "location.href = 'conf://settings';", 1);
    for (int i = 0; i < 20; ++i)
        t_pump();
    char url[128];
    url_of(web, url, sizeof url);
    CHECK(strcmp(url, "https://web.example/") == 0); /* dropped */

    bdk_contents_t internal = open_page(p, "conf://settings", "page:settings");
    run_script(internal, "location.href = 'conf://history';", 1);
    CHECK(wait_for_url(internal, "conf://history/"));
}

static void test_engine_a_bundled_page_runs_under_the_content_security_policy(void) {
    t_start_with(kPageRoutes);
    t_freeze();
    bdk_contents_t c = open_page(t_new_profile("Policy"), "conf://settings", "page:settings");
    char title[256];
    run_script(c,
               "try { eval('1'); document.title = 'eval ran'; }"
               " catch (e) { document.title = 'eval blocked'; }",
               0);
    REQUIRE(wait_for_title_prefix(c, "eval", title, sizeof title));
    CHECK(strcmp(title, "eval blocked") == 0);
    run_script(c,
               "try { document.body.innerHTML = '<b>x</b>'; document.title = 'tt off'; }"
               " catch (e) { document.title = 'tt on'; }",
               0);
    REQUIRE(wait_for_title_prefix(c, "tt", title, sizeof title));
    CHECK(strcmp(title, "tt on") == 0);
    /* The SDK path is served from the bundle; a missing module is not found. */
    run_script(c,
               "import('/@bdk/internal/nothing.js').then(() => document.title = 'sdk found',"
               " () => document.title = 'sdk missing');",
               0);
    REQUIRE(wait_for_title_prefix(c, "sdk", title, sizeof title));
    CHECK(strcmp(title, "sdk missing") == 0);
}

/* ---- search (T-SVC-10) ---- */

static void test_engine_search_engines_classify_and_suggest(void) {
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Search");
    t_capture_t cap;
    host_invoke(p, "search.addEngine",
                "{\"name\":\"Example\",\"keyword\":\"ex\","
                "\"searchUrl\":\"https://search.example/?q={searchTerms}\"}",
                &cap);
    REQUIRE(!cap.has_error);
    char id[80];
    REQUIRE(t_json_string(cap.json, "id", id, sizeof id));
    char args[160];
    snprintf(args, sizeof args, "{\"id\":\"%s\"}", id);
    host_invoke(p, "search.setDefault", args, &cap);
    CHECK(!cap.has_error);
    /* Chromium's prepopulated engines come first and fill the capture's 512 bytes, so the listing
       is only checked to succeed; classify below shows the added engine is the default. */
    host_invoke(p, "search.engines", "{}", &cap);
    CHECK(!cap.has_error);

    host_invoke(p, "search.classify", "{\"text\":\"cats and dogs\"}", &cap);
    CHECK(strstr(cap.json, "\"kind\":\"search\"") != NULL);
    CHECK(strstr(cap.json, "search.example") != NULL && strstr(cap.json, "cats") != NULL);
    host_invoke(p, "search.classify", "{\"text\":\"example.com\"}", &cap);
    CHECK(strstr(cap.json, "\"kind\":\"url\"") != NULL);

    host_invoke(p, "history.add", "{\"url\":\"https://www.wikipedia.org/\",\"title\":\"Wiki\"}",
                &cap);
    host_invoke(p, "search.suggestions", "{\"text\":\"wikip\"}", &cap);
    CHECK(!cap.has_error && strstr(cap.json, "wikipedia.org") != NULL);
}

/* ---- M3 verification left open (T-UIR-07) ---- */

/* A loopback HTTP server, in a child process, for one purpose: Chromium's network stack asks for
   credentials only for a real 401, which the engine's interceptor cannot produce. Answers 401
   with a Basic challenge until a request carries an Authorization header, then a page titled
   "authed". */
static pid_t g_http_pid;
static int g_http_port;

static void stop_http_server(void) {
    if (g_http_pid) {
        kill(g_http_pid, SIGTERM);
        waitpid(g_http_pid, NULL, 0);
        g_http_pid = 0;
    }
}

static void serve_auth(int listener) {
    for (;;) {
        int fd = accept(listener, NULL, NULL);
        if (fd < 0)
            continue;
        char request[4096];
        ssize_t n = read(fd, request, sizeof request - 1);
        request[n > 0 ? n : 0] = '\0';
        const char* reply =
            strstr(request, "\nAuthorization: Basic ")
                ? "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: 21\r\n"
                  "Connection: close\r\n\r\n<title>authed</title>"
                : "HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Basic realm=\"bdk\"\r\n"
                  "Content-Type: text/html\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        (void)!write(fd, reply, strlen(reply));
        close(fd);
    }
}

static int start_http_server(void) {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    socklen_t len = sizeof a;
    if (listener < 0 || bind(listener, (struct sockaddr*)&a, sizeof a) != 0 ||
        listen(listener, 8) != 0 || getsockname(listener, (struct sockaddr*)&a, &len) != 0)
        return 0;
    g_http_port = ntohs(a.sin_port);
    g_http_pid = fork();
    if (g_http_pid == 0) {
        serve_auth(listener);
        _exit(0);
    }
    close(listener);
    return g_http_pid > 0;
}

typedef struct auth_seen {
    int calls;
    char payload[512];
} auth_seen_t;

static bdk_ui_response_t answer_auth(void* ctx, const bdk_ui_request_info_t* info) {
    auth_seen_t* seen = (auth_seen_t*)ctx;
    ++seen->calls;
    snprintf(seen->payload, sizeof seen->payload, "%.*s", (int)info->payload_json.length,
             info->payload_json.data);
    bdk_error_t err = 0;
    (void)bdk_ui_complete(info->request, S("accept"),
                          S("{\"username\":\"ada\",\"password\":\"pw\"}"),
                          (bdk_native_handle_t){0, NULL, 0}, &err);
    return BDK_UI_HANDLED;
}

static void test_engine_http_authentication_end_to_end(void) {
    if (!start_http_server()) {
        printf("    (skipped: no loopback socket for the 401 server)\n");
        return;
    }
    auth_seen_t seen;
    memset(&seen, 0, sizeof seen);
    t_start();
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_HTTP_AUTH, answer_auth, &seen, NULL, &err) ==
            BDK_OK);
    t_freeze();
    bdk_contents_t c = t_new_contents(t_new_profile("Auth"));
    bdk_harness_attach_view(c);
    char url[64];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/", g_http_port);
    REQUIRE(bdk_contents_load(c, S(url), &err) == BDK_OK);
    char title[64];
    CHECK(wait_for_title_prefix(c, "authed", title, sizeof title));
    CHECK(seen.calls == 1);
    CHECK(strstr(seen.payload, "bdk") != NULL); /* the realm */
    stop_http_server();
}

/* The TLS server of tests_engine_prompts.c, on a name HSTS can apply to: Chromium applies no HSTS
   to *.localhost, so the harness maps hsts.bdk.test to the loopback address
   (BDK_ENGINE_HOST_RESOLVER_RULES). */
static pid_t g_tls_pid;
static int g_tls_port;

static int start_tls_server(void) {
    char dir[] = "/tmp/bdk-hsts-XXXXXX";
    if (!mkdtemp(dir))
        return 0;
    char cmd[512], key[128], cert[128], port[16];
    snprintf(key, sizeof key, "%s/k.pem", dir);
    snprintf(cert, sizeof cert, "%s/c.pem", dir);
    snprintf(cmd, sizeof cmd,
             "/usr/bin/openssl req -x509 -newkey rsa:2048 -nodes -keyout %s -out %s "
             "-subj /CN=hsts.bdk.test -days 2 >/dev/null 2>&1",
             key, cert);
    if (system(cmd) != 0)
        return 0;
    g_tls_port = 20000 + (int)((getpid() + 7) % 20000);
    snprintf(port, sizeof port, "%d", g_tls_port);
    g_tls_pid = fork();
    if (g_tls_pid == 0) {
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);
        execl("/usr/bin/openssl", "openssl", "s_server", "-accept", port, "-key", key, "-cert",
              cert, "-www", "-quiet", (char*)NULL);
        _exit(1);
    }
    usleep(500000);
    return g_tls_pid > 0;
}

typedef struct interstitial_seen {
    int calls;
    bdk_request_t request;
    char payload[1024];
} interstitial_seen_t;

static bdk_ui_response_t hold_interstitial(void* ctx, const bdk_ui_request_info_t* info) {
    interstitial_seen_t* seen = (interstitial_seen_t*)ctx;
    ++seen->calls;
    seen->request = info->request;
    snprintf(seen->payload, sizeof seen->payload, "%.*s", (int)info->payload_json.length,
             info->payload_json.data);
    return BDK_UI_HANDLED;
}

static void test_engine_a_non_overridable_certificate_error_cannot_proceed(void) {
    if (!start_tls_server()) {
        printf("    (skipped: no openssl to serve a bad certificate)\n");
        return;
    }
    interstitial_seen_t seen;
    memset(&seen, 0, sizeof seen);
    t_start();
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_CERTIFICATE_ERRORS, hold_interstitial, &seen,
                               NULL, &err) == BDK_OK);
    t_freeze();
    bdk_contents_t c = t_new_contents(t_new_profile("HSTS"));
    bdk_harness_attach_view(c);
    char args[128];
    snprintf(args, sizeof args, "{\"contents\":\"%llu\",\"host\":\"hsts.bdk.test\"}",
             (unsigned long long)c);
    REQUIRE(bdk_testing_command(t_runtime, S("hsts.add"), S(args), NULL, 0) == BDK_OK);
    REQUIRE(bdk_testing_command(t_runtime, S("network.passthrough"),
                                S("{\"host\":\"hsts.bdk.test\"}"), NULL, 0) == BDK_OK);
    t_pump();
    char url[64];
    snprintf(url, sizeof url, "https://hsts.bdk.test:%d/", g_tls_port);
    REQUIRE(bdk_contents_load(c, S(url), &err) == BDK_OK);
    for (int i = 0; i < 80 && seen.calls == 0; ++i)
        t_pump();
    REQUIRE(seen.calls == 1);
    CHECK(strstr(seen.payload, "\"isOverridable\":false") != NULL);
    err = 0;
    CHECK(bdk_ui_complete(seen.request, S("proceed"), S("{}"), bdk_harness_engine_gesture(c, 0),
                          &err) == BDK_ERR_FAILED);
    CHECK(t_error_is(err, "ui", "notOverridable"));
    bdk_ui_request_release(seen.request);
    kill(g_tls_pid, SIGTERM);
    waitpid(g_tls_pid, NULL, 0);
    g_tls_pid = 0;
}

static const char* kCardForm =
    "<title>card</title><form id=f action='https://shop.example/done' method=post>"
    "<input id=name name=cc-name autocomplete=cc-name>"
    "<input id=number name=cc-number autocomplete=cc-number>"
    "<input id=month name=cc-exp-month autocomplete=cc-exp-month>"
    "<input id=year name=cc-exp-year autocomplete=cc-exp-year>"
    "<input type=submit id=go></form>";

typedef struct autofill_seen {
    int calls;
    char payload[1024];
} autofill_seen_t;

static bdk_ui_response_t record_autofill(void* ctx, const bdk_ui_request_info_t* info) {
    autofill_seen_t* seen = (autofill_seen_t*)ctx;
    ++seen->calls;
    snprintf(seen->payload, sizeof seen->payload, "%.*s", (int)info->payload_json.length,
             info->payload_json.data);
    return BDK_UI_REJECT;
}

static void test_engine_a_submitted_card_raises_the_card_save_prompt(void) {
    autofill_seen_t seen;
    memset(&seen, 0, sizeof seen);
    t_start();
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_AUTOFILL, record_autofill, &seen, NULL,
                               &err) == BDK_OK);
    t_freeze();
    char args[1024];
    snprintf(args, sizeof args,
             "{\"url\":\"https://shop.example/pay\",\"contentType\":\"text/html\",\"body\":\"%s\"}",
             kCardForm);
    REQUIRE(bdk_testing_command(t_runtime, S("script_page"), S(args), NULL, 0) == BDK_OK);
    REQUIRE(bdk_testing_command(t_runtime, S("script_page"),
                                S("{\"url\":\"https://shop.example/done\",\"contentType\":"
                                  "\"text/html\",\"body\":\"<title>done</title>\"}"),
                                NULL, 0) == BDK_OK);
    bdk_contents_t c = open_page(t_new_profile("Card"), "https://shop.example/pay", "card");
    run_script(c,
               "name.value = 'Ada Lovelace'; number.value = '4111111111111111';"
               " month.value = '12'; year.value = '2031'; go.click();",
               1);
    char title[64];
    REQUIRE(wait_for_title_prefix(c, "done", title, sizeof title));
    for (int i = 0; i < 80 && seen.calls == 0; ++i)
        t_pump();
    CHECK(seen.calls == 1);
    CHECK(strstr(seen.payload, "\"kind\":\"card\"") != NULL);
}

const t_test_t t_engine_pages_tests[] = {
    {"engine.bundled_page_calls_the_broker_as_its_route",
     test_engine_a_bundled_page_calls_the_broker_as_its_route},
    {"engine.page_with_no_grants_is_refused",
     test_engine_a_page_with_no_grants_is_refused_everything_but_who_it_is},
    {"engine.other_page_asking_for_the_broker_loses_its_renderer",
     test_engine_another_page_asking_for_the_broker_loses_its_renderer},
    {"engine.branded_links_follow_who_asks", test_engine_links_to_branded_urls_follow_who_asks},
    {"engine.bundled_page_content_security_policy",
     test_engine_a_bundled_page_runs_under_the_content_security_policy},
    {"engine.search_engines_classify_and_suggest", test_engine_search_engines_classify_and_suggest},
    {"engine.http_authentication_end_to_end", test_engine_http_authentication_end_to_end},
    {"engine.non_overridable_certificate_error",
     test_engine_a_non_overridable_certificate_error_cannot_proceed},
    {"engine.card_save_prompt", test_engine_a_submitted_card_raises_the_card_save_prompt},
    {NULL, NULL},
};
