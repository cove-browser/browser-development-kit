/* Robustness (T-REL-03): many pages opened, loaded and destroyed; the runtime started and shut down
   with pages still open; renderers crashed and recovered; memory that stays bounded. Written
   against the ABI and the harness only, so the same scenarios run on the stub (CI) and the engine.

   BDK_ROBUSTNESS_ITERATIONS sets the number of rounds (default 100, a second or so on the stub).
   The pre-release pass (`nx run core:test-release`) runs 50000, about three seconds. The memory
   bound is checked only from 1000 rounds, where a leak of a few kilobytes per page shows above the
   allocator's noise. */
#define _DEFAULT_SOURCE  /* glibc: getrusage and ru_maxrss */
#define _DARWIN_C_SOURCE /* macOS: ru_maxrss */
#include <stdlib.h>
#include <sys/resource.h>

#include "test_support.h"

#define URL_PAGE "https://robustness.example/page"
#define URL_OTHER "https://robustness.example/other"
#define URL_CRASH "chrome://crash" /* Chromium's debug URL that kills the page's renderer */

static int iterations(void) {
    const char* v = getenv("BDK_ROBUSTNESS_ITERATIONS");
    long n = v ? strtol(v, NULL, 10) : 0;
    return n > 0 && n < 10000000 ? (int)n : 100;
}

/* The process's peak resident size in KiB (getrusage reports bytes on macOS, KiB elsewhere). */
static long peak_rss_kib(void) {
    struct rusage u;
    if (getrusage(RUSAGE_SELF, &u) != 0)
        return 0;
#ifdef __APPLE__
    return u.ru_maxrss / 1024;
#else
    return u.ru_maxrss;
#endif
}

typedef struct counts {
    int committed, finished, gone, closed, releases;
    char gone_detail[96];
} counts_t;

static void on_event(void* ctx, const bdk_contents_event_t* ev) {
    counts_t* c = (counts_t*)ctx;
    if (ev->kind == BDK_CONTENTS_EVENT_NAVIGATION_COMMITTED)
        ++c->committed;
    else if (ev->kind == BDK_CONTENTS_EVENT_LOAD_FINISHED)
        ++c->finished;
    else if (ev->kind == BDK_CONTENTS_EVENT_CLOSED)
        ++c->closed;
    else if (ev->kind == BDK_CONTENTS_EVENT_RENDERER_GONE) {
        ++c->gone;
        size_t n = ev->detail_json.length < sizeof c->gone_detail - 1 ? ev->detail_json.length
                                                                      : sizeof c->gone_detail - 1;
        memcpy(c->gone_detail, ev->detail_json.data, n);
        c->gone_detail[n] = 0;
    }
}

static void on_release(void* ctx) {
    ++((counts_t*)ctx)->releases;
}

/* Pumps until `*field` reaches `want`, for at most 200 pumps (the engine's loads take several). */
static int pump_until(const int* field, int want) {
    for (int i = 0; i < 200 && *field < want; ++i)
        t_pump();
    return *field >= want;
}

static bdk_profile_t start_with_profile(void) {
    t_start_frozen();
    REQUIRE(bdk_harness_script_navigation(t_runtime, URL_PAGE, 0, "Page") == BDK_OK);
    REQUIRE(bdk_harness_script_navigation(t_runtime, URL_OTHER, 0, "Other") == BDK_OK);
    return t_new_profile("robustness");
}

/* One page's life: create, subscribe, load, close, destroy. Returns 1 when every step behaved. */
static int one_page(bdk_profile_t profile, const char* url) {
    bdk_contents_t c = t_new_contents(profile);
    counts_t n;
    memset(&n, 0, sizeof n);
    if (bdk_contents_subscribe(c, on_event, &n, on_release) == 0)
        return 0;
    bdk_error_t err = 0;
    if (bdk_contents_load(c, S(url), &err) != BDK_OK)
        return 0;
    if (!pump_until(&n.finished, 1))
        return 0;
    if (bdk_contents_close(c, &err) != BDK_OK || !pump_until(&n.closed, 1))
        return 0;
    if (bdk_contents_destroy(c) != BDK_OK || bdk_contents_destroy(c) != BDK_ERR_STALE_HANDLE)
        return 0;
    t_pump();
    return n.committed == 1 && n.releases == 1;
}

static void test_pages_open_load_and_close_repeatedly(void) {
    bdk_profile_t profile = start_with_profile();
    const int rounds = iterations();
    int failed = 0;
    for (int i = 0; i < rounds; ++i)
        failed += !one_page(profile, (i & 1) ? URL_OTHER : URL_PAGE);
    if (failed)
        printf("    %d of %d rounds misbehaved\n", failed, rounds);
    CHECK(failed == 0);
}

