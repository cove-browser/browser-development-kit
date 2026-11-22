/* What the provider tests need from the runtime under test beyond tests/harness.h: pages with text,
   a block list for the default reputation provider, and a page's geolocation watch. The stub has
   them as test hooks (stub/bdk_stub_providers.h); a runtime without them answers 0 from
   bdk_harness_provider_hooks and those tests are skipped. The engine tests the same behaviour on
   real pages in tests_engine_providers.c. Plain C. */
#ifndef BDK_TEST_HARNESS_PROVIDERS_H_
#define BDK_TEST_HARNESS_PROVIDERS_H_

#include "bdk/bdk.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 1 if the runtime has these hooks, 0 if not. */
int bdk_harness_provider_hooks(void);

/* The page finished loading with these text segments (a JSON array of strings) in `language`
   (NULL or "": not declared). */
bdk_status_t bdk_harness_page_text_set(bdk_contents_t, const char* language,
                                       const char* texts_json);
/* The page's text segments now, as a JSON array, into out (at most cap bytes, terminated). */
bdk_status_t bdk_harness_page_text(bdk_contents_t, char* out, size_t cap);

/* Loads a block list for the default reputation provider, as a checked download would. */
bdk_status_t bdk_harness_reputation_list_load(bdk_runtime_t, const char* list_json, bdk_error_t*);

/* The page watches its position, reads the last report (empty when none), stops watching. */
bdk_status_t bdk_harness_geolocation_watch(bdk_contents_t, int high_accuracy);
bdk_status_t bdk_harness_geolocation_last(bdk_contents_t, char* out, size_t cap);
bdk_status_t bdk_harness_geolocation_stop(bdk_contents_t);

#ifdef __cplusplus
}
#endif

#endif /* BDK_TEST_HARNESS_PROVIDERS_H_ */
