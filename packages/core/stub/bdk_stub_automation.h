/* Test hooks of the stub runtime for automation. Not part of the ABI. The stub runs no JavaScript,
   so its automation host runs "scripts" the test defines as callbacks: the callback receives every
   frame BDK sends to the script (the first is `start`) and answers with bdk_stub_script_send, in
   the wire protocol of packages/core/docs/conventions.md ("Automation"). */
#ifndef BDK_STUB_AUTOMATION_H_
#define BDK_STUB_AUTOMATION_H_

#include "bdk/bdk.h"

#ifdef __cplusplus
extern "C" {
#endif

/* task identifies one running script: it is not a bdk_task_t. */
typedef void (*bdk_stub_script_cb)(void* context, uint64_t task, bdk_string_t frame_json);

/* The script named `name` (it must also be listed in the resolved configuration's
   automation.scripts) runs as this callback. Until the runtime stops. */
BDK_EXPORT bdk_status_t bdk_stub_script_define(bdk_runtime_t, bdk_string_t name, bdk_stub_script_cb,
                                               void* ctx);
/* A frame from the script. Delivered later. BDK_ERR_STALE_HANDLE once the task ended. */
BDK_EXPORT bdk_status_t bdk_stub_script_send(uint64_t task, bdk_string_t frame_json);
/* The host process dies: every running task ends with automation.hostGone. */
BDK_EXPORT bdk_status_t bdk_stub_automation_host_crash(bdk_runtime_t);

/* The external endpoint's per-connection backlog limits, in bytes (zero keeps the default): past
   the soft limit a slow client's state events are coalesced, past the hard one it is closed. */
BDK_EXPORT bdk_status_t bdk_stub_endpoint_limits(bdk_runtime_t, size_t soft_bytes,
                                                 size_t hard_bytes);

#define BDK_STUB_AUTOMATION_STARTED 0u         /* tasks the host started */
#define BDK_STUB_AUTOMATION_ENDED 1u           /* tasks the host was told to tear down */
#define BDK_STUB_AUTOMATION_SCRIPTS_CHANGED 2u /* user script updates the backend received */
BDK_EXPORT uint32_t bdk_stub_automation_count(bdk_runtime_t, uint32_t which);

#ifdef __cplusplus
}
#endif

#endif /* BDK_STUB_AUTOMATION_H_ */
