/* Test hooks of the Chromium engine. Not part of the ABI: the engine's
   counterpart of packages/core/stub/bdk_stub.h, used by the conformance
   harness (packages/core/tests/harness_engine.mm) and the engine scenario
   tests. The framework exports them; an application has no reason to call
   them.

   None of them gives a page or an automation session anything: each acts as
   the application itself (raising a UI request to the application's own
   handler, calling its own keyboard handler) or on the application's own
   pages, which the application already controls fully through the ABI. */
#ifndef BDK_ENGINE_BDK_TESTING_H_
#define BDK_ENGINE_BDK_TESTING_H_

#include "bdk/bdk.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Serves later navigations to `url` from the engine instead of the network:
   net_error 0 answers with an HTML page titled `title` (the URL if empty);
   otherwise the navigation fails with that network error. */
BDK_EXPORT bdk_status_t bdk_testing_script_navigation(bdk_runtime_t,
                                                      bdk_string_t url,
                                                      int32_t net_error,
                                                      bdk_string_t title);

/* With `on` set, every http and https navigation is served as if scripted
   with net_error 0 and no title, so a test never reaches the network. */
BDK_EXPORT bdk_status_t bdk_testing_intercept_all(uint8_t on);

/* Raises a UI request as Chromium would, for the application's own handler.
 */
BDK_EXPORT bdk_status_t bdk_testing_raise_ui_request(bdk_runtime_t,
                                                     uint32_t surface,
                                                     bdk_contents_t,
                                                     bdk_string_t payload_json);
BDK_EXPORT bdk_status_t bdk_testing_withdraw_ui_requests(bdk_runtime_t);

typedef struct bdk_testing_ui_outcome {
  size_t struct_size;
  uint32_t kind; /* 0 none, 1 completed, 2 defaulted, 3 rejected,
                    4 cancelled, 5 withdrawn */
  uint32_t surface;
  char completion[32];
  char arguments_json[128];
} bdk_testing_ui_outcome_t;

/* The outcome of the request most recently raised by
   bdk_testing_raise_ui_request. */
BDK_EXPORT bdk_status_t
bdk_testing_last_ui_outcome(bdk_runtime_t, bdk_testing_ui_outcome_t* out);

/* Calls the page's keyboard handler as its view would for `key_event`. */
BDK_EXPORT bdk_status_t bdk_testing_simulate_key(bdk_contents_t,
                                                 bdk_native_handle_t key_event,
                                                 uint8_t* out_reserved);

/* Runs `script` in the page's main frame, with a user activation if
   `user_gesture` is set (for window.open and requestFullscreen). */
BDK_EXPORT bdk_status_t bdk_testing_execute_script(bdk_contents_t,
                                                   bdk_string_t script,
                                                   uint8_t user_gesture);

/* Sends a mouse click at x, y in the page's view (top-left origin). button: 0
   left, 2 right. A right click opens the context menu. */
BDK_EXPORT bdk_status_t bdk_testing_click(bdk_contents_t,
                                          double x,
                                          double y,
                                          uint32_t button);

/* Runs the application's run loop for `seconds`, letting Chromium run its
   tasks even when called from inside one (a harness running as a task). */
BDK_EXPORT void bdk_testing_run_for(double seconds);

/* The next runtime starts with no persistent profiles, as with a new data
   directory (Chromium's data directory is fixed for the process). Their data
   stays on disk under identifiers that are not reused. */
BDK_EXPORT void bdk_testing_forget_profiles(void);

/* How many Chrome browser windows (Browser objects) exist that are not host
   contexts. BDK's host contexts are Browsers with no native window of their
   own, so they are not counted: anything above 0 means Chromium opened a window
   on its own. */
BDK_EXPORT size_t bdk_testing_chrome_window_count(void);

/* How many host contexts (the Browsers BDK keeps its pages' tabs in) exist. */
BDK_EXPORT size_t bdk_testing_host_context_count(void);

typedef struct bdk_testing_tab_info {
  size_t struct_size;
  uint8_t is_tab;            /* the page is a tab of a host context */
  uint8_t parking;           /* ... of a parking context (no window) */
  uint8_t active;            /* ... and the active tab of it */
  uint8_t has_native_window; /* the context's window has a native window */
  int32_t tab_id;     /* the tab's session identifier: the extension tab id */
  int32_t context_id; /* the context's window identifier (Browser session id) */
  uint32_t tab_count; /* tabs in the context */
  uint32_t reserved;
} bdk_testing_tab_info_t;

/* Where the page's tab is. */
BDK_EXPORT bdk_status_t bdk_testing_tab_info(bdk_contents_t,
                                             bdk_testing_tab_info_t* out);

