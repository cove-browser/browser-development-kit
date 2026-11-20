# chromium/bdk

The `//bdk` overlay: BDK's engine, built by GN inside the Chromium checkout and linked into the Mac
framework. Chromium style (`.clang-format`, `BasedOnStyle: Chromium`).

`chromium:build` copies this directory to `<checkout>/src/bdk` under the checkout lock, writing only
files whose content changed (so only they rebuild), deleting files removed here, and excluding `bdk/`
from `git status` in `src`. It also copies, under `//bdk/core`, the runtime core
(`packages/core/runtime`), the C ABI headers (`packages/core/include/bdk`) and the generated ABI hash.
A copy rather than a symlink: the managed checkout is shared by every worktree, and a link out of the
source root would follow whichever worktree last changed it (and dangle when that worktree is removed).
The overlay therefore reflects the worktree that is building.

## Targets

| Target | What |
|---|---|
| `//bdk:core` | The runtime core shared with the stub: every C ABI function's checks, handles, calls, subscriptions, broker and UI requests. Built as BDK's portable C++ (`no_chromium_code`, no Chromium clang plugins); its own lint is in `packages/core`. |
| `//bdk:bdk` | The engine: the Chromium backend of the core, start-up inside the application, and the marker. |

## The engine (`engine/`)

