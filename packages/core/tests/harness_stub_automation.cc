/* The automation part of the harness for the stub runtime (harness_automation.h). */
#include <cstring>

#include "bdk_stub_automation.h"
#include "harness_automation.h"

namespace {

struct Thunk {
    bdk_harness_script_cb cb;
    void* ctx;
};

void forward(void* ctx, uint64_t task, bdk_string_t frame) {
    auto* t = static_cast<Thunk*>(ctx);
    t->cb(t->ctx, task, frame.data, frame.length);
}

} // namespace

extern "C" {

int bdk_harness_automation_hooks(void) {
    return 1;
}

bdk_status_t bdk_harness_script_define(bdk_runtime_t rt, const char* name, bdk_harness_script_cb cb,
                                       void* ctx) {
    static Thunk thunks[16];
    static unsigned next;
    Thunk* t = &thunks[next++ % 16];
    *t = Thunk{cb, ctx};
    return bdk_stub_script_define(rt, bdk_string_t{name, strlen(name)}, forward, t);
}

bdk_status_t bdk_harness_script_send(uint64_t task, const char* frame) {
    return bdk_stub_script_send(task, bdk_string_t{frame, strlen(frame)});
}

unsigned bdk_harness_script_budget(uint64_t task) {
    return bdk_stub_script_budget(task);
}

bdk_status_t bdk_harness_script_exhaust_budget(uint64_t task) {
    return bdk_stub_script_exhaust_budget(task);
}

bdk_status_t bdk_harness_automation_host_crash(bdk_runtime_t rt) {
    return bdk_stub_automation_host_crash(rt);
}

bdk_status_t bdk_harness_endpoint_limits(bdk_runtime_t rt, size_t soft_bytes, size_t hard_bytes) {
    return bdk_stub_endpoint_limits(rt, soft_bytes, hard_bytes);
}

unsigned bdk_harness_automation_count(bdk_runtime_t rt, unsigned which) {
    return bdk_stub_automation_count(rt, which);
}

} /* extern "C" */
