/* Engine scenarios for FedCM (ui-requests.md 5.22, T-UIR-14): a site and an identity provider on
   the loopback server (the provider's well-known file, config, accounts, client metadata and
   assertion endpoint), and the identity requests of Chromium's sign-in. Built only into the
   engine conformance app (BDK_HARNESS_ENGINE_TESTS). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bdk_testing.h"
#include "engine_http_server.h"
#include "test_support.h"

void bdk_harness_engine_show(bdk_contents_t contents, int visible);
bdk_native_handle_t bdk_harness_engine_gesture(bdk_contents_t contents, int kind);

static const char kSite[] = "<html><head><title>site</title></head><body>site</body></html>";
static const char kWellKnown[] = "{\"provider_urls\":[\"/fedcm/config.json\"]}";
static const char kConfig[] =
    "{\"accounts_endpoint\":\"/fedcm/accounts\",\"id_assertion_endpoint\":\"/fedcm/assertion\","
    "\"client_metadata_endpoint\":\"/fedcm/client\",\"login_url\":\"/fedcm/login.html\"}";
static const char kAccounts[] =
    "{\"accounts\":[{\"id\":\"1\",\"name\":\"Ada Lovelace\",\"email\":\"ada@idp.test\","
    "\"given_name\":\"Ada\"}]}";
static const char kClient[] = "{}";
static const char kAssertion[] = "{\"token\":\"fedcm-ok\"}";
static const char kLogin[] = "<html><head><title>login</title></head><body>login</body></html>";

static int start_server(void) {
    const t_http_route_t routes[] = {
        {"/fedcm/site.html", "text/html", kSite, sizeof kSite - 1, NULL},
        {"/.well-known/web-identity", "application/json", kWellKnown, sizeof kWellKnown - 1, NULL},
        {"/fedcm/config.json", "application/json", kConfig, sizeof kConfig - 1, NULL},
        {"/fedcm/accounts", "application/json", kAccounts, sizeof kAccounts - 1, NULL},
        {"/fedcm/client", "application/json", kClient, sizeof kClient - 1, NULL},
        {"/fedcm/assertion", "application/json", kAssertion, sizeof kAssertion - 1, NULL},
        {"/fedcm/login.html", "text/html", kLogin, sizeof kLogin - 1, NULL},
    };
    return t_http_start(routes, 7);
}

typedef struct {
    int calls;
    char steps[256];
    char payload[4096]; /* the chooseAccount step's */
    const char* choose; /* "grant" signs in with account 1; "deny" dismisses; NULL: default */
} identity_log_t;

static identity_log_t g_log;
static bdk_contents_t g_page;

static void control(const char* verb, const char* args, char* out, size_t n) {
    REQUIRE(bdk_testing_command(t_runtime, S(verb), S(args), out, n) == BDK_OK);
}

static bdk_ui_response_t on_identity(void* ctx, const bdk_ui_request_info_t* info) {
    identity_log_t* log = (identity_log_t*)ctx;
    ++log->calls;
    char payload[4096];
    snprintf(payload, sizeof payload, "%.*s", (int)info->payload_json.length,
             info->payload_json.data ? info->payload_json.data : "");
    const char* step = strstr(payload, "\"step\":\"");
    if (step) {
        char name[32];
        snprintf(name, sizeof name, "%.*s", (int)strcspn(step + 8, "\""), step + 8);
        snprintf(log->steps + strlen(log->steps), sizeof log->steps - strlen(log->steps), "%s%s",
                 log->steps[0] ? "," : "", name);
    }
    if (!strstr(payload, "\"step\":\"chooseAccount\""))
        return BDK_UI_HANDLED; /* loading and verifying: left open, withdrawn by the next step */
    snprintf(log->payload, sizeof log->payload, "%s", payload);
    if (!log->choose)
        return BDK_UI_USE_DEFAULT;
    bdk_error_t err = 0;
    bdk_native_handle_t none;
    memset(&none, 0, sizeof none);
    const int grant = strcmp(log->choose, "grant") == 0;
    CHECK(bdk_ui_complete(info->request, S(log->choose), S(grant ? "{\"account\":\"1\"}" : "{}"),
                          grant ? bdk_harness_engine_gesture(g_page, 0) : none, &err) == BDK_OK);
    return BDK_UI_HANDLED;
}

/* Waits until the title starts with `prefix`; the title in `out`. */
static int wait_for_title(bdk_contents_t c, const char* prefix, char* out, size_t n) {
    for (int i = 0; i < 400; ++i) {
        bdk_contents_state_t st;
        memset(&st, 0, sizeof st);
        st.struct_size = sizeof st;
        bdk_error_t err = 0;
        REQUIRE(bdk_contents_state(c, &st, &err) == BDK_OK);
        snprintf(out, n, "%.*s", (int)st.title.length, st.title.data ? st.title.data : "");
        if (strncmp(out, prefix, strlen(prefix)) == 0)
            return 1;
        t_pump();
        bdk_testing_run_for(0.05);
    }
    printf("    title is \"%s\", not \"%s...\" (steps: %s)\n", out, prefix, g_log.steps);
    return 0;
}