| File | Role |
|---|---|
| `chromium_main.h`, `chromium_main_mac.mm` | Starts the `//chrome` browser process inside an application that already owns `NSApplication` (architecture.md section 6.1). `ChromeMainDelegate` is subclassed so the browser is initialised and handed back instead of running its loop; the loop then runs nested inside the application's own, from a run-loop block, until the runtime shuts down. Adds `--bdk-hosted`, `--no-startup-window`, `--no-first-run`, `--no-default-browser-check`. |
| `application_mac.mm` | `BDKApplicationBase`, the class the Swift SDK's `BDKApplication` subclasses (declared for applications in `packages/core/include/bdk_mac`). It subclasses Chromium's `BrowserCrApplication`, so events, accessibility activation and the message pump behave as in Chrome; `terminate:` is `NSApplication`'s. |
| `engine_backend.cc` | `bdkcore::Backend` over Chromium: start and stop, the profile registry (`BDK Profiles.json` in the user data directory; profile `profile-N` lives in directory `profile-N`), persistent profiles through `ProfileManager` (kept loaded from their first open until the runtime stops: an unload runs in the background, and reopening meanwhile would put two instances on one profile's databases), private profiles as separate off-the-record profiles of the base profile, and every page operation. |
| `engine_contents.{h,cc}` | One page: its `WebContents` (a tab of a host context, owned by the tab model), its observer (events) and its delegate (new-window, fullscreen and context-menu UI requests, keyboard routing, `window.close()`). BDK's delegate serves the page in every context: a `Browser` takes the delegate when it takes a tab, so the engine sets it again after each insertion, move and replacement. Forwarding Chrome's own delegate (`BrowserWebContentsDelegate`) for a service is Phase 2 work, call by call. |
| `host_context.{h,cc}` | Host contexts (architecture.md section 4.1): `HostContext` wraps one `Browser` and its tab model, `HostContexts` owns them. A profile's **parking context** has no native window; a registered window has one context per profile whose pages it shows (a `Browser` belongs to one profile). Pages enter a context as background tabs and move only through the tab model (`DetachTabAtForInsertion`, `InsertDetachedTabAt`), so the tab, its session identifier (the extension tab id) and its tab helpers survive. A context is created on first use, never shown, omitted from session restore, and destroyed with its window (its pages go to parking first), its profile or the runtime. |
| `bdk_browser_window.{h,cc}` | `BdkBrowserWindow`, the `BrowserWindow` of a context, modelled on `WebUIBrowserWindow`: `AsBrowserView()` is null, most calls do nothing, a call a service may need logs once (`NOTIMPLEMENTED_LOG_ONCE`). Identity, bounds, activation and fullscreen state come from the registered native window; `Show`, `Hide`, `Close` and `SetBounds` do nothing, because the application owns its windows and a context outlives its tabs. Real objects, not nulls, where Chromium dereferences without a check: a location bar (`Browser::OnTabDeactivated` saves state into it), a find bar, an exclusive-access context and a modal-dialog host, each a stub for now. |
| `host_window.h`, `host_window_mac.mm` | The platform half of a registered window: the `NSWindow` as `gfx::NativeWindow`, its frame in screen coordinates, key/visible/minimised/zoomed/fullscreen state and key-window notifications (which drive `BrowserActiveStateManager`). The only window code with AppKit types. |
| `host_view.h`, `host_view_mac.mm` | The native view the application places. It holds the page's view, so the page can be replaced (restore, discard) without the application noticing; reports when it enters or leaves a window (`viewDidMoveToWindow`), which moves the page's tab to the registered window's context (later, from a task, never inside the application's own call); the default context menu and fullscreen presenters. Engine files with AppKit types: this, `host_window_mac.mm` and start-up. |
| `session_state.{h,cc}` | Page history for the application to store: a BDK envelope (`BDKC`, version 1) around Chromium's session-restore records. |
| `identity.{h,cc}` | The application's identity from `bdk.resolved.json`: data directory, Keychain item, product name (T-PKG-02). |
| `devtools_frontend.{h,cc}` | Undocked DevTools in a BDK page: Chrome's bundled front end and `DevToolsUIBindings`, attached to the inspected page. Opened from the context menu's Inspect item (absent when `[ui.surfaces] devtools = "disabled"`); arrives as a new-window request of kind `devTools`. |
| `content_browser_client.{h,cc}` | `BdkContentBrowserClient`, Chrome's browser client plus BDK's additions (the delegate supplies it through a patch). Today: the scripted-navigation interceptor and HTTP authentication (`CreateLoginDelegate`). |
| `json_util.{h,cc}` | The engine's one way between JSON text and dictionaries (`ToJson`, `ParseJsonDict`): request payloads, command arguments, test-hook answers. |
| `ui_request.{h,cc}` | The one way the engine raises a request: `RaiseUiRequest` goes through the core's router and gives the engine its outcome (`UiResult`), including whether a granting completion carried a recent mouse-up or key-down of this application's window (the one gesture check, `UiPresenters::IsRecentGesture`). `SurfaceAvailable` and `SurfaceHasHandler` say how the configuration and the application treat a surface. Every surface below uses it, and so do the page's own requests in `engine_contents.cc`. |
| `ui_presenters.h`, `ui_presenters.cc`, `presenters_mac.mm` | The default presenters, behind the platform-neutral `UiPresenters` (payload and callbacks only; no AppKit type outside `presenters_mac.mm`, the one AppKit implementation of them): `NSAlert` sheets for permissions, JavaScript dialogs, authentication and the engine's own prompts (`ShowAlert`: the password and download questions), `NSOpenPanel`/`NSSavePanel` sheets for file pickers and where to save a download, a find bar view and an in-window notification banner. A page that is in no window gets its sheet when it is in one (`ShowAlert` and `ShowSavePanel` answer at once instead when there is no visible window). `ui_presenters.cc` is the one way to script them for tests (`ScriptPresenter`; `ScriptedPresent` for a default that is Chromium's own UI, such as the system print panel). T-MAC-04: [feasibility.md](../../docs/planning/feasibility.md) section 11. |
| `ui_permissions.{h,cc}` | Permission prompts: the page's `PermissionRequestManager` is given BDK's `PermissionPrompt` instead of Chrome's bubble, so the decision (accept, one-time, block, dismiss, ignore) goes back to Chrome and persists in the profile's content settings. Camera and microphone reach it through `MediaCaptureDevicesDispatcher`, forwarded by `EngineContents`. |
| `ui_dialogs.{h,cc}` | `PageDialogs`: the page's `JavaScriptDialogManager` (alert, confirm, prompt, beforeunload, suppression) and `RunFileChooser`/`EnumerateDirectory`. |
| `ui_auth.{h,cc}` | The login delegate for HTTP and proxy authentication, supplied by `BdkContentBrowserClient::CreateLoginDelegate`. |
| `ui_find.{h,cc}` | `PageFind`: find in page on Chrome's `FindTabHelper`; the bar is a `find` request. The application's own search field uses `Start`/`Next`/`Stop` and reads `result()` (not in the C ABI yet). |
| `ui_notifications.{h,cc}` | A `NotificationDisplayServiceImpl` per profile that raises `notifications` requests instead of using a platform bridge; clicks and closes return through Chrome's notification handlers. |
| `testing_commands.cc` | `bdk_testing_command` (see `bdk_testing.h`), the one command hook for tests: scripts the default presenters, drives find, reads a page's site setting or a preference, captures a page, answers the operating system's authentication prompt, sets the download policy, serves pages with content of their own. |
| `services/` | The M3 services and prompts. `passwords_service`, `autofill_service`: the `passwords.*` and `autofill.*` commands (one entry point, `ExecutePasswordsOrAutofillCommand`, called first by the backend's `ExecuteCommand`). `authentication{,_mac}`: `Backend::Authenticate` over LocalAuthentication. `password_prompts`: the password save and update bubble as a `passwords` request (a `TabDialogs` subclass; the question is asked when the page finishes a navigation with one pending, see ui-requests.md 5.8). `autofill_prompts`: the save-address, update-address and save-card bubbles as `autofill` requests (an `AutofillBubbleHandler` the host context's window creates and registers in the Browser's user data) and the suggestion popup as `autofill_suggestions` requests (a popup view the factory hook makes; Chromium's own popup when the application has no handler). `download_prompts`: where to save and dangerous downloads as `downloads` requests (a `ChromeDownloadManagerDelegate` subclass and download observers). `print_prompts`: `window.print()` as a `printing` request, system print panel by default. `interstitial_prompts`: certificate and Safe Browsing interstitials as requests that can only go back or proceed. `prompt_hooks`: installs the hooks the patches add. |
| `bdk_testing.h`, `testing.{h,cc}` | Test hooks, exported but not ABI: scripted navigations served without the network (and pages with content of their own), UI requests for the application's own handler, script with a user gesture, synthetic clicks, a run-loop slice that lets Chromium work from inside a task, a pending-work count, the count of Chrome windows that are not host contexts and of host contexts, and where a page's tab is (its context, the tab's session identifier, whether it is active). Used by the engine conformance app (`packages/core`, `nx run core:test-engine`). None gives a page or a session anything. |

Test and diagnostic switches, read from the environment of the application: `BDK_ENGINE_MOCK_KEYCHAIN=1`
(Chromium's mock Keychain, so tests never prompt), `BDK_ENGINE_SWITCHES` (more Chromium switches, space
separated, no dashes), and `BDK_ENGINE_KEEP_CHROMIUM=1` (a runtime shutdown leaves Chromium running, so
one process can start several runtimes; Chromium itself starts once per process).

## Host contexts

Every page is a tab of a `Browser`, so Chrome's tab helpers and tab-scoped services attach (T-ADP-01 to
T-ADP-03; the C ABI did not change).

