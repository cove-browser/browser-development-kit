/* Test hooks of the stub runtime for providers. Not part of the ABI. The stub renders nothing, so a
   test gives a page its text here (as a load that finished would), and starts a geolocation watch
   for a page as its script would. The conformance suite reaches these through
   tests/harness_providers.h. */
#ifndef BDK_STUB_PROVIDERS_HOOKS_H_
#define BDK_STUB_PROVIDERS_HOOKS_H_

#include "bdk/bdk.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The page finished loading with these text segments (a JSON array of strings) in `language`
   (empty: not declared). The stub detects, offers or translates as the engine would after a load.
 */
BDK_EXPORT bdk_status_t bdk_stub_page_text_set(bdk_contents_t, bdk_string_t language,
                                               bdk_string_t texts_json);
/* The page's text segments now, as a JSON array; release the buffer. */
BDK_EXPORT bdk_status_t bdk_stub_page_text(bdk_contents_t, bdk_buffer_t* out);

/* Loads a block list for the default reputation provider, as a downloaded list whose signature was
   checked would be. Fails with provider.invalidList and the reason. */
BDK_EXPORT bdk_status_t bdk_stub_reputation_list_load(bdk_runtime_t, bdk_string_t list_json,
                                                      bdk_error_t*);

/* The page's script calls watchPosition (its permission is granted). */
BDK_EXPORT bdk_status_t bdk_stub_geolocation_watch(bdk_contents_t, uint8_t high_accuracy);
/* The last position or error the page received, as JSON (empty when none); release the buffer. */
BDK_EXPORT bdk_status_t bdk_stub_geolocation_last(bdk_contents_t, bdk_buffer_t* out);
/* The page clears its watch. */
BDK_EXPORT bdk_status_t bdk_stub_geolocation_stop(bdk_contents_t);

#ifdef __cplusplus
}
#endif

#endif /* BDK_STUB_PROVIDERS_HOOKS_H_ */
