/* BDK C ABI: what the application does with extensions that needs the user's own gesture. See
   extensions.md sections 4.1 and 4.7. Everything else about extensions is a command in the
   extensions namespace (bdk_invoke) or a UI request on an extension surface (bdk_ui.h).
   PROVISIONAL signatures: c-abi.md names no extension functions. */
#ifndef BDK_EXTENSIONS_H_
#define BDK_EXTENSIONS_H_

#include "bdk_base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What a click on the extension's action does to the page the user is looking at, decided by
   Chromium: activeTab is granted for that page and its action runs. gesture_event is the event
   that clicked (kind BDK_NATIVE_NSEVENT; otherwise ui.gestureRequired). Synchronous: on BDK_OK,
   *out_result_json is {"outcome": "dispatchedClick" | "popupRequested" | "sidePanelRequested" |
   "accessRequested" | "blocked", "reason"?: string}; a popup or side panel arrives afterwards as a
   UI request. Release the buffer with bdk_release. Fails with extension.notFound for an extension
   that is not installed and enabled in the page's profile, and extension.surfaceDisabled when
   [extensions.ui] actions is disabled. */
BDK_EXPORT bdk_status_t bdk_extension_action_activate(bdk_contents_t page,
                                                      bdk_string_t extension_id,
                                                      bdk_native_handle_t gesture_event,
                                                      bdk_buffer_t* out_result_json, bdk_error_t*);

/* Runs a keyboard command an extension declared (extensions.commands) on the page, as its
   shortcut would: the application owns the key bindings. Same gesture rule, result and errors as
   bdk_extension_action_activate; a command that is not the action's is "dispatchedClick" once
   commands.onCommand has fired. */
BDK_EXPORT bdk_status_t bdk_extension_command_perform(bdk_contents_t page,
                                                      bdk_string_t extension_id,
                                                      bdk_string_t command_name,
                                                      bdk_native_handle_t gesture_event,
                                                      bdk_buffer_t* out_result_json, bdk_error_t*);

/* Shows BDK's default extensions menu (extensions.md 4.7) for the window's focused page, at the
   window's extensions anchor, or at the top trailing corner of its content when it has none.
   Fails with extension.surfaceDisabled when actions are not in default mode. */
BDK_EXPORT bdk_status_t bdk_extension_menu_present(bdk_window_t window,
                                                   bdk_native_handle_t gesture_event, bdk_error_t*);

#ifdef __cplusplus
}
#endif

#endif /* BDK_EXTENSIONS_H_ */