| Application action | What the engine does |
|---|---|
| Creates a page | Inserts its `WebContents` as a background tab of the profile's parking context |
| Adds the page's view to a view in registered window W | After the view reports it, moves the tab to W's context for the page's profile, creating the context on first use. A view in a window that was never registered changes nothing |
| Moves the view to another registered window | The same move; tab id, history and renderer survive |
| Removes the view from every window | Nothing: the page stays a background tab of its last context |
| Unregisters W (`bdk_window_unregister`) | Moves W's pages to parking, then closes W's contexts |
| Focuses a page (`bdk_contents_focus`, or the page taking focus) | Makes its tab the active tab of its context; a window's context becomes the most recently active one |
| Destroys the page | Closes its tab (`DetachAndDeleteWebContentsAt`) |

Restoring or discarding a page replaces its `WebContents` inside the same tab (`DiscardWebContents`), so the
context is kept; the tab's session identifier changes then, as in Chrome.

`chromium_main_mac.mm` keeps the `APP_CONTROLLER` keep-alive, so closing the last context never starts a
shutdown. `bdk_testing_chrome_window_count()` ignores host contexts; anything it counts is a window Chromium
opened itself.

What Chromium requires of a window that is not a `BrowserView` (T-ADP-02) is in
[amendments.md](../../docs/planning/amendments.md) section 2.13.