static bdk_contents_t start(const char* extra, const char* choose, int handler) {
    memset(&g_log, 0, sizeof g_log);
    g_log.choose = choose;
    t_config_extra = extra;
    t_start();
    t_config_extra = NULL;
    control("present.clear", "{}", NULL, 0);
    bdk_error_t err = 0;
    if (handler)
        REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_IDENTITY, on_identity, &g_log, NULL,
                                   &err) == BDK_OK);
    t_freeze();
    REQUIRE(start_server());
    g_page = t_new_contents(t_new_profile("Identity"));
    bdk_harness_engine_show(g_page, 1);
    REQUIRE(bdk_contents_focus(g_page) == BDK_OK);
    t_pump();
    REQUIRE(bdk_contents_load(g_page, S(t_http_url("/fedcm/site.html")), &err) == BDK_OK);
    char title[128];
    REQUIRE(wait_for_title(g_page, "site", title, sizeof title));
    return g_page;
}

static void finish(bdk_contents_t c) {
    control("present.clear", "{}", NULL, 0);
    bdk_harness_engine_show(c, 0);
    for (int i = 0; i < 10; ++i)
        t_pump();
    t_http_stop();
}

static void sign_in(bdk_contents_t c) {
    REQUIRE(
        bdk_testing_execute_script(c,
                                   S("navigator.credentials.get({identity: {providers: [{configURL:"
                                     " location.origin + '/fedcm/config.json', clientId: 'bdk'}]}})"
                                     ".then(c => document.title = 'token ' + c.token,"
                                     " e => document.title = 'error ' + e.name)"),
                                   1) == BDK_OK);
    t_pump();
}

/* ---- Scenarios ---- */

/* The chooseAccount request names the site, the provider and its account; a grant with the
   gesture signs in, and the site gets the provider's token. */
static void test_a_handler_signs_in_with_an_account(void) {
    bdk_contents_t c = start(NULL, "grant", 1);
    sign_in(c);
    char title[128];
    CHECK(wait_for_title(c, "token fedcm-ok", title, sizeof title));
    char origin[96];
    snprintf(origin, sizeof origin, "\"origin\":\"%s\"", t_http_origin());
    CHECK(strstr(g_log.payload, origin) != NULL);
    CHECK(strstr(g_log.payload, "\"email\":\"ada@idp.test\"") != NULL);
    CHECK(strstr(g_log.payload, "/fedcm/config.json") != NULL);
    CHECK(t_http_requested("/fedcm/assertion"));
    printf("    steps: %s\n", g_log.steps);
    finish(c);
}

/* A deny dismisses: the site gets no token. */
static void test_a_deny_signs_in_with_nothing(void) {
    bdk_contents_t c = start(NULL, "deny", 1);
    sign_in(c);
    char title[128];
    CHECK(wait_for_title(c, "error", title, sizeof title));
    CHECK(!t_http_requested("/fedcm/assertion"));
    finish(c);
}

/* With no handler BDK's sheet asks; "Continue as" signs in. */
static void test_the_default_sheet_signs_in(void) {
    bdk_contents_t c = start(NULL, NULL, 0);
    control("present.script", "{\"surface\":\"identity\",\"answer\":{\"answer\":\"1\"}}", NULL, 0);
    sign_in(c);
    char title[128];
    CHECK(wait_for_title(c, "token fedcm-ok", title, sizeof title));
    finish(c);
}

#define DISABLED_LAUNCH "engine_launch_identity_disabled_pages_have_no_identity_credential"
#define DISABLED_CONFIG "\"surfaces\":{\"identity\":\"disabled\"}"

static int g_disabled_prepared;

/* Called by the harness before its first start: FedCM is turned on or off as Chromium starts, so
   this scenario runs alone (scripts/test-engine.sh). */
void bdk_engine_identity_launch_prepare(const char* name) {
    if (strcmp(name, DISABLED_LAUNCH) != 0)
        return;
    t_config_extra = DISABLED_CONFIG;
    g_disabled_prepared = 1;
}

/* identity = "disabled": pages have no IdentityCredential. */
static void test_disabled_pages_have_no_identity_credential(void) {
    if (!g_disabled_prepared) {
        printf("    nothing prepared: run alone (scripts/test-engine.sh does)\n");
        return;
    }
    bdk_contents_t c = start(DISABLED_CONFIG, NULL, 1);
    REQUIRE(bdk_testing_execute_script(c, S("document.title = typeof IdentityCredential"), 0) ==
            BDK_OK);
    char title[128];
    CHECK(wait_for_title(c, "undefined", title, sizeof title));
    finish(c);
}

const t_test_t t_engine_identity_tests[] = {
    {"engine_identity_a_handler_signs_in_with_an_account", test_a_handler_signs_in_with_an_account},
    {"engine_identity_a_deny_signs_in_with_nothing", test_a_deny_signs_in_with_nothing},
    {"engine_identity_the_default_sheet_signs_in", test_the_default_sheet_signs_in},
    {DISABLED_LAUNCH, test_disabled_pages_have_no_identity_credential},
    {NULL, NULL},
};
