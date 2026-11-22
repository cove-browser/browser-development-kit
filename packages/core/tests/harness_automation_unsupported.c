/* harness_automation.h for a runtime with a real automation host: the scripted-host tests are
   skipped (bdk_harness_automation_hooks answers 0) and nothing else is called. */
#include "harness_automation.h"

int bdk_harness_automation_hooks(void) {
    return 0;
}

bdk_status_t bdk_harness_script_define(bdk_runtime_t rt, const char* name, bdk_harness_script_cb cb,
                                       void* ctx) {
    (void)rt;
    (void)name;
    (void)cb;
    (void)ctx;
    return BDK_ERR_INVALID_ARGUMENT;
}

bdk_status_t bdk_harness_script_send(uint64_t task, const char* frame_json) {
    (void)task;
    (void)frame_json;
    return BDK_ERR_INVALID_ARGUMENT;
}

bdk_status_t bdk_harness_automation_host_crash(bdk_runtime_t rt) {
    (void)rt;
    return BDK_ERR_INVALID_ARGUMENT;
}

bdk_status_t bdk_harness_endpoint_limits(bdk_runtime_t rt, size_t soft_bytes, size_t hard_bytes) {
    (void)rt;
    (void)soft_bytes;
    (void)hard_bytes;
    return BDK_ERR_INVALID_ARGUMENT;
}

unsigned bdk_harness_automation_count(bdk_runtime_t rt, unsigned which) {
    (void)rt;
    (void)which;
    return 0;
}
