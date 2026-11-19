/* BDK C ABI: diagnostics. See c-abi.md section 9. Declared: the log handler (PROVISIONAL
   signature). Named in c-abi.md but not declared: bdk_crash_reports_list,
   bdk_crash_reports_set_handler, bdk_netlog_start, bdk_netlog_stop. */
#ifndef BDK_DIAGNOSTICS_H_
#define BDK_DIAGNOSTICS_H_

#include "bdk_base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* level: 0 debug, 1 info, 2 warning, 3 error. Strings are borrowed until the callback returns. */
typedef void (*bdk_log_cb)(void* context, uint32_t level, bdk_string_t message);

/* Replaces any earlier handler. A null callback removes it. */
BDK_EXPORT bdk_status_t bdk_log_set_handler(bdk_runtime_t, bdk_log_cb, void* ctx, bdk_release_cb,
                                            bdk_error_t*);

#ifdef __cplusplus
}
#endif

#endif /* BDK_DIAGNOSTICS_H_ */
