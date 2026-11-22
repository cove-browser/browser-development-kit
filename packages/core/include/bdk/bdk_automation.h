/* BDK C ABI: automation sessions, tasks and the external endpoint. See c-abi.md section 9 and
   packages/core/docs/conventions.md ("Automation"). Signatures are PROVISIONAL: c-abi.md names the
   functions without their shapes. The wire protocol scripts and external tools speak is in the
   same conventions section. The audit trail is not a function of its own: it is the built-in
   events of namespace `automation` (bdk_subscribe_named). */
#ifndef BDK_AUTOMATION_H_
#define BDK_AUTOMATION_H_

#include "bdk_base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* options_json: {"name": string, "capabilities": [string],
                  "scope": {"profiles": "all" | [profile id], "contents": "all" | "created" |
                            "explicit"}}
   scope defaults to every profile and the pages the session creates. A capability that may not be
   granted to a session fails with capability.denied, a profile that is not open with
   profile.notFound, any other bad shape with broker.invalidArguments. Released by
   bdk_session_revoke (or bdk_release), which ends its tasks, calls, subscriptions and external
   connections and removes its user scripts. */
BDK_EXPORT bdk_status_t bdk_session_create(bdk_runtime_t, bdk_string_t options_json,
                                           bdk_session_t* out, bdk_error_t*);
BDK_EXPORT bdk_status_t bdk_session_revoke(bdk_session_t, bdk_error_t*);
/* Widens the scope by one page; for "explicit" scopes (a "created" scope takes pages too). */
BDK_EXPORT bdk_status_t bdk_session_grant_contents(bdk_session_t, bdk_contents_t, bdk_error_t*);

/* Runs a bundled script, by its name in the resolved configuration (automation.scripts), in the
   automation host, as the session. arguments_json is any JSON value, passed to the script's
   default export. Fails with automation.disabled when [automation] enabled is false and with
   automation.scriptNotFound for a name that is not listed. The task starts later; its events
   arrive through bdk_task_subscribe. Releasing the handle (bdk_release) cancels a task that is
   still running. */
BDK_EXPORT bdk_status_t bdk_session_run(bdk_session_t, bdk_string_t script,
                                        bdk_string_t arguments_json, bdk_task_t* out, bdk_error_t*);
/* Ends a running task: it finishes with broker.cancelled. Does nothing to a finished task. */
BDK_EXPORT bdk_status_t bdk_task_cancel(bdk_task_t, bdk_error_t*);

/* kind is BDK_TASK_EVENT_* (bdk_ids.h). detail_json: PROGRESS {"fraction","message"}; LOG
   {"level","message"}; COMMAND {"command","contents"?,"profile"?,"error"?:{"domain","code"}}, one
   per command the task called, when it ends; FINISHED {"result": json} or
   {"error":{"domain","code","message"}}. FINISHED is the last event and the subscription is
   released after it. A subscription made after the task finished receives FINISHED alone. */
typedef void (*bdk_task_event_cb)(void* context, bdk_task_t task, uint32_t kind,
                                  bdk_string_t detail_json);
BDK_EXPORT bdk_subscription_t bdk_task_subscribe(bdk_task_t, bdk_task_event_cb, void* ctx,
                                                 bdk_release_cb);

/* The external endpoint (T-AUT-04). Off unless [automation.external] enabled is true
   (automation.disabled). options_json: {} or {"directory": path}, a directory of the user's that
   no one else may write; the socket goes in a new 0700 directory inside it (by default inside the
   temporary directory). Nothing listens on a network port. One endpoint per runtime. Fills
   out_socket_path (a buffer the caller releases). Every connection's first frame is a hello;
   hello_cb receives it later, on the UI thread, with {"token","client"}, and the application
   answers once, now or later, with bdk_automation_hello_accept or _refuse. BDK owns ctx until the
   endpoint closes. */
typedef void (*bdk_automation_hello_cb)(void* context, bdk_context_t hello,
                                        bdk_string_t hello_json);
BDK_EXPORT bdk_status_t bdk_automation_endpoint_open(bdk_runtime_t, bdk_string_t options_json,
                                                     bdk_automation_hello_cb, void* ctx,
                                                     bdk_release_cb, bdk_buffer_t* out_socket_path,
                                                     bdk_error_t*);
/* Closes every connection and removes the socket. */
BDK_EXPORT bdk_status_t bdk_automation_endpoint_close(bdk_runtime_t, bdk_error_t*);
/* Binds the connection to a live session and welcomes it. The hello handle is spent either way;
   it is stale already if the client went away. */
BDK_EXPORT bdk_status_t bdk_automation_hello_accept(bdk_context_t hello, bdk_session_t,
                                                    bdk_error_t*);
BDK_EXPORT bdk_status_t bdk_automation_hello_refuse(bdk_context_t hello, bdk_string_t message,
                                                    bdk_error_t*);

#ifdef __cplusplus
}
#endif

#endif /* BDK_AUTOMATION_H_ */
