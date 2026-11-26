/* Privacy (T-PRV-07, T-PRV-06): Chromium runs with the start-up settings of the audit list
   (chromium/bdk/engine/providers/README.md), and a scripted session contacts no Google-owned host.
   Built only into the engine conformance app (BDK_HARNESS_ENGINE_TESTS).

   The network audit reads the net log Chromium writes from its first request. It runs only when
   the app was started with that log, so that nothing before the test is missed:
     BDK_TEST_NETLOG=/tmp/bdk-audit.json BDK_ENGINE_SWITCHES="log-net-log=/tmp/bdk-audit.json" \
       <build>/BDKEngineConformance.app/Contents/MacOS/BDKEngineConformance \
       --run engine_privacy_a_scripted_session_contacts_no_google_host
   Without BDK_TEST_NETLOG it says so and passes nothing off as checked. */
#include <stdlib.h>

#include "bdk_testing.h"
#include "test_support.h"

static void control(const char* verb, const char* args, char* out, size_t n) {
    REQUIRE(bdk_testing_command(t_runtime, S(verb), S(args), out, n) == BDK_OK);
}

/* Google-owned registrable domains, as the audit list names them. A host is Google's when it is
   one of these or ends in "." followed by one. */
static const char* const kGoogleDomains[] = {
    "google.com",
    "googleapis.com",
    "gstatic.com",
    "gvt1.com",
    "gvt2.com",
    "googleusercontent.com",
    "google-analytics.com",
    "doubleclick.net",
    "googlesyndication.com",
    "googletagmanager.com",
    "1e100.net",
    "youtube.com",
    "ggpht.com",
    "googlevideo.com",
    "chrome.com",
    "chromium.org",
    "android.com",
    "gmail.com",
    "google",
    NULL,
};

static int is_google(const char* host, size_t n) {
    for (int i = 0; kGoogleDomains[i]; ++i) {
        size_t d = strlen(kGoogleDomains[i]);
        if (n == d && strncmp(host, kGoogleDomains[i], n) == 0)
            return 1;
        if (n > d && host[n - d - 1] == '.' && strncmp(host + n - d, kGoogleDomains[i], d) == 0)
            return 1;
    }
    return 0;
}

static void test_engine_privacy_chromium_runs_with_the_audit_settings(void) {
    t_start_frozen();
    char out[512];
    control("providers.settings", "{}", out, sizeof out);
    CHECK(strstr(out, "\"componentUpdatesDisabled\":true") != NULL);
    CHECK(strstr(out, "\"syncDisabled\":true") != NULL);
    CHECK(strstr(out, "\"googleKey\":false") != NULL);
    /* The suite's first runtime uses the default providers, so the process runs with
       geolocation from the operating system only. */
    CHECK(strstr(out, "\"platformOnlyGeolocation\":true") != NULL);
    bdk_contents_t c = t_new_contents(t_new_profile("Privacy"));
    static const char* const kOff[] = {"safebrowsing.enhanced", "alternate_error_pages.enabled",
                                       "spellcheck.use_spelling_service",
                                       "url_keyed_anonymized_data_collection.enabled", NULL};
    for (int i = 0; kOff[i]; ++i) {
        char args[160];
        snprintf(args, sizeof args, "{\"contents\":\"%llu\",\"name\":\"%s\"}",
                 (unsigned long long)c, kOff[i]);
        control("pref.json", args, out, sizeof out);
        if (!strstr(out, "\"value\":false"))
            printf("    %s is %s\n", kOff[i], out);
        CHECK(strstr(out, "\"value\":false") != NULL);
    }
}

static void test_engine_privacy_a_scripted_session_contacts_no_google_host(void) {
    const char* path = getenv("BDK_TEST_NETLOG");
    if (!path || !*path) {
        printf("    skipped: run with BDK_TEST_NETLOG and --log-net-log (see "
               "tests_engine_privacy.c)\n");
        return;
    }
    t_start_frozen();
    bdk_profile_t p = t_new_profile("Audit");
    bdk_contents_t c = t_new_contents(p);
    const char* const pages[] = {"https://audit.example/", "https://audit.example/two",
                                 "https://other.example/", NULL};
    for (int i = 0; pages[i]; ++i) {
        char args[256];
        snprintf(args, sizeof args,
                 "{\"url\":\"%s\",\"contentType\":\"text/html\",\"headers\":\"\",\"body\":"
                 "\"<title>audit</title><p>text</p>\"}",
                 pages[i]);
        control("script_page", args, NULL, 0);
        bdk_error_t err = 0;
        REQUIRE(bdk_contents_load(c, S(pages[i]), &err) == BDK_OK);
        t_pump();
    }
    /* The component updater would ask after 60 s, network time at start-up; wait past both. */
    for (int i = 0; i < 90; ++i) {
        t_pump();
        bdk_testing_run_for(1.0);
    }
    char args[256], out[8192];
    snprintf(args, sizeof args, "{\"path\":\"%s\"}", path);
    control("network.hosts", args, out, sizeof out);
    printf("    hosts: %s\n", out);
    /* Every quoted host in the answer is checked. */
    int google = 0;
    for (const char* q = strchr(out, '['); q && (q = strchr(q, '"')) != NULL;) {
        const char* end = strchr(q + 1, '"');
        if (!end)
            break;
        if (is_google(q + 1, (size_t)(end - q - 1))) {
            printf("    Google host contacted: %.*s\n", (int)(end - q - 1), q + 1);
            ++google;
        }
        q = end + 1;
    }
    CHECK(google == 0);
}

/* [services.push] provider = "none", the suite's default (web-apps.md 7, T-PWA-05): pages have no
   Push API, so none can subscribe and Chromium's push client never checks in with Google's
   messaging hosts. The suite's first runtime starts Chromium, so the whole process runs so. */
static void test_engine_privacy_with_no_push_provider_pages_have_no_push_api(void) {
    t_start_frozen();
    bdk_contents_t c = t_new_contents(t_new_profile("Push"));
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_load(c, S("https://push.example/"), &err) == BDK_OK);
    t_pump();
    REQUIRE(bdk_testing_execute_script(
                c,
                S("document.title = [typeof PushManager, typeof PushSubscription,"
                  " 'pushManager' in ServiceWorkerRegistration.prototype].join(',')"),
                0) == BDK_OK);
    char title[128] = "";
    for (int i = 0; i < 200; ++i) {
        bdk_contents_state_t st;
        memset(&st, 0, sizeof st);
        st.struct_size = sizeof st;
        REQUIRE(bdk_contents_state(c, &st, &err) == BDK_OK);
        snprintf(title, sizeof title, "%.*s", (int)st.title.length,
                 st.title.data ? st.title.data : "");
        if (strstr(title, ","))
            break;
        t_pump();
    }
    CHECK(strcmp(title, "undefined,undefined,false") == 0);
}

const t_test_t t_engine_privacy_tests[] = {
    {"engine_privacy_with_no_push_provider_pages_have_no_push_api",
     test_engine_privacy_with_no_push_provider_pages_have_no_push_api},
    {"engine_privacy_chromium_runs_with_the_audit_settings",
     test_engine_privacy_chromium_runs_with_the_audit_settings},
    {"engine_privacy_a_scripted_session_contacts_no_google_host",
     test_engine_privacy_a_scripted_session_contacts_no_google_host},
    {NULL, NULL},
};
