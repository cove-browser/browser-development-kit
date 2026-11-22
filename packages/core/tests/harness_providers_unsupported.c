/* The provider part of the harness for a runtime without the stub's test hooks (the engine): the
   tests that need them are skipped. See harness_providers.h. */
#include "harness_providers.h"

int bdk_harness_provider_hooks(void) {
    return 0;
}

bdk_status_t bdk_harness_page_text_set(bdk_contents_t c, const char* language,
                                       const char* texts_json) {
    (void)c, (void)language, (void)texts_json;
    return BDK_ERR_INVALID_ARGUMENT;
}

bdk_status_t bdk_harness_page_text(bdk_contents_t c, char* out, size_t cap) {
    (void)c, (void)out, (void)cap;
    return BDK_ERR_INVALID_ARGUMENT;
}

bdk_status_t bdk_harness_reputation_list_load(bdk_runtime_t rt, const char* list_json,
                                              bdk_error_t* out_error) {
    (void)rt, (void)list_json, (void)out_error;
    return BDK_ERR_INVALID_ARGUMENT;
}

bdk_status_t bdk_harness_geolocation_watch(bdk_contents_t c, int high_accuracy) {
    (void)c, (void)high_accuracy;
    return BDK_ERR_INVALID_ARGUMENT;
}

bdk_status_t bdk_harness_geolocation_last(bdk_contents_t c, char* out, size_t cap) {
    (void)c, (void)out, (void)cap;
    return BDK_ERR_INVALID_ARGUMENT;
}

bdk_status_t bdk_harness_geolocation_stop(bdk_contents_t c) {
    (void)c;
    return BDK_ERR_INVALID_ARGUMENT;
}
