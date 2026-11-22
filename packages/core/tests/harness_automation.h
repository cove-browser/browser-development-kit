/* What the automation tests need from the runtime under test beyond tests/harness.h: an automation
   host whose scripts the test plays, so that sessions, tasks and the wire protocol can be tested
   without JavaScript. The stub offers it (stub/bdk_stub_automation.h); a runtime with a real host
   answers 0 from bdk_harness_automation_hooks, the tests that need it are skipped, and its own
   scenarios run real scripts (tests_engine_automation.c). Plain C. */
#ifndef BDK_TEST_HARNESS_AUTOMATION_H_
#define BDK_TEST_HARNESS_AUTOMATION_H_

#include "bdk/bdk.h"

#ifdef __cplusplus
extern "C" {
#endif

int bdk_harness_automation_hooks(void);

/* Receives every frame BDK sends to the script; task names the running script. */
typedef void (*bdk_harness_script_cb)(void* ctx, uint64_t task, const char* frame, size_t length);
/* The script `name` (listed in the resolved configuration) runs as cb. */
bdk_status_t bdk_harness_script_define(bdk_runtime_t, const char* name, bdk_harness_script_cb cb,
                                       void* ctx);
/* A frame from the script, delivered later. */
bdk_status_t bdk_harness_script_send(uint64_t task, const char* frame_json);
/* The host process dies. */
bdk_status_t bdk_harness_automation_host_crash(bdk_runtime_t);

#define BDK_HARNESS_AUTOMATION_STARTED 0u
#define BDK_HARNESS_AUTOMATION_ENDED 1u
#define BDK_HARNESS_AUTOMATION_SCRIPTS_CHANGED 2u
unsigned bdk_harness_automation_count(bdk_runtime_t, unsigned which);

#ifdef __cplusplus
}
#endif

#endif /* BDK_TEST_HARNESS_AUTOMATION_H_ */
