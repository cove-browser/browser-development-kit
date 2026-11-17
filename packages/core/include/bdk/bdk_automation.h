/* BDK C ABI: automation sessions. See c-abi.md section 9. Declared: session create, revoke and
   contents grant (PROVISIONAL signatures). Named in c-abi.md but not declared: bdk_session_run,
   bdk_task_cancel, bdk_task_subscribe, bdk_automation_endpoint_open, bdk_automation_endpoint_close,
   bdk_automation_subscribe_audit. */
#ifndef BDK_AUTOMATION_H_
#define BDK_AUTOMATION_H_

#include "bdk_base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Released by bdk_session_revoke (or bdk_release). */
BDK_EXPORT bdk_status_t bdk_session_create(bdk_runtime_t, bdk_string_t options_json,
                                           bdk_session_t* out, bdk_error_t*);
BDK_EXPORT bdk_status_t bdk_session_revoke(bdk_session_t, bdk_error_t*);
BDK_EXPORT bdk_status_t bdk_session_grant_contents(bdk_session_t, bdk_contents_t, bdk_error_t*);

#ifdef __cplusplus
}
#endif

#endif /* BDK_AUTOMATION_H_ */
