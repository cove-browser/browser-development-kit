/* The broker part of the harness for a runtime without the test hooks (the engine, for now): the
   tests that need internal callers or the stub's services are skipped. See harness_broker.h. */
#include "harness_broker.h"

int bdk_harness_broker_hooks(void) {
    return 0;
}

bdk_status_t bdk_harness_caller_create(bdk_runtime_t rt, const bdk_harness_caller_spec_t* spec,
                                       bdk_harness_caller_t* out, bdk_error_t* out_error) {
    (void)rt, (void)spec, (void)out, (void)out_error;
    return BDK_ERR_INVALID_ARGUMENT;
}

bdk_call_t bdk_harness_invoke_as(bdk_runtime_t rt, bdk_harness_caller_t caller, const char* command,
                                 const char* args, const bdk_invoke_options_t* opts, int gesture,
                                 bdk_result_cb cb, bdk_progress_cb prog, void* ctx,
                                 bdk_release_cb rel) {
    (void)rt, (void)caller, (void)command, (void)args, (void)opts, (void)gesture, (void)cb,
        (void)prog, (void)ctx, (void)rel;
    return 0;
}

bdk_call_t bdk_harness_invoke_as_id(bdk_runtime_t rt, bdk_harness_caller_t caller, uint32_t id,
                                    const char* args, const bdk_invoke_options_t* opts, int gesture,
                                    bdk_result_cb cb, bdk_progress_cb prog, void* ctx,
                                    bdk_release_cb rel) {
    (void)rt, (void)caller, (void)id, (void)args, (void)opts, (void)gesture, (void)cb, (void)prog,
        (void)ctx, (void)rel;
    return 0;
}

bdk_status_t bdk_harness_caller_revoke(bdk_runtime_t rt, bdk_harness_caller_t caller) {
    (void)rt, (void)caller;
    return BDK_ERR_INVALID_ARGUMENT;
}

bdk_status_t bdk_harness_set_authentication(bdk_runtime_t rt, int mode) {
    (void)rt, (void)mode;
    return BDK_ERR_INVALID_ARGUMENT;
}

bdk_status_t bdk_harness_answer_authentication(bdk_runtime_t rt, int granted) {
    (void)rt, (void)granted;
    return BDK_ERR_INVALID_ARGUMENT;
}

bdk_status_t bdk_harness_hold_services(bdk_runtime_t rt, int hold) {
    (void)rt, (void)hold;
    return BDK_ERR_INVALID_ARGUMENT;
}

bdk_status_t bdk_harness_release_held_commands(bdk_runtime_t rt) {
    (void)rt;
    return BDK_ERR_INVALID_ARGUMENT;
}

unsigned bdk_harness_service_count(bdk_runtime_t rt, unsigned which) {
    (void)rt, (void)which;
    return 0;
}
