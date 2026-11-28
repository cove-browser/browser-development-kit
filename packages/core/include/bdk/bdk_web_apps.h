/* BDK C ABI: installing and uninstalling a web app that the application's own user confirmed. See
   web-apps.md sections 2 and 3. Everything else about web apps is a command in the webApps
   namespace (bdk_invoke) or a request on the web_app_installs surface (bdk_ui.h).
   PROVISIONAL signatures: c-abi.md names no web app functions. */
#ifndef BDK_WEB_APPS_H_
#define BDK_WEB_APPS_H_

#include "bdk_base.h"
#include "bdk_broker.h"

#ifdef __cplusplus
extern "C" {
#endif

/* webApps.install, confirmed: the application has already asked its user, so no web_app_installs
   request is raised. arguments_json and options are those of the command (the profile, and the page
   to install when arguments_json names no url). gesture_event is the event that confirmed it (kind
   BDK_NATIVE_NSEVENT; otherwise the call fails with ui.gestureRequired). Answers as bdk_invoke
   does: 0 is a synchronous rejection, and the result is the command's. */
BDK_EXPORT bdk_call_t bdk_web_app_install(bdk_runtime_t, bdk_string_t arguments_json,
                                          const bdk_invoke_options_t*,
                                          bdk_native_handle_t gesture_event, bdk_result_cb,
                                          void* ctx, bdk_release_cb);

/* webApps.uninstall, confirmed by gesture_event, under the same rules. */
BDK_EXPORT bdk_call_t bdk_web_app_uninstall(bdk_runtime_t, bdk_string_t arguments_json,
                                            const bdk_invoke_options_t*,
                                            bdk_native_handle_t gesture_event, bdk_result_cb,
                                            void* ctx, bdk_release_cb);

#ifdef __cplusplus
}
#endif

#endif /* BDK_WEB_APPS_H_ */