## Services (`engine/services/`)

`EngineBackend::ExecuteCommand` hands every built-in command to `ExecuteBuiltinCommand`
(`command_dispatch.cc`), which finds a handler by command name; a command with no handler answers
`broker.notImplemented`. One file per service, each adding to the table: `history_service.cc`,
`bookmarks_service.cc`, `downloads_service.cc`, `settings_service.cc` (the catalogue table maps each key to one
Chromium pref; a key not in it reads as `null` and cannot be written), `permissions_service.cc` (the kind table maps
each kind to a `ContentSettingsType`), `cookies_service.cc`, `privacy_service.cc` (`BrowsingDataRemover`),
`profiles_service.cc` (`runtime.info`, `profiles.*`), `system_service.cc` with `system_mac.mm` (the only platform
code: default-browser status and request), and `capture_service.cc` (PNG screenshot, print to PDF). A handler is
`void(const std::shared_ptr<ServiceCall>&)`; `ServiceCall` carries the parsed arguments, the Chromium `Profile`
and the ways to answer. Result field names and shapes are those of `packages/core/interface/*.json`; timestamps are
integer milliseconds; void commands answer `null`.

A private profile reads and writes its parent's bookmarks, reads and deletes in its parent's history and records
none, and has its own cookies, content-setting exceptions, preferences and downloads list. Content-setting defaults
are set on the parent's map, which the private profile's follows.

Events (`events.cc`): per opened profile a `ServiceEvents` observes history, bookmarks and downloads and emits
`history.*`, `bookmarks.*` and `downloads.*`; command handlers emit `settings.changed`, `permissions.changed`,
`privacy.cleared` and `profiles.*`. All go through the core's `emit_builtin_event`. Not emitted: `cookies.changed`,
`autofill.changed`, `passwords.*`.

Not implemented (answer `broker.notImplemented`): `bookmarks.import`/`export`, `permissions.certificateExceptions`
and `revokeCertificateException` (Chromium keeps no list to read), and every `passwords.*` and `autofill.*`
command. Screenshots and PDFs have no command yet (`contents.screenshot` and `contents.pdf` are M7); the functions
are in `capture_service.cc` and the test hook `bdk_testing_command` starts them (`capture.start`). `privacy.clear` cannot stop a
removal that has begun. `privacy.estimate` counts history, cookies and downloads; other types report 0 items.

Pages record visits in history through Chromium's `HistoryTabHelper` (attached in `EngineContents::Attach`).

## The patches it needs

`chromium/patches` links `//bdk` into the framework, exports the C ABI, `BDKApplicationBase` and the test hooks
(`chromium:build` fails when the framework does not export every function the headers declare), lets the
engine keep the content runner after start-up, leaves `NSApplication`'s delegate and menu to the
application, lets the application name its Keychain item, lets the engine supply its own
`ChromeContentBrowserClient`, lets it supply the `BrowserWindow` of a `Browser` (a settable factory that
`BrowserWindow::CreateBrowserWindow` asks first), accepts a `Browser` with no side panel UI outside tests,
allows a `Browser` in a BDK private profile, lets the engine supply the `TabDialogs` of a tab (the passwords bubble; a settable factory that `TabDialogs::Create` asks first), lets it present printing (a settable hook `PrintViewManager` asks before it creates the print preview dialog, in both the print command's and `window.print()`'s entry), lets it supply the autofill popup view (a settable factory `AutofillPopupView::Create` asks first, with Chromium's own popup callable), makes the address bubbles find the autofill bubble handler in the Browser's user data (they asked for a `BrowserView`), and exports the test hooks (the individual ones and the one command hook `bdk_testing_command`, all in the same patch). Each patch's message says why.

`bdk_overlay_marker()` stays as the cheapest proof that `//bdk` is linked: `chromium:build` runs
`nm -gU` on the built framework and fails if the symbol is absent.
