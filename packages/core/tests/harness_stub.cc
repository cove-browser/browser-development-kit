/* Harness for the stub runtime. */
#include <cstring>
#include <thread>

#include "bdk_stub.h"
#include "harness.h"

extern "C" {

size_t bdk_harness_pump(void) {
    return bdk_stub_pump();
}

void bdk_harness_run_on_other_thread(void (*fn)(void*), void* ctx) {
    std::thread t([fn, ctx] { fn(ctx); });
    t.join();
}

bdk_status_t bdk_harness_script_navigation(bdk_runtime_t rt, const char* url, int32_t net_error,
                                           const char* title) {
    bdk_stub_navigation_script_t s;
    memset(&s, 0, sizeof s);
    s.struct_size = sizeof s;
    s.net_error = net_error;
    s.title.data = title;
    s.title.length = title ? strlen(title) : 0;
    bdk_string_t u = {url, strlen(url)};
    return bdk_stub_script_navigation(rt, u, &s);
}

bdk_status_t bdk_harness_raise_ui_request(bdk_runtime_t rt, uint32_t surface, bdk_contents_t c,
                                          const char* payload) {
    bdk_string_t p = {payload, strlen(payload)};
    return bdk_stub_raise_ui_request(rt, surface, c, p);
}

bdk_status_t bdk_harness_withdraw_ui_requests(bdk_runtime_t rt) {
    return bdk_stub_withdraw_ui_requests(rt);
}

bdk_status_t bdk_harness_last_ui_outcome(bdk_runtime_t rt, bdk_harness_ui_outcome_t* out) {
    bdk_stub_ui_outcome_t o;
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    bdk_status_t s = bdk_stub_last_ui_outcome(rt, &o);
    if (s != BDK_OK)
        return s;
    memset(out, 0, sizeof *out);
    out->kind = o.kind;
    memcpy(out->completion, o.completion, sizeof out->completion);
    memcpy(out->arguments_json, o.arguments_json, sizeof out->arguments_json);
    return BDK_OK;
}

bdk_status_t bdk_harness_simulate_key(bdk_contents_t c, uint8_t* out_reserved) {
    bdk_native_handle_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = BDK_NATIVE_NSEVENT;
    ev.id = 1;
    return bdk_stub_simulate_key(c, ev, out_reserved);
}

} /* extern "C" */
