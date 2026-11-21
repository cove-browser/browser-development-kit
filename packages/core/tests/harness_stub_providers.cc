/* The provider part of the harness for the stub runtime, over its test hooks. */
#include <cstdio>
#include <cstring>

#include "bdk_stub_providers.h"
#include "harness_providers.h"

namespace {

bdk_string_t Str(const char* s) {
    return bdk_string_t{s ? s : "", s ? strlen(s) : 0};
}

bdk_status_t Copy(bdk_status_t s, bdk_buffer_t buffer, char* out, size_t cap) {
    if (s != BDK_OK)
        return s;
    bdk_bytes_t bytes = bdk_buffer_bytes(buffer);
    snprintf(out, cap, "%.*s", static_cast<int>(bytes.length),
             reinterpret_cast<const char*>(bytes.data));
    bdk_release(buffer);
    return BDK_OK;
}

} // namespace

extern "C" {

int bdk_harness_provider_hooks(void) {
    return 1;
}

bdk_status_t bdk_harness_page_text_set(bdk_contents_t c, const char* language,
                                       const char* texts_json) {
    return bdk_stub_page_text_set(c, Str(language), Str(texts_json));
}

bdk_status_t bdk_harness_page_text(bdk_contents_t c, char* out, size_t cap) {
    bdk_buffer_t buffer = 0;
    bdk_status_t s = bdk_stub_page_text(c, &buffer);
    return Copy(s, buffer, out, cap);
}

bdk_status_t bdk_harness_reputation_list_load(bdk_runtime_t rt, const char* list_json,
                                              bdk_error_t* out_error) {
    return bdk_stub_reputation_list_load(rt, Str(list_json), out_error);
}

bdk_status_t bdk_harness_geolocation_watch(bdk_contents_t c, int high_accuracy) {
    return bdk_stub_geolocation_watch(c, high_accuracy ? 1 : 0);
}

bdk_status_t bdk_harness_geolocation_last(bdk_contents_t c, char* out, size_t cap) {
    bdk_buffer_t buffer = 0;
    bdk_status_t s = bdk_stub_geolocation_last(c, &buffer);
    return Copy(s, buffer, out, cap);
}

bdk_status_t bdk_harness_geolocation_stop(bdk_contents_t c) {
    return bdk_stub_geolocation_stop(c);
}

} /* extern "C" */