static void test_the_runtime_restarts_with_pages_still_open(void) {
    const int rounds = iterations() / 10 + 1;
    for (int i = 0; i < rounds; ++i) {
        bdk_profile_t profile = start_with_profile();
        counts_t n;
        memset(&n, 0, sizeof n);
        bdk_contents_t c = t_new_contents(profile);
        REQUIRE(bdk_contents_subscribe(c, on_event, &n, on_release) != 0);
        bdk_error_t err = 0;
        REQUIRE(bdk_contents_load(c, S(URL_PAGE), &err) == BDK_OK);
        (void)t_new_contents(profile); /* a second page that is never loaded */
        t_pump();
        t_capture_t cap;
        t_cap_init(&cap);
        REQUIRE(bdk_runtime_shutdown(t_runtime, t_on_result, &cap, t_on_release) != 0);
        REQUIRE(pump_until(&cap.results, 1));
        CHECK(!cap.has_error);
        t_runtime = 0;
        t_pump();
        CHECK(n.releases == 1); /* the open page's subscription is released by the shutdown */
        CHECK(bdk_contents_load(c, S(URL_PAGE), &err) == BDK_ERR_STALE_HANDLE);
    }
}

static void test_a_crashed_renderer_recovers_and_leaves_other_pages_alone(void) {
    bdk_profile_t profile = start_with_profile();
    counts_t other;
    memset(&other, 0, sizeof other);
    bdk_contents_t bystander = t_new_contents(profile);
    REQUIRE(bdk_contents_subscribe(bystander, on_event, &other, on_release) != 0);
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_load(bystander, S(URL_OTHER), &err) == BDK_OK);
    REQUIRE(pump_until(&other.finished, 1));

    counts_t n;
    memset(&n, 0, sizeof n);
    bdk_contents_t c = t_new_contents(profile);
    REQUIRE(bdk_contents_subscribe(c, on_event, &n, on_release) != 0);
    REQUIRE(bdk_contents_load(c, S(URL_PAGE), &err) == BDK_OK);
    REQUIRE(pump_until(&n.finished, 1));
    const int rounds = iterations() / 10 + 1;
    for (int i = 0; i < rounds; ++i) {
        REQUIRE(bdk_contents_load(c, S(URL_CRASH), &err) == BDK_OK);
        REQUIRE(pump_until(&n.gone, i + 1));
        CHECK(strstr(n.gone_detail, "\"reason\":\"crashed\"") != NULL);
        /* The application reloads the page, as architecture.md section 15 expects. */
        REQUIRE(bdk_contents_reload(c, 0, &err) == BDK_OK);
        REQUIRE(pump_until(&n.finished, i + 2));
    }
    CHECK(other.gone == 0);
    CHECK(other.closed == 0);
    REQUIRE(bdk_contents_reload(bystander, 0, &err) == BDK_OK);
    CHECK(pump_until(&other.finished, 2));
}

static void test_memory_stays_bounded_over_many_pages(void) {
    const int rounds = iterations();
    if (rounds < 1000) {
        printf("    memory bound not checked below 1000 rounds (BDK_ROBUSTNESS_ITERATIONS=%d)\n",
               rounds);
        return;
    }
    bdk_profile_t profile = start_with_profile();
    for (int i = 0; i < 200; ++i) /* warm up the allocator and the runtime's tables */
        REQUIRE(one_page(profile, URL_PAGE));
    const long before = peak_rss_kib();
    for (int i = 0; i < rounds; ++i)
        REQUIRE(one_page(profile, (i & 1) ? URL_OTHER : URL_PAGE));
    const long grown = peak_rss_kib() - before;
    /* 16 MiB over the run: about 330 bytes per page at 50000 rounds. A leak of 1 KiB per page fails
     * it. */
    printf("    peak resident size grew %ld KiB over %d pages\n", grown, rounds);
    CHECK(grown < 16 * 1024);
}

const t_test_t t_robustness_tests[] = {
    {"robustness.pages_open_load_and_close", test_pages_open_load_and_close_repeatedly},
    {"robustness.restart_with_pages_open", test_the_runtime_restarts_with_pages_still_open},
    {"robustness.renderer_crash_recovery",
     test_a_crashed_renderer_recovers_and_leaves_other_pages_alone},
    {"robustness.memory_bounded", test_memory_stays_bounded_over_many_pages},
    {NULL, NULL},
};