/* Drives and inspects the browser-owned surfaces and the services: one
   function, so that the framework's export list needs one name for all of it.
   `command` names what to do, `args_json` is an object ("contents" is a decimal
   handle string), and `out` receives the JSON object the command writes (empty
   for most). BDK_ERR_INVALID_ARGUMENT for an unknown command, bad arguments, or
   a result that does not fit `out_size`. Commands:

   present.script {"surface", "answer"}  while scripted, the surface's default
   presenter shows nothing and answers as `answer` (an object) says: permissions
       {"choice": "allow" | "allowThisVisit" | "block" | "dismiss"},
       javascript_dialogs {"accept", "text", "suppress"}, http_auth
       {"username", "password"} (none: cancel), file_pickers {"paths": [...]},
       notifications {"event": "clicked" | "closed" | "timeout", "action"},
       passwords {"answer": "save" | "notNow" | "never"} (the key of the
   button), downloads {"answer": "keep" | "discard"} for a dangerous download or
   a path (or "cancel") for where to save, printing {"answer": "systemDialog"}
   (no panel opens); an answer with "none": true is never given, as a person who
   has not decided present.clear   ends all scripts and forgets what was
   recorded present.record {"surface"} -> {"shown", "dismissed", "payload"} what
   the default presenter was asked to show, and how often it was taken down
   find.present / find.start {"text", "matchCase"} / find.next {"forward"} /
   find.stop / find.result {"contents"} -> {"ordinal", "count", "presented"}
   permission.get {"contents", "origin", "kind"} -> {"value": "allow" | "block"
   | "ask"} the page's profile's site setting pref.json {"contents", "name"} ->
   {"value": <the preference's JSON value>} capture.start {"contents", "path",
   "format": "png" | "pdf"} writes the page to the file in the background;
   capture.state -> {"state": 0 (running, or none started) | 1 (written) | 2
   (failed)} tab.chromiumDiscard {"contents"} -> {"discarded"}  Chromium's own
   urgent discard of the page's tab (TabLifecycleUnit), as memory pressure
   would make it authentication {"answer": "allow" | "deny" | "ask"}  what the
   operating system's authentication prompt answers ("ask" is the real prompt,
   the default); authentication.count -> {"count"}  how many times a command
   asked for it downloads.ask {"on"}  every loaded profile asks where to save,
   as the setting does downloads.replaceDelegate {"contents"}  BDK's download
   delegate is put in again on the page's profile after its download manager
   exists, as for the profile Chromium loads at startup downloads.danger {"on"}
   every download is held as dangerous content (this build has no Safe Browsing
   or file-type policy to flag a real one before M6) script_page {"url", "body"
   (or "bodyBase64" for a binary body), "contentType", "headers"}  serves
   later navigations to url with this content
   reputation.list {"json"} -> {"loaded", "error"}  gives the default
   reputation provider a block list, as a download whose signature was checked
   would providers.settings -> {"componentUpdatesDisabled", "syncDisabled",
   "platformOnlyGeolocation", "googleKey", "geolocation", "reputation"}  what
   Chromium runs with network.hosts {"path"} -> {"hosts": [...]}  the hosts a
   net log written with --log-net-log has requests or resolutions for
   hsts.add {"contents", "host"}  the page's profile treats host as HSTS
   network.passthrough {"host"}  loads from host go to the network, not to the
   scripted pages (a test's server on a name BDK_ENGINE_HOST_RESOLVER_RULES
   maps to the loopback address) startup.post  posts a task as Chromium posts
   its after-start-up work; startup.state -> {"complete", "ran"} whether
   Chromium's start-up is complete and whether that task ran
   protocols.permit  Chrome accepts the next external-protocol launch, as after
   a user gesture
   pages.count -> {"webContents", "pages", "hostContexts"}  what is alive in
   the process: every WebContents, BDK's pages and their host contexts
   extensions.pack {"dir", "crx", "key"} -> {"id"}  packs the directory into the
   CRX with Chromium's ExtensionCreator, signed with the key in the file `key`
   (made when missing), so a second package of the same key is an update of the
   first extensions.serve {"url", "body" | "file"}  a fetch of the url (an
   update manifest, a package) is answered by the engine, not the network;
   neither removes the entry. present.script on "extension_installs" and
   "extension_uninstalls" answers the default sheets with the key of a button
   ("install" or "remove", "cancel"). present.script on "webauthn" answers
   the default sheet with a completion, {"completion": "accept" | "grant" |
   "deny", "mechanism" | "account" | "pin" | "action"}, or with one such
   answer per step name ({"enterPIN": {...}, "touchSecurityKey": {"none":
   true}})
   present.script also answers the engine's own sheets for Chromium's other
   browser dialogs (ui_browser_dialogs.h) with the key of a button:
   "external_protocols" ("cancel", "open"), "client_certificates"
   ("certificate<N>", "cancel"), "browser_dialogs" ("accept", "cancel"; a
   tab-modal dialog model) and "passwords" ("account<N>", "cancel" for the
   credential manager's account chooser; "ok", "turnOff" for its auto sign-in
   notice); any other key dismisses the sheet.
   dialogs.tab_modal {"contents", "title", "text", "defaultCancel"?}  gives
   chrome::ShowTabModal a model with that title and paragraph, OK and Cancel
   (Cancel the default when asked), as File System Access does; poll
   dialogs.tab_modal.result -> {"result": "accepted" | "cancelled" | "closed" |
   "" (open), "destroyed"}
   pref.set {"contents", "name", "value"}  sets the Chromium preference `name`
   of the page's profile to the JSON value (of the preference's own type)
   webauthn.virtual {"pin"?, "residentKeys"?, "uv"?}  every WebAuthn request
   from now on sees one virtual USB security key (CTAP2, touched at once; a PIN
   makes it ask for one, "uv" verifies its user itself) and no other
   authenticator; webauthn.inject {"rpId", "userId",
   "name", "displayName"} -> {"injected"}  a passkey on it; webauthn.state ->
   {"installed", "credentials"}; webauthn.clear  the real authenticators again
   chooser.open {"contents", "kind", "devices": [names]}  a device chooser of
   those devices on the page, shown as WebUSB's is (a device_choosers request);
   chooser.add {"name"} and chooser.remove {"name"} change its list;
   chooser.result -> {"ended", "chosen"? (the name; absent when cancelled)}.
   clientcert.identity {"certificate", "key"} (PEM) -> {"set"}  the one client
   identity every profile offers, instead of the Keychain's; {} clears it
   present.script on "device_choosers" answers {"device": name} or
   {"answer": "cancel"}

   Extensions, windows and side panels (extensions/panel_testing.cc; "window"
   is a decimal bdk_window_t string):
   extensions.load {"contents", "path"}  loads the unpacked extension at path
       in the page's profile without the install prompt; poll extensions.loaded
       -> {"done", "id", "error"}
   extensions.eval {"contents", "extension", "script"}  runs the script in the
       extension's service worker (its value is the script's synchronous
       result; a script that needs a promise stores the answer in a global and a
       second call reads it); poll extensions.eval.result -> {"done", "value"
       (JSON text), "error"}
   window.extension_windows {"contents"} -> {"windows": [{"id", "type"}]}  the
       windows the extension windows API lists for the page's profile
   sidepanel.state {"window"} -> {"open", "extension", "url", "pageScoped",
       "visible", "defaultPresenter", "frame": [x, y, w, h] (content view,
       origin top left), "requestOpen", "opened"}  the extension side panel
       open in the window
   sidepanel.drag {"window", "delta"}  drags the default panel's divider by
       delta points (negative: wider) and lets go
   prompt.anchored -> {"attached", "frame"}  the prompt a default presenter
       attached to a window anchor, in the content view; prompt.anchored.answer
       {"index"}  presses its button

   Extension UI (T-EXT-02, T-EXT-04). present.script also takes the surfaces
   "extension_popups" ({"action": "blur" | "close"} answers as a blur or a
   close; "none": true leaves the popup open) and "extension_menu" ({"choose":
   "<extension id>" | "manage"} chooses a line; present.record's payload holds
   {"items": [ExtensionActionState]}).
   extui.load {"contents", "path"}  loads the unpacked extension at path in the
   page's profile; poll extui.loaded -> {"done", "id", "error"}
   extui.has_active_tab {"contents", "id"} -> {"granted"}  whether activeTab is
   granted for the page
   extui.context_items {"contents", "id"} -> {"count"}  the extension's
   contextMenus items
   extui.popup {"contents"?} -> {"opened", "shown", "closed", "lastCloseReason",
   "lastSize", "open"}  what popups did, and whether the page's window has one
   open
   extui.blur {"contents"}  the popup of the page's window loses focus
   extui.titlebar {"contents"} -> {"present", "visible", "x", "y", "width",
   "height"}  the title-bar extensions button of the page's window (relative to
   the window's frame, origin top-left) */
BDK_EXPORT bdk_status_t bdk_testing_command(bdk_runtime_t,
                                            bdk_string_t command,
                                            bdk_string_t args_json,
                                            char* out,
                                            size_t out_size);

/* Work the engine has queued or started for the application: BDK tasks not yet
   run plus pages still loading. A harness pumps until it reads 0. */
BDK_EXPORT size_t bdk_testing_pending_work(void);

#ifdef __cplusplus
}
#endif

#endif /* BDK_ENGINE_BDK_TESTING_H_ */
