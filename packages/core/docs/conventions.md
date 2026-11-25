# BDK C ABI conventions

Specification: `docs/planning/sdk/c-abi.md`. This file records the rules the headers follow, the decisions taken where
c-abi.md was silent, and how the ABI hash works. Tasks T-ABI-01 (headers, version, hash) and T-ABI-02 (stub, suite).

## Layout

| Path | Contents |
|---|---|
| `include/bdk/*.h` | Public headers: plain C, `extern "C"` guards, `BDK_EXPORT`, no platform header, no platform type |
| `interface/` | Interface definitions (JSON, T-ABI-03): one `<namespace>.json` per namespace, `policy.json`, and the identifier registry `ids.json`. Every `*.json` here goes into the hash |
| `runtime/` | The runtime core, C++23, no exceptions, no platform code: handles, errors, calls, subscriptions, broker (`broker.cc`, `broker_args.cc`, `broker_internal.h`), application settings (`app_settings.cc`), UI requests and every ABI function's checks. What a page does is behind the `Backend` interface in `runtime/core_internal.h` |
| `stub/` | The stub's in-memory backend and its task queue. Test hooks in `stub/bdk_stub.h` are not ABI |
| `tests/` | Conformance suite in C11, written against `bdk/bdk.h` and `tests/harness.h` only |
| `cmake/abi-hash.cmake`, `generated/` | Hash generator and checker; the recorded hash. Also `bdk_command_table.inc`, generated from `interface/` (not part of the hash: its inputs are) |

## Rules (c-abi.md section 2, as implemented)

* Handles are 64-bit identifiers. The stub packs type tag, generation and index. Zero is never valid; a stale or
  mistyped handle returns `BDK_ERR_STALE_HANDLE` and touches nothing. Do not rely on the packing: only the runtime core
  does it.
* Every struct passed by pointer starts with `size_t struct_size`. The callee reads or writes only the bytes
  declared and treats missing fields as zero. A `struct_size` smaller than `sizeof(size_t)` is
  `BDK_ERR_INVALID_ARGUMENT`.
* Strings are UTF-8 with explicit length, borrowed. Strings out come in a `bdk_buffer_t` (release with
  `bdk_release`) or are borrowed until a callback returns.
* Everything is on the UI thread unless marked `BDK_ANY_THREAD`. Other threads get `BDK_ERR_WRONG_THREAD`
  (functions returning a handle return 0 and call nothing back). The thread that calls `bdk_runtime_start` is the UI
  thread until shutdown completes.
* No callback runs synchronously from the call that causes it (results, events, release callbacks, `resolve`).
* Asynchronous calls: the result callback runs exactly once, then the release callback. A cancelled call delivers
  `broker.cancelled`. A call that returns 0 was rejected synchronously and calls nothing back. A `bdk_call_t` goes
  stale when its result callback starts (cancel then returns `BDK_ERR_STALE_HANDLE`).
* Registration functions (`bdk_app_*`, `bdk_page_handler_register`, `bdk_provider_register`, `bdk_ui_set_handler`)
  work only between start and `bdk_runtime_freeze_registrations`; later they fail with
  `config.registrationAfterStart`. On success BDK owns the context and calls its release callback once, at shutdown
  (or on replacement). On failure the caller keeps the context and no release callback runs.
* Contents cannot be created before the freeze (`BDK_ERR_NOT_STARTED`).
* Errors: `bdk_status_t` plus a trailing `bdk_error_t* out_error`, set to 0 on entry and filled only with
  `BDK_ERR_FAILED`. Domains and codes are strings (`bdk_ids.h` has the ones used so far).
* Coordinates are window coordinates in device-independent pixels, origin top-left. Screen coordinates (a
  window request's `frame`, Swift's `BDKWindow` frames) are Chromium's: device-independent pixels, origin at the
  top-left of the primary display, y downwards; only macOS converts (T-XPL-01, X8). Paths are UTF-8 strings: a
  runtime converts them at the edge from UTF-8 (`char8_t`, `base::FilePath::FromUTF8Unsafe`), never through
  `std::filesystem::path(std::string)`, which reads the ANSI code page on Windows (X6).

## Decisions where c-abi.md was silent or sketched (provisional)

1. Signatures. Section 5 gives families, not signatures. The declared signatures follow the section 2 conventions and
   are marked PROVISIONAL in the headers. They are what the stub implements and the suite tests.
2. `bdk_release` returns `bdk_status_t` (c-abi.md leaves it unspecified) so double release is observable. Runtime,
   call and call-context handles are not released through it (`BDK_ERR_INVALID_ARGUMENT`).
3. `bdk_buffer_bytes(bdk_buffer_t)` was added: a buffer needs an accessor.
4. `bdk_profile_open` returns the profile handle through `bdk_profile_t* out_profile` (written synchronously, valid
   after the result callback succeeds) because the result callback carries only JSON. `bdk_profile_close` releases
   it. One open handle per profile (`profile.inUse`).
5. Ephemeral profiles: `bdk_profile_open_private`, id `ephemeral:N`, never listed, gone after close.
6. `bdk_contents_state_t` strings are borrowed until the next `bdk_contents_state` call on that contents. Its
   `audio` and `capture` are flag words (`BDK_AUDIO_*`); no capture flags are defined.
7. Callback types c-abi.md names without a signature: `bdk_runtime_event_cb`, `bdk_event_cb`, `bdk_key_reserve_cb`,
   `bdk_key_unhandled_cb`, `bdk_native_page_cb`. Provisional shapes are in the headers.
8. `bdk_subscribe_named` was added (by-name pair of `bdk_subscribe`): application events have names and no
   generated identifier, so they could not be observed otherwise.
9. `bdk_provider_register` and `bdk_provider_set_key` take a `bdk_runtime_t` (c-abi.md omits it). The vtable and its
   contract are in "Providers" below (T-PRV-01).
10. Application command and event names must start with `app.` (`config.nameNotApp`). Duplicates fail
    with `config.duplicateName` (not in the swift-api error table).
10a. Application settings (`bdk_app_setting_register`, T-SVC-08): the broker, not the backend, answers `settings.get`,
    `getMany`, `describe`, `set` and `reset` for `app.` keys (`runtime/app_settings.cc`), so every runtime behaves the
    same. A key's type is the JSON kind of its default and `set` checks it. A key registered with `exposedToPages`
    false is refused with `capability.denied` to a page or session before the service runs, whatever it holds. The
    values are cached by scope (profile id, or the application) and the backend only keeps them:
    `Backend::LoadAppSettings` is called once per scope after the profile opens, and `SaveAppSettings` with the whole
    JSON object after each change; neither is called for a private profile. A `getMany` that mixes `app.` and browser
    keys is `broker.invalidArguments`.
11a. Find and print (T-SWF-07). Find results are `BDK_CONTENTS_EVENT_FIND_RESULT` events on the contents subscription, not
    a callback of their own: they are ordered with the page's other events and need no new handle or release rule. The
    detail is `{"matchCount","activeIndex","rect":{"x","y","width","height"},"final"}`; `activeIndex` is 1-based (0 for
    none) and `final` is false while Chromium is still counting. `find_next` and `find_previous` do nothing when no
    search is in progress. The stub renders nothing: every start and every move reports zero matches, one final event
    each (an empty text clears the search; after `find_stop` the moves report nothing), and `find_present` and `print`
    raise the `find` (payload `initialText` is the last search) and `printing` (`isUserInitiated` true) requests, one
    find UI open per page. `bdk_contents_print` takes the path the print command takes, so with no handler the default
    presenter runs.
11. Which completions grant is the `grants` column of the surface table in `runtime/ui_router.cc`, from
    ui-requests.md section 5: `grant` (permissions, and on every surface), `keep` (downloads), `save` (passwords,
    autofill), `accept` (autofill suggestions), `proceed` (interstitials), `select` (device choosers), `share` (screen
    capture), `open` (external protocols). A granting completion needs a gesture event of kind `BDK_NATIVE_NSEVENT`
    (`ui.gestureRequired`). Completing twice fails with `ui.requestCompleted`. Other completion names are not
    validated per surface until the per-surface interface definitions exist; the router adds only the rules in
    "The UI request router" below.
12. Built-in events (the events of the interface definitions) are emitted by a backend with `emit_builtin_event`
    (`runtime/broker_internal.h`): delivered later, in order, to the subscriptions for the event's name or
    identifier. A subscription made with a profile in its options gets only that profile's events; one made
    without gets every profile's. Events with no profile (`profiles.*`) reach only the unfiltered subscriptions.
    A page in the options works the same way: the subscription gets only events emitted for that page
    (`emit_builtin_event_for` and `emit_builtin_event_to` name one), and none of the events that name no page.
12. `bdk_native_handle_t` and `bdk_native_kind_t` live in `bdk_base.h` (c-abi.md puts them in section 5.4) because the
    start configuration needs them.
13. `include/bdk/bdk_ids.h` is generated (T-ABI-03) from `interface/*.json` and the registry `interface/ids.json`; do
    not edit it. Error domains and codes, runtime and contents event kinds, event flags, audio, security and visibility
    values and surface numbers are `constants` in `interface/abi.json`, with the values they had when the file was
    written by hand. Surface numbers follow the order of the ui-requests.md table. See "Interface definitions and
    identifiers".

14. A surface's mode comes from `surfaces` in the resolved configuration. A surface that is not listed is `default`
    (the resolver lists them all). `[ui] default_browser_surfaces` is already folded into the listed modes by the
    resolver, so the core never reads it.

15. Logical routes (`runtime/routes.{h,cc}`, M4). The core reads `browser.internal_scheme`, `routes` and `aliases`
    at start and owns every rule of architecture.md section 8: `bdk_page_handler_register` sets a route's mode before
    the freeze (`config.noBuiltin`, `routeUnknown`, `routeInvalid`, `routeMode`; `config.handlerMissing` at the
    freeze for a native route with no handler), `resolve_navigation` says what a URL becomes (load the internal URL,
    native, blocked with `ERR_BLOCKED_BY_CLIENT` for a disabled route and `ERR_INVALID_URL` for an unknown one, or
    dropped when a web page asks), `display_url` gives the branded form a backend must report, `page_caller_bind`
    makes a bundled page's caller, and `broker_subscribe_as` subscribes it to events it holds (sessions subscribe in
    M7). An alias is shown as its target route. The built-in routes and their Chromium hosts are a table in
    `routes.cc`, checked at the pin; packages/cli lists the same routes. Internal pages are not recorded in history,
    as in Chrome. Page subscriptions take no filter yet.

16. Navigation policy (`runtime/navigation_policy.{h,cc}`, T-SWF-10, M9). The core owns the handler, the pending
    decisions and the rules in `bdk_contents.h`; a backend only calls `ask_navigation_policy` where a navigation
    would send its request (after its start event) and at each server redirect, waits for `done`, and carries
    `policy_redirects` from a redirect decision to the load it starts. `ask_navigation_policy` returns null, and
    the backend goes on at once, when there is no handler or for the start of a load a redirect decision began,
    so with no handler nothing changes. A decision is its own handle type (`T_DECISION`), a `bdk_context_t` that
    `bdk_release` refuses like a call context; it goes stale when it is answered, when the backend withdraws it
    (`withdraw_navigation_policy`: stopped, superseded), when the page is destroyed (`destroy_contents`) and at
    shutdown. The answer runs on a later task. The handler is set with `bdk_navigation_policy_set_handler` at any
    time, like `bdk_log_set_handler`, because Swift's `runtime.navigationPolicy` is a property, not part of the
    configuration; a handler removed before a decision is delivered lets that navigation go on. The twenty-first
    redirect decision in a row is `kTooManyRedirects` (net error -310, the error page). The stub asks before its
    reputation check and its route handling (a native route is asked like any other URL) and simulates server
    redirects with `bdk_stub_script_redirect` (`bdk_harness_script_redirect`, a page hook). The engine (T-SWF-11)
    is expected to be a throttle beside `engine/pages/pages_navigation_throttle.cc`: `WillStartRequest` and
    `WillRedirectRequest` return `DEFER` for the outermost main frame of a page with a `bdk_contents_t`, then
    `Resume`, or `CancelDeferredNavigation(CANCEL_AND_IGNORE)` with the page noting the reason as it does for
    `handledNatively`, or `{CANCEL, ERR_TOO_MANY_REDIRECTS}`; a redirect posts a load as `LoadAsBrowser` does.
    Chromium runs no throttle for a page activation (`NavigationRequest`, "Throttles will already have run the
    first time the page was navigated"), so back/forward cache restores and prerender activations are not
    asked; the throttle must ask for a prerender's own navigation and discard a prerender it may not keep.
    Checked against the source at the pin, not built.

17. Profile snapshots and the downgrade policy (`runtime/profile_versions.{h,cc}`, T-PKG-05, M9). Before the
    backend starts, the core reads `runtime.downgrade` and `runtime.snapshot_on_upgrade`, asks the backend for
    the data directory and the engine version (`Backend::ProfileDataDirectory`, `Backend::EngineVersion`; both
    default to empty, which skips the policy) and applies the rule to that directory with Chromium's own files:
    `Last Version` and `Snapshots/<version>/`, valid when it holds its own `Last Version`. A newer milestone
    snapshots what the last version left (caches and the process singleton are left out; the three newest
    snapshots are kept, Chromium's default retention); an older milestone restores the newest snapshot no newer
    than the engine, and with none fails start with `runtime.downgradeRefused` (`refuse`) or renames the
    directory to `<name>.newer-<version>` beside it and starts empty (`fresh-profile`). A change within a
    milestone does nothing, as in Chromium. Other failures are `runtime.profileMigrationFailed`. The codes are
    strings, not `bdk_ids.h` constants, like `noDataDirectory`. The stub applies it to `user_data_path_override`
    as engine version `1.0.0.0`; the Chromium engine does not supply its directory and version yet (T-PKG-05).
    `bdk_harness_own_data_directory` says whether a harness lets a start name its own data directory; the
    engine's does not (Chromium starts once per process), so `tests_profile_versions.c` runs on the stub only.

18. Robustness (`tests/tests_robustness.c`, T-REL-03). Pages opened, loaded, closed and destroyed many times;
    the runtime shut down with pages open and started again; a renderer crashed and the page reloaded; peak
    resident size bounded. `BDK_ROBUSTNESS_ITERATIONS` sets the rounds: 100 by default in `core:test`,
    50000 in `core:test-release` (about three seconds on the stub), where the memory bound applies. The stub
    treats a load of `chrome://crash` as Chromium does: the page's renderer goes (`reason` `crashed`) and
    nothing commits.

### The UI request router

`runtime/ui_router.cc` (T-UIR-01) is the one place that decides what happens to a request. The backend raises it with
`raise_ui_request(rt, request)` and learns the outcome from `request->on_finish`, called once, on a later task.

| Question | Decision |
|---|---|
| Where are the modes validated? | `bdk_runtime_start` reads `surfaces` before the backend starts. A mode the surface does not allow is `ui.surfaceMode`, an unknown surface `ui.surfaceUnknown`, a value that is not a mode `config.resolvedInvalid`. packages/cli validates the same table from its schema first (`ui.surface_mode`); `units.test.ts` fails when the schema and the table in `ui_router.cc` disagree. The surface table (name, id, allowed modes, granting completions) is the core's source of truth for the runtime, the schema for the CLI. |
| `custom` and no handler | A `custom` surface with no handler fails `bdk_runtime_freeze_registrations` with `config.handlerMissing` (naming every such surface); the runtime stays unfrozen so a handler can be added. configuration.md says "before start"; handlers are registered after `bdk_runtime_start` and before the freeze, so the freeze is where it can be checked. |
| `disabled` | The request is finished as `UiOutcome::kDisabled` (stub and harness: `BDK_STUB_UI_DISABLED`, `BDK_HARNESS_UI_DISABLED`, 6) on a later task, with no handler called and no queue slot taken. The backend denies or cancels and makes the web or extension API report unavailable; `ui_surface_available(rt, surface)` answers that question before a request is raised. A handler registered on a disabled surface is ignored and released at shutdown as usual. |
| `default`, no handler | `kDefaulted`: the backend shows its default presenter. A handler's `BDK_UI_USE_DEFAULT` and `bdk_ui_use_default` give the same outcome. |
| `reject` | `kRejected`: the backend denies or cancels. A handler answer that is not one of the three responses is also `kRejected`, the safe reading. |
| Interstitials (`certificate_errors`, `reputation_warnings`) | Only `go_back` and `proceed` complete them (`ui.completionNotAllowed`). `proceed` needs a gesture and a payload with `"isOverridable":true`; a request that does not say is not overridable (`ui.notOverridable`). `reject` and a dropped request finish as `kCompleted` with `go_back`, so the raiser never has to decide what an unsafe reject means. A withdrawal stays `kWithdrawn`. They cannot be disabled (`ui.surfaceMode`). |
| Subsets | A `grant` on `permissions` may name `kinds` in its arguments; each must be in the request's `kinds` array (`ui.widensRequest`). A request that names no kinds offers nothing to name. No `kinds` argument means everything asked. |
| Queueing | Requests of one surface for one page (`contents`, 0 is its own page) reach the handler one at a time; the next is delivered, on a later task, after the previous one finishes by any route (completed, default, reject, released, withdrawn). A request withdrawn while waiting is never delivered. The default presenter path finishes at once, so it never holds the queue; the engine queues its own presenters. |
| Notices | A request with no `on_finish` (a notice to a custom presenter, such as leaving fullscreen) is delivered at once and never queued. |
| Dropped | An uncompleted request released by the application finishes `kCancelled`, never granted. |
| Gesture kind | The core checks that the gesture is an `NSEVENT`. That it is recent and went to a window of this application is the backend's: while a completion's `on_finish` runs, `Request::gesture` holds the borrowed event so the engine can check it (the Chromium engine does: mouse-up or key-down, under 10 s old, delivered to one of the application's windows; anything less grants nothing). `withdraw_ui_request` withdraws one request, for a backend whose own UI went away. |
| WebAuthn (`webauthn`, T-UIR-09) | One request per step of a ceremony (ui-requests.md 5.18). Completions are `accept`, `grant` and `deny` only (`ui.completionNotAllowed`). `grant` names one of the payload's `accounts` by `id`, and needs the gesture like every grant; `accept` with an `account` is refused (`ui.completionNotAllowed`). `accept` names one of the payload's `mechanisms`, or gives a `pin` where the payload has `pin`, or asks for `{"action":"otherMechanisms"}` where it has `otherMechanismsLabel`, or, with none of these, presses the button of a payload with `acceptLabel`; anything else is `ui.widensRequest`. A non-string `account`, `mechanism`, `pin` or `action` is `BDK_ERR_INVALID_ARGUMENT`. |
| Autofill suggestions (`autofill_suggestions`, T-UIR-15) | Completed by `grant` or `accept` (both grant, so both need the gesture) naming one of the payload's `suggestions` by `id` (`ui.widensRequest` otherwise), or `deny`; anything else is `ui.completionNotAllowed`. `bdk_ui_update` takes only `{"preview": id}` for an offered suggestion or `{"preview": null}` (`ui.widensRequest` otherwise); the core checks every other surface's updates only for being an object. |
| Exclusive access (`exclusive_access`, T-UIR-15) | A notice: `accept` only (`ui.completionNotAllowed`). A rejected or dropped one finishes `kDefaulted`, because the exit instruction must be shown (ui-requests.md 5.12). |
| Page health (`page_health`, surface 33, T-UIR-15) | Two kinds by the payload's `kind` (ui-requests.md 5.19). `unresponsive`: `accept` with `{"action":"wait"}` or `{"action":"terminate"}`, or `deny` (waits). `blockedPopups`: `grant` with exactly one of `{"popup": id}` (one of the payload's `popups`) and `{"always": true}`, or `deny`. Other answers are `ui.widensRequest` or `ui.completionNotAllowed`; a non-string `action` or `popup`, or a non-literal `always`, is `BDK_ERR_INVALID_ARGUMENT`. It cannot be disabled. The stub raises both itself (`bdk_stub_page_unresponsive`, `bdk_stub_page_responsive`, `bdk_stub_block_popup`); the harness's `bdk_harness_page_*` and `bdk_harness_block_popup` reach them, and answer `BDK_ERR_INVALID_ARGUMENT` on a runtime without them, which the tests skip. |
| Extension surfaces | See "Extensions" below. |

### Extensions (M5)

Specification: [extensions.md](../../../docs/planning/sdk/extensions.md). Tasks T-EXT-02 to T-EXT-09 and T-SWF-08. Decided
where it was silent (2026-11-20):

1. Surfaces. Eight surfaces follow the ui-requests.md table: `EXTENSION_ACTIONS` 25, `EXTENSION_POPUPS` 26,
   `EXTENSION_SIDE_PANELS` 27, `EXTENSION_CONTEXT_MENUS` 28, `EXTENSION_OPTIONS_PAGES` 29, `EXTENSION_DEVTOOLS` 30,
   `EXTENSION_INSTALLS` 31, `EXTENSION_UNINSTALLS` 32. The first six are configured under `[extensions.ui]` by the
   keys `actions`, `popups`, `side_panels`, `context_menus`, `options_pages`, `devtools` (the core's `kExtensionKeys`;
   in the router's table they are named `extension_<key>` so they cannot collide with `[ui.surfaces]`, and those names
   in `surfaces` are `ui.surfaceUnknown`). Install and uninstall prompts have no key and allow `default` and `custom`
   only, so they cannot be disabled. `[extensions] enabled = false` makes all eight unavailable.
2. Data-only surfaces. `actions` (state is data, extensions.md 4.1), `context_menus` (items arrive inside the
   `context_menus` request), `options_pages` (a `new_windows` request) and `devtools` (panels load in the front end)
   raise no request of their own, so `custom` needs no handler at the freeze. Their mode still decides what BDK shows
   and what is refused at install.
3. Open requests. A popup and a side panel are requests that stay open while their view is on screen, as the find bar
   does. The application may complete them with `accept` `{"action":"close"}` (it closed its container) or `deny`
   (it will not show this one); a withdrawal means the extension or BDK closed the view (blur, `window.close()`,
   navigation, another panel). `bdk_ui_update` sends the application's side while it is open (`{"placed":true}`,
   `{"side":"leading"|"trailing"}`), and `bdk_ui_set_update_handler` receives the runtime's
   (`{"preferredSize":{"width","height"}}` for a popup, `{"visible":bool}` for a page-scoped panel). Both are
   delivered later and in order; nothing is delivered after the request finishes, and the update handler is released
   then. The secure view is the runtime's and lives as long as the request: when the request finishes, the runtime
   removes the view from the application's container and may destroy it, so the application does not touch it
   afterwards. With no handler the default presenter shows the view (a transient panel at the extensions anchor; a panel at
   the trailing edge of the page's window).
4. Payloads. `extension_popups`: `extensionId`, `extensionName`, `url`, `trigger` (`userClick`, `openPopupAPI`,
   `command`), `preferredSize`; `secure_view` is the popup's native view. `extension_side_panels`: `extensionId`,
   `title`, `icon` (a `data:` URL or absent), `url`, `scope` (`window` or `page`), `contents` (the page, for `page`);
   `secure_view` is the panel's view. `extension_installs`: `kind` (`install`, `permissionsIncrease`,
   `optionalPermissions`, `reEnable`), `extensionId` (absent before the package is unpacked), `extensionName`, `icon`,
   `source` (an `ExtensionSourceKind`), `warnings` and `hostWarnings` (Chromium's strings, in its order),
   `compatibility` (a `CompatibilityReport`). `extension_uninstalls`: `extensionId`, `extensionName`, `requestedBy`
   (`{"kind":"user"}` or `{"kind":"extension","extensionId"}`). An install or uninstall is completed by `grant`
   (which needs the gesture, as on every surface) or `deny`; anything else, a dropped request and the default with
   no presenter able to ask, refuse the install with `extension.cancelled`. There is no way to pre-approve.
   An extension's context-menu items are in the `context_menus` request's `items` with `"source":"extension"` and
   `extensionId`, under one submenu per extension when it adds more than one; an options page is a `new_windows`
   request of kind `newPage` whose payload has `initiator` `{"extensionId"}`.
5. Commands. `interface/extensions.json` defines the `extensions.*` commands and events of extensions.md section 3,
   plus `extensions.actions` (the action states for the page named in the call's options, or the profile's defaults
   with none) and the event `extensions.actionChanged`. Installing and uninstalling require a gesture from a page
   and always raise the prompt, from any caller. Errors are in the domain `extension`: `notFound`,
   `sourceNotAllowed` (naming the `bdk.toml` key), `incompatible` (naming the first blocking finding), `cancelled`,
   `surfaceDisabled`, `manifestInvalid`, `notUnpacked` (reload), `privateNotAllowed` (`allow_in_private = "never"`).
6. Policy before Chromium. `runtime/extension_policy.{h,cc}` decides, for the stub and the engine alike, whether a
   source is allowed (`source_refusal`: `unpacked`, `crx`, `update_urls` patterns with `*`, named `stores`) and what a
   manifest needs (`inspect_manifest`: manifest version and `minimum_chrome_version` against the engine's
   `EngineFacts`, permissions for APIs BDK lacks, surfaces the configuration disables, `update_url` against
   `update_urls`). A blocking finding refuses the install or update unless `install_policy = "allow_all"`, which
   shows the findings in the prompt instead (`install_refused`). Since T-EXT-14 (2026-11-25) it also reads
   `developer_mode` (`"off"` refuses unpacked directories in `source_refusal`; `"user"` behaves as `"on"` until the
   setting `extensions.developerMode` exists), `[extensions.install] blocklist` (`blocklisted`: refused from every
   source with `extension.blocklisted`, which `allow_all` does not override) and `[extensions.bundled]` (each
   `{path, required}`, with `path` relative to the bundle's `Contents/Resources`, `bundled_directory`). The stub loads
   bundled extensions, with no prompt, the first time a persistent profile's extensions are asked for, refuses to
   uninstall them and to disable a required one (`extension.notRemovable`), and reads their manifest from a package
   registered as `bundled` with the declared path, or from the bundle.
7. Gestures from the host. `bdk_extension_action_activate`, `bdk_extension_command_perform` and
   `bdk_extension_menu_present` (`bdk_extensions.h`) take the confirming event, like a granting completion: the core
   checks its kind (`ui.gestureRequired`), the engine that it is recent and went to one of the application's windows.
   They are functions, not commands, because a command from the host has no gesture to give. An extension that is
   not installed and enabled is `extension.notFound` before the engine judges the event. Activation is
   synchronous, as Chromium's `ExtensionActionRunner::RunAction` is; a popup or panel follows as a request.
8. Windows (T-SWF-08). `bdk_window_register` takes `BDK_WINDOW_KIND_NORMAL` or `_POPUP` (anything else is
   `BDK_ERR_INVALID_ARGUMENT`); the engine creates that window's host contexts with the matching `Browser` type, so
   `windows.get` reports it. `bdk_window_set_anchor_provider` is asked for `BDK_ANCHOR_*` rectangles when a default
   presenter attaches; `bdk_window_set_request_handler` receives window requests from extensions and pages
   (`BDK_UI_USE_DEFAULT` or no handler: BDK applies it to the native window). Both contexts are released at
   replacement, unregistration or shutdown. `bdk_window_for_contents`, `bdk_window_contents`,
   `bdk_window_active_contents` and `bdk_window_extension_id` answer from the backend
   (`Backend::WindowForContents`, `WindowContents`, `WindowExtensionId`); a backend with no attachments answers 0,
   an empty list and -1.

### Named in c-abi.md and not declared (nothing states their shape)

`bdk_contents_prune`, `_perform_command`, `_can_perform_command`, `_set_host_context`;
`bdk_crash_reports_*`, `bdk_netlog_*`. `bdk_automation_subscribe_audit` is not declared on purpose: the audit trail
is the built-in events of namespace `automation` ("Automation" below). Interface definition validation and generated identifiers are T-ABI-03.

## Interface definitions and identifiers

The format is c-abi.md section 7. The generator, validator and tests are in `packages/js/codegen`; nothing in this
chain needs Chromium.

| File in `interface/` | Role |
|---|---|
| `<namespace>.json` | One namespace: `types`, `commands`, `events` and, for the ABI's own identifier families, `constants`. The file name must equal `namespace` |
| `policy.json` | `neverForPages` and `neverForSessions`: capability names or `namespace.*` patterns. A command whose `callers` include `page` cannot use one on the first list; one that includes `session` cannot use one on the second |
| `ids.json` | The registry: every identifier ever assigned, retired ones included. Generated; keep it under version control |

`constants` is the section for identifiers that are not commands (c-abi.md is silent on it). Each entry is a group:
`{ "group", "kind": "string" | "enum" | "flags", "prefix": "BDK_X_", "comment", "values": { "NAME": value } }`, which
becomes `#define BDK_X_NAME <value>` (strings quoted, enums `Nu`, flags `0xNu` with a single bit each). Values are
stated in the definition. The group named `surfaces` is also the set of surface names a command's `raises` may use, in
lower case. Commands and events get `BDK_COMMAND_<NAME>` and `BDK_EVENT_<NAME>` (`history.deleteAll` becomes
`HISTORY_DELETE_ALL`) with an identifier the registry assigns: the next integer above every identifier ever assigned,
starting at 1. The M3 namespaces are defined (runtime, profiles, settings, history, bookmarks, downloads, permissions,
cookies, privacy, passwords, autofill, system); the rest of the services.md catalogue arrives with its milestone.

Rules the registry enforces: a new name gets a new identifier; a removed name stays in `ids.json` marked `retired` and
its identifier is never given to another name; a retired name cannot be defined again; a constant whose value differs
from the registry is a renumbering and fails; so does a new constant that takes a value already in the registry.

To add an identifier: edit the definition, run `pnpm exec nx run js-codegen:codegen` (rewrites `ids.json`,
`bdk_ids.h` and `generated/bdk_command_table.inc`, the tables the broker reads), run `cmake -DMODE=generate -P packages/core/cmake/abi-hash.cmake` (the definitions are hashed), and
commit all of it except the `@bdk/internal` modules, which are git-ignored build outputs (amendments.md 2.14). `pnpm exec nx run js-codegen:codegen-check` fails when the committed outputs are stale; `js-codegen:test`
and the `abi-linux` job run the same check. To remove one, delete it from the definition and regenerate. Do not edit
`ids.json` by hand except to resolve a merge: two branches that add identifiers conflict there, and the fix is to keep
both sides and re-run the generator, which fails if two names ended up with one identifier.

## The broker

Specification: services.md sections 1 to 5 and architecture.md section 9. Task T-SVC-01. The core does every check
once, in `runtime/broker.cc`; a backend implements the work behind `Backend::ExecuteCommand`.

Decisions taken where the specification was silent (2026-11-19):

1. One pipeline for every command. A call is looked up by name or identifier (built-in commands, from the generated
   tables, then registered `app.` commands by name), and then checked in the order of services.md section 2: caller
   identity, capability, scope, gesture or authentication, argument validation, the service. A command that does not
   exist is `broker.unknownCommand` before anything else. Each refusal delivers its result later, never from the
   call that caused it, and a refused call never reaches the service.
2. Callers. The host (Swift, through the C ABI) has full authority: no capability, scope or gesture check. An internal
   page and an automation session are `Caller` objects (`broker_internal.h`) with a capability set, and a scope: a
   page's profile and no pages; a session's profiles and pages. Their transports arrive in M4 and M7; until then the
   stub's test hooks (`stub/bdk_stub_broker.h`, through `tests/harness_broker.h`) are the only way to make one. Calls
   from one caller start in the order sent.
3. Capability. A built-in command lists the callers it admits (`callers`) and the capability it needs; either missing is
   `capability.denied`. An application command needs the capability named like itself (`app.tabs.open`) or a prefix
   pattern (`app.tabs.*`, which covers every name under the prefix and nothing else). A grant is checked when the
   caller is created: no wildcard outside `app.<prefix>.*`, nothing on the policy's never-for-pages or
   never-for-sessions list. Capability names the interface does not define are accepted, because the capability table
   is generated from the commands and events defined so far. `runtime.caller` needs none.
4. Scope. A page may name only its own profile (any other is `capability.outOfScope`) and no page. A session may name
   only profiles and pages in its scope, and with exactly one profile in scope that profile is the default. A
   command with profile `required` and no profile resolved is `broker.invalidArguments`; one with `none` is not given
   a profile. A handle that went stale between the call and its dispatch is `profile.notFound` or `contents.stale`.
5. Gesture and authentication. A page's call to a command with `requires: ["gesture"]` needs the gesture the
   transport reports (`capability.gestureRequired`); a session holds the capability and needs no gesture; the host
   never does. Authentication is asked of the operating system whoever calls, through `Backend::Authenticate`, before
   the arguments are checked, and refusing it is `capability.reauthenticationFailed`. `authenticateWhen` names an
   argument that makes the call need authentication only when it is present (`passwords.update` with a new password,
   services.md 5.9). A call cancelled while the user is being asked stops there.
6. Arguments. They are one JSON object that matches the command's `args` exactly: every non-optional field present
   and of its type, no unknown field (also inside structs), `null` only for an optional field, enums by name.
   `int` is an integer written without fraction or exponent, at most 2^53 in magnitude; `timestamp` is an `int` of
   milliseconds since 1970-01-01T00:00:00Z; `bytes` is a base64 string; `json` is any value. A paginated command's
   `limit` is at least 1. A failure is `broker.invalidArguments` and names the field. Application commands are checked
   only for being JSON.
7. Limits. Arguments and results are each at most 4 MiB (4194304 bytes) of JSON; more is `broker.payloadTooLarge`
   (checked after the capability, on the arguments; on the result when the service answers). Binary data is outside
   the JSON value and not counted. A command with a `void` result answers JSON `null`.
8. Cancellation. `bdk_call_cancel`, a revoked caller (`broker_revoke_caller`) and runtime shutdown each cancel the
   call: `broker.cancelled` is delivered, and the service is told to stop through the `CommandControl` it was given
   (`OnCancel`, `cancelled()`). Whatever the service answers afterwards is ignored. A call cancelled before it starts
   never reaches the service. Revoking a caller also refuses its later calls (`capability.denied`).
9. Progress. A command marked `progress` may report fraction and message through `CommandControl::Progress`
   (clamped to 0 to 1); a command not marked ignores it. Progress is delivered before the result, in order.
10. Streams. The `stream` mark is carried in the definition; the ABI has one result per call, so how partial results
    cross it is not settled. No M3 command streams.
11. The service. `Backend::ExecuteCommand(Runtime&, shared_ptr<BuiltinCall>)` receives the command, its validated
    arguments, the resolved profile and page, and the caller; it answers through `call->control` exactly once, on the UI
    thread, now or later. The default answers `broker.notImplemented`, so the engine need not change until it
    implements a command. `Backend::Authenticate` defaults to refusing.
12. The stub. `stub/stub_services.cc` implements, in memory and per profile id: `runtime.info`, `profiles.list` and
    `current`, `settings.get`, `getMany`, `set`, `reset`, history (`add`, `search` with an offset cursor, `visits`,
    `deleteUrls`, `deleteRange`, `deleteAll`), bookmarks (`roots`, `get`, `children`, `create`, `createFolder`, `update`, `move`, `remove`,
    `search`, `forUrl`), `downloads.list` and `get` (empty), `permissions.get`, `set`, `reset`, `getDefault`,
    `setDefault`, `cookies.sites` (empty), `privacy.clear` (clears history; reports progress), `passwords.add`, `list`,
    `forOrigin`, `reveal`, `update`, `remove`, `autofill.addresses.add`, `list`, `remove`, `search.engines`, `addEngine`, `updateEngine`, `removeEngine`, `setDefault`, `classify` (a stand-in for
    Chromium's classifier), `suggestions` (stub history and bookmarks only), `deleteSuggestion`, and
    `system.defaultBrowser.status`. Everything else answers `broker.notImplemented`. It is the reference for result
    shapes, not for Chromium's behaviour. `runtime.caller` is answered by the core itself.
13. The conformance tests that need an internal caller or the stub's services check `bdk_harness_broker_hooks()` and are
    skipped by a runtime that answers 0 (`tests/harness_broker_unsupported.c`, linked by the engine harness). The
    tests that need neither (unknown commands, application commands, host calls) run everywhere.
14. Error codes added to the `codes` constants: `denied`, `outOfScope`, `reauthenticationFailed`, `payloadTooLarge`,
    `notImplemented` (the first four are in the swift-api.md table; `notImplemented` is new, domain `broker`).
    `gestureRequired` is reused in domain `capability`.

15. A call that names a page and no profile addresses the page's profile (`translation.state`). A session must
    have that profile in its scope (`capability.outOfScope`).

Not done here: the host's and pages' event subscriptions are not checked by capability (sessions' are, see
"Automation"), the `favicons` and `extensions` namespaces (`search` arrived with M4, `translation` with M6), and the TypeScript declarations
generated from the definitions (T-JS-02).

## Automation

Specification: architecture.md 9.3, services.md 5.15 to 5.17, javascript-api.md 5, c-abi.md 9. Tasks T-AUT-01 to
T-AUT-05, T-SVC-09. The core does everything that needs no Chromium (`runtime/automation.cc`,
`runtime/contents_commands.cc`, `runtime/local_socket_posix.cc`); a backend runs scripts and renderer work. Decisions
taken where the specification was silent (2026-11-20):

1. A session (`bdk_session_create`) is a broker `Caller` of kind session with the application's capabilities (checked
   by `grant_error`) and scope: `profiles` is `"all"` or a list of open profile ids, resolved to handles at creation
   (a profile closed and reopened leaves the scope); `contents` is `"all"` (every page of those profiles),
   `"created"` (default: pages the session creates) or `"explicit"` (pages granted with
   `bdk_session_grant_contents`; pages it creates are added too). A page is in scope only in a profile that is
   (`check_scope`). With one profile in scope it is the default profile of a call.
2. Revocation (`bdk_session_revoke`, or `bdk_release` of the session) revokes the caller (its calls end with
   `broker.cancelled`), ends its tasks with `automation.sessionRevoked`, closes its external connections with a
   `closed` frame and removes its user scripts. At shutdown every session is revoked without the audit event.
3. One wire protocol for scripts and external tools: one JSON object per frame, field `type`. Client to BDK: `hello
   {protocol, token, client?}` (external only, first), `call {id, command, args?, profile?, contents?}`, `cancel {id}`,
   `subscribe {id, event, profile?, contents?}`, `unsubscribe {id}`, and from a script only `progress {fraction,
   message}`, `log {level, message}`, `done {result}`, `failed {error: {message, stack?}}`. BDK to client: `welcome
   {protocol, session: {name, capabilities}}`, `start {protocol, task, script, arguments, session}` (a script's first
   frame), `result {id, value, binary?}` (binary in base64), `error {id, error: {domain, code, message}}`, `progress
   {id, fraction, message}`, `event {id, name, payload}`, `abort {error}` (the task is being torn down) and `closed
   {error}` (sent before BDK closes a connection). Ids are positive integers unique per connection across calls and
   subscriptions; a reused id, an unknown `type` or a frame that is not a JSON object closes the connection with
   `broker.invalidArguments`. Pages are the decimal string of their handle, profiles their id. Protocol version 1.
4. Tasks: `bdk_session_run` checks `[automation] enabled` (`automation.disabled`) and that the script is a key of
   the resolved `automation.scripts` (`automation.scriptNotFound`), then, on a later task, calls
   `Backend::StartTask` with a `TaskPort` and sends the `start` frame. Events reach `bdk_task_subscribe`:
   PROGRESS, LOG, COMMAND (one per call, when it ends) and FINISHED last (`{"result"}` or `{"error"}`); a
   subscription after the end gets FINISHED alone. `bdk_task_cancel` and releasing a running task's handle send
   `abort` and finish it with `broker.cancelled`; a `failed` frame or a host that could not load the script is
   `automation.scriptFailed`; a host crash `automation.hostGone`. `Backend::EndTask` tears the script down after any
   end. A `done` result is capped at 4 MiB like any result. Each task carries `[automation] cpu_budget_ms` (resolved
   `automation.cpu_budget_ms`, default 60000, a whole number from 1 to 3600000, else `config.resolvedInvalid`) in
   `TaskPort::cpu_budget_ms`; a host that stops the script at it calls `TaskPort::BudgetExceeded` and the task
   finishes with `automation.budgetExceeded` (M8, T-AUT-07). A `failed` frame is always `scriptFailed`, so a script
   cannot claim the code. The engine's host counts the CPU time of each entry into the script (its start, a frame,
   a timer) with `base::ThreadTicks` and arms a watchdog thread with the budget left. When that much wall-clock time
   has passed (the earliest the budget can be used up), the watchdog interrupts the run, reads the host thread's CPU
   time inside the interrupt, and terminates the run only if it has used the budget; otherwise it waits again for
   what is left. The stub's
   host reports the budget (`bdk_stub_script_budget`) and can be told the script used it
   (`bdk_stub_script_exhaust_budget`).
5. Audit: every call a session makes emits `automation.commandInvoked {session, task?, command, profile?, contents?,
   error?}` when it ends, refused calls included (also those the connection refuses before the broker: arguments
   that are not an object, an unknown profile, a stale page); `automation.sessionCreated` and `automation.sessionRevoked` frame a
   session's life. They are built-in events with capability `automation.audit`, so the host subscribes with
   `bdk_subscribe_named` and an auditing session can be granted them; no separate audit function exists.
6. A session subscribes to built-in events and declared `app.` events. The event's capability is checked when it
   subscribes (an `app.` event needs the capability named like it), and each event is delivered only when its
   profile and page are in the session's scope. Events of a page carry it: backends use `emit_builtin_event_for`
   (scope by page) or `emit_builtin_event_to` (one caller only: `debug.event` goes to whoever attached).
7. The external endpoint (`bdk_automation_endpoint_open`) needs `[automation.external] enabled`
   (`automation.disabled`). It is a Unix-domain stream socket, 0600, in a new 0700 directory (`mkdtemp`) under the
   temporary directory or a directory the application names (it must be the user's and not writable by others);
   every connection's peer must have the user's uid (`getpeereid`, `SO_PEERCRED`). Frames are a 4-byte big-endian
   length and the JSON, at most 32 MiB. Nothing listens on a network port; the conformance suite checks that no
   socket of the process listens on anything but a Unix-domain address. The first frame must be `hello`; the
   application gets it through its callback with a `bdk_context_t` and answers once, now or later, with
   `bdk_automation_hello_accept` (a live session) or `_refuse` (a `closed` frame with `capability.denied` and its
   message). At most 64 connections at a time. A transport thread (`std::thread`, `poll`) moves bytes; everything
   else is on the UI thread. `local_socket_posix.cc` is the core's one platform file; Windows replaces it.

   A client that does not read keeps its connection (T-AUT-07). The transport queues each connection's frames
   (under the one mutex the UI thread and the transport thread share) and takes them from the queue 256 KiB at a
   time to write. Once more than 4 MiB (the soft limit) is waiting, a *state event* replaces the one still queued
   for the same subscription, event name, profile and page, and is queued after everything queued so far: the
   frames that are kept never change their relative order, and a coalesced event is never delivered before a frame
   sent earlier. State events are those whose payload is the whole current state of one page, so only the latest
   matters: `contents.loadingChanged` and `contents.titleChanged` (the list is `is_state_event` in `automation.cc`;
   an event joins it only if its payload says everything about its page and the core passes that page to
   `DeliverEvent`). Everything else is history, a change or a reply and is never dropped or reordered: results,
   errors, progress, `closed`, and events such as `contents.navigated`, `downloads.*`, `automation.commandInvoked`.
   Frames the transport has already taken to write are not taken back. A connection that would have more than
   64 MiB (the hard limit) waiting, state events included, is closed (`automation.overloaded`, with no `closed`
   frame, since it cannot be written). The conformance suite lowers both limits through the harness
   (`bdk_harness_endpoint_limits`) so it need not queue megabytes.
8. The core answers, on the Backend's page operations so every runtime behaves the same, `contents.list`, `get`,
   `create` (the page belongs to the application, which gets `contents.created` with `createdBy` and destroys it like
   any other), `close`, `navigate` (resolves `{url, errorPage}` at commit, or at load with `waitUntil: "finished"`; a
   failed navigation is `navigation.<code>`), `reload`, `stop`, `goBack`, `goForward` (no entry:
   `broker.invalidArguments`), `waitFor` with `load` and `url` (a `*` glob) only, deadline `timeoutMs` (default 30 s,
   `broker.timeout`, through `Backend::PostDelayed`), `setMuted`, `setZoom` and `requestAttention` (the event
   `contents.attentionRequested`; BDK moves no view). Every other `contents.*` command goes to the backend. A command
   that needs a page and names none is `broker.invalidArguments`. Page events become `contents.navigated`,
   `loadingChanged`, `titleChanged`, `closed` and `crashed`; a UI request raised for a page is
   `contents.dialogOpened`, so a script knows it waits on the user (it cannot answer requests).
9. `scripts.*`: the core keeps a profile's user scripts (in memory, for the runtime's life), in registration order,
   and gives all of them to `Backend::UserScriptsChanged` after each change. A script needs an id, a match pattern
   and JavaScript or CSS; `runAt` defaults to `documentIdle` and `world` to an isolated world named like the id. A
   session can replace or remove only its own scripts (`capability.denied`); the host any.
10. `debug.*`: `[devtools] protocol = false` refuses `debug.attach` and `debug.send` to every caller
    (`devtools.protocolDisabled`); sessions are refused the methods of the domains Browser, Target, SystemInfo,
    Tethering, Storage, IO, ServiceWorker and BackgroundService (`capability.denied`). `devtools.open` is refused when
    `[devtools] frontend = "disabled"` (`devtools.disabled`).
11. Binary results (commands marked `binary`): `CommandControl::ResolveBinary` delivers the bytes as the result
    callback's `bdk_buffer_t`, borrowed for the callback, outside the 4 MiB JSON limit.
12. The stub. `Backend::PostDelayed` has no clock: its time moves only when nothing else is queued (`bdk_stub_pump`
    then runs the earliest delayed task). Its automation host runs scripts the test defines as callbacks
    (`stub/bdk_stub_automation.h`, through `tests/harness_automation.h`; a runtime with a real host links
    `tests/harness_automation_unsupported.c` and those tests are skipped). `contents.frames` and `text` answer the main
    frame and `""`, `screenshot` and `pdf` fixed minimal PNG and PDF bytes; the rest of the renderer's commands answer
    `broker.notImplemented`. `bdk_stub_automation_server` (tests/automation_server.c) is the stub with its endpoint
    open, for clients in other languages.

## Providers

Specification: architecture.md 11 and 11.1, configuration.md 3.8, swift-api.md 11.2, c-abi.md 9. Tasks T-PRV-01
(contracts) and T-PRV-09 (choosing a provider). The core does everything here once (`runtime/providers.cc`,
`translation.cc`, `reputation_lists.cc`, `providers_internal.h`), so the stub and the engine behave the same.

Choosing a provider. `bdk_runtime_start` reads `services` from the resolved configuration before the backend starts:

| Service | Built in this runtime | Start error for a name BDK documents but has not built | `endpoint` |
|---|---|---|---|
| `translation` | `default`, `app` | `apple`, `deepl`, `microsoft`, `google`: `config.providerUnavailable` (T-PRV-08) | `config.endpointUnsupported` |
| `reputation` | `default`, `app`, `google` | `web-risk`: `config.providerUnavailable` | Only with `default`: where the lists are fetched (https, or http on the loopback address) |
| `geolocation` | `system`, `app`, `google` | | `config.endpointUnsupported`: the network provider's host is a constant (T-PRV-14) |
| `credentials`, `leak_check` | Only their defaults (`chromium`, `none`); S14 and S16 are proposals | Anything else: `config.providerUnavailable` | |
| `webauthn` | Not a provider table: `phones`, a boolean, false when absent (T-UIR-12). `webauthn_phones_allowed` (`providers_internal.h`) gives it to the backend | | |

An unknown name or a value of the wrong kind is `config.resolvedInvalid` (the CLI's `services.provider_unknown` comes
first). The Google key is `google_api_key` in `bdk_start_config_t`, not `bdk_provider_set_key`, because Chromium reads
its key before it starts and allows setting it only once (`InitializeApiKeyCacheInstance`, `google_api_keys.cc`). A
service with `provider = "google"` and no key is `config.keyMissing`; a key with no such service is `config.keyUnused`,
so a key never reaches Chromium for services the application did not configure that way (architecture.md 16,
invariant 9). `bdk_provider_set_key` fails with `config.keyUnused`: no named provider in this runtime takes a key.

Registering. `bdk_provider_register(runtime, service, vtable, ctx, release, err)` works between start and the freeze
for a service configured `app` (otherwise `config.providerNotApp`); the freeze fails with `config.providerMissing`
while one has none. The vtable starts with `descriptor_json`: `id`, `name`, `privacy` (`onDevice` or `remote`) and,
for a remote provider, `host` and an https `policyUrl` (`config.descriptorInvalid`, swift-api.md 11.2). Required
operations: translation `supported_pairs` and `translate`; reputation `check_url`; geolocation `start`. An unknown
service (also `credentials` and `leak_check`), a null vtable or a missing required operation is
`BDK_ERR_INVALID_ARGUMENT`.

Calls. The core calls an operation with a `bdk_context_t` and a JSON input, on the UI thread and never from the call
that caused it. The provider answers once with `bdk_provider_resolve` or `bdk_provider_reject`, reports with
`bdk_provider_progress` (prepare) and `bdk_provider_report` (geolocation positions). Reports sent before the answer
are delivered before it. The optional `cancel` operation is called, from a task of its own, when BDK no longer wants
the answer (the page navigated, the translation was reverted, the watch stopped, the runtime shuts down); afterwards
the context is stale, `bdk_provider_is_cancelled` answers 1 and any answer is ignored. At shutdown every outstanding
call is cancelled before the provider's release callback runs. c-abi.md's geolocation `stop` is this `cancel`.

The contract, checked by the core on every answer (`provider.contractViolation` names the problem):

| Operation | Answer |
|---|---|
| `supported_pairs` | `{"pairs":[{"from","to","installed"}]}`: two different language codes (two or three letters, then `-` subtags); `installed` defaults to true |
| `translate` | `{"texts":[...]}` with as many strings as were sent |
| `detect` | `null`, or `{"language","reliable"}` |
| `check_url` | `{"verdict"}`: `safe`, `unknown`, `malware`, `phishing`, `unwantedSoftware` |
| `check_download` | `{"verdict"}`: `safe`, `unknown`, `malware`, `unwantedSoftware`, `dangerousDownload` |
| `start` reports | `{"latitude","longitude","accuracy"}` in range, optional `altitude`, `altitudeAccuracy`, `heading`, `speed`, `timestamp`; or `{"error":"permissionDenied" \| "positionUnavailable" \| "timeout","message"}`. A malformed report ends the watch, so a page is never given a wrong place |

A rejection keeps its code in the `provider` domain; one in another domain becomes `provider.remoteError` with the
original in the message. A backend's own provider (the default translation engine) answers through the same checks
(`translation_call`).

Translation. `translation_job_start` sends a page's text segments: the provider's pairs first
(`provider.unsupportedPair`; languages match on the primary subtag), `prepare` for a pair that is not installed, then
batches of at most 64 segments and 16 KiB, one at a time; blank segments are not sent, and a segment over 16 KiB goes as
pieces that fit (split at a space, else at a character boundary) whose answers are joined. The page's state
(`translation_set_state`) is reported as a `BDK_CONTENTS_EVENT_TRANSLATION` event (detail: the `TranslationState` JSON)
and as `translation.stateChanged` (`page` is the contents handle as a decimal string, provisional until M7's page
identifiers). `translation_decide`: no offer for an undetermined language or the target; a language on the always list
is translated without asking; otherwise an offer unless offers are off, the language is on the never list or the site
is. A disabled `translation` surface turns the service off: every `translation.*` command fails with
`provider.unavailable`. The request (ui-requests.md 5.10) is raised for an offer and by `translation.present`; its
payload is `phase` (`offer`, `translating`, `translated`, `failed`), `detected`, `target`, `languages`, `provider`. It
is the offer, not a request that moves through the phases: later phases are the events. Completions (none grants):
`translate` (`from`, `to`), `revert`, `always` and `never` (`language`, default the detected one), `neverForSite`,
`dismiss`.

Reputation lists (decision D2, T-PRV-05). The default provider's format, parsed by `reputation_list_parse`:

```json
{ "format": "bdk-reputation-list", "version": 1, "sequence": 42, "generated": 1763600000000,
  "expires": 1763686400000, "source": "…",
  "entries": [ { "expression": "evil.example/", "verdict": "phishing" } ] }
```

An expression is a lower-case host and a path, as Safe Browsing forms them: a URL is looked up by up to five host
suffixes (the host, then the last five components successively shortened, not the bare top-level domain; an IP
literal only as itself) times up to six paths (the path with its query, without it, `/` and up to three prefixes
ending in `/`). Verdicts are `malware`, `phishing` and `unwantedSoftware`. A list replaces the current one only with a
higher `sequence`; an expired list answers `unknown` for everything, which blocks nothing. The backend that fetches a
list checks its signature before parsing it. The warning's payload (`reputation_warning_payload`) carries the
provider's descriptor; for the default lists it says "reduced protection" (architecture.md 11).

The stub. `stub/stub_providers.cc` runs the translation commands over page text the test sets
(`stub/bdk_stub_providers.h`, through `tests/harness_providers.h`), checks each http(s) navigation with the reputation
provider before it commits (a blocked one fails with `{"code":"blocked"}` and `net::ERR_BLOCKED_BY_CLIENT`), and
starts geolocation watches. Its default translation provider is unavailable, as the engine's is until the engine of
T-PRV-02 lands. Provider tests that need those hooks are skipped by a runtime without them (the engine runs the same
behaviour on real pages, `tests/tests_engine_providers.c`).

## The ABI hash

`bdk_abi_hash()` returns a lower-case hex SHA-256 of the public interface.

Input: every `include/bdk/*.h` and `interface/*.json`, in byte-wise path order. For each file the text is normalised:
CRLF and CR become LF; in headers `/* ... */` comments are removed; every whitespace run becomes one space; leading
and trailing whitespace is dropped. The digest covers `"<path relative to packages/core>\n<normalised text>\n"` for
each file in order. Reformatting or editing a comment leaves the hash alone; changing a declaration, macro, type,
header name or interface definition changes it. Do not use `//` comments in headers (they are not stripped).

Commands (CMake 3.20 or newer, run from anywhere):

```sh
cmake -P packages/core/cmake/abi-hash.cmake                   # print the hash to stdout
cmake -DMODE=generate -P packages/core/cmake/abi-hash.cmake   # rewrite generated/bdk_abi_hash.h and abi-hash.txt
cmake -DMODE=check    -P packages/core/cmake/abi-hash.cmake   # non-zero exit if the recorded hash is stale
```

`generated/bdk_abi_hash.h` (`#define BDK_ABI_HASH "..."`) is committed and is what `bdk_abi_hash()` returns in the
stub; a real runtime includes the same header. `generated/abi-hash.txt` holds the bare hash for tooling that does
not want to run CMake. The `check` mode runs in every build (target `bdk_abi_hash_check`) and as ctest
`abi_hash_is_current`. After any header change run `generate` and commit the result (`cmake --build <dir> --target
abi_hash_update` does the same).

## Running the suite against another runtime

Implement `tests/harness.h` for the runtime (pump the UI thread, run a function on another thread, script a
navigation, raise and withdraw a UI request, simulate a key) and, if it has the broker test hooks,
`tests/harness_broker.h` (otherwise link `tests/harness_broker_unsupported.c`, which skips those tests), then
configure with
`-DBDK_RUNTIME_LIBRARY=<library> -DBDK_HARNESS_SOURCES=<harness.cc>`. `bdk_conformance --list` and `--run <name>`
select tests.

## Cross-platform checklist (c-abi.md section 11) for this skeleton

No AppKit, Win32 or toolkit type appears; native objects cross only as `bdk_native_handle_t` (kinds exist for NSView,
NSWindow, NSApplication, NSEvent, HWND, X11 and Wayland). The stub uses only the C++23 standard library and
pthreads, and builds on macOS and Linux.

Reviewed for Windows, Linux and a .NET binding in T-XPL-01: [docs/release/cross-platform-review.md](../../../docs/release/cross-platform-review.md).
From it:

* Hosts are 64-bit only (x64 and arm64), so no calling convention is named in the headers (X4).
* Whether a native handle is the user's confirming event is decided in one place, `is_gesture_event`
  (`runtime/ui_router.cc`); a new platform adds its input event's kind there and nowhere else (X1, X2).
* Functions returning `bdk_string_t` return a 16-byte struct by value: a hidden pointer on Windows x64, two
  registers elsewhere. Blittable for .NET (X5).
* A .NET or other managed binding calls `bdk_runtime_start` on its UI thread and receives every callback there
  (X11).

## Running the suite against the engine

`nx run core:test-engine` (`scripts/test-engine.sh`) builds `BDKEngineConformance.app` with CMake
(`-DBDK_ENGINE_FRAMEWORK=<directory with Chromium Framework.framework>`, default the runtime at
`.bdk/runtime`), embeds the framework and runs the suite plus `tests/tests_engine.c` in it. The harness
(`tests/harness_engine.mm`) is an application, because the framework needs `NSApplication` and an app bundle;
it drives the engine through `chromium/bdk/engine/bdk_testing.h`. Chromium starts once per process, so the
harness keeps it between runtimes (`BDK_ENGINE_KEEP_CHROMIUM`), forgets persistent profiles at each start (the
suite expects a new data directory) and serves every http(s) load from the engine's interceptor. It needs a
logged-in Mac. Each run has a new data directory under `/tmp` (`bdk-engine-conformance-*`, a Chromium profile tree): `scripts/test-engine.sh` makes it, passes it as `BDK_ENGINE_DATA_DIR` and removes it once the app has exited, even after a crash. Run directly, the harness makes its own and removes it at exit; a crash then leaves it. The whole suite runs in one process: a runtime start and stop leaves nothing behind, and a profile
BDK closes is unloaded by Chromium (`engine_a_closed_profile_releases_its_descriptors` counts descriptors). Scenarios that need a key window (an input method's composition) are skipped, with a message,
when macOS does not let the harness come to the front. `BDK_TEST_TRACE=1` prints every contents event.

`BDK_ENGINE_SILENT=1` (or `nx run core:test-engine:silent`) runs it without getting in the way of whoever is using
the Mac: the harness never activates, has no Dock icon or menu bar, keeps its windows behind every other window and
never takes one fullscreen, and Chromium keeps drawing its occluded pages (`disable-backgrounding-occluded-windows`).
The scenarios that need a key window are skipped, as above. The screen must still be unlocked: with it locked, page
capture, the accessibility tree and both HTML fullscreen scenarios fail.

A harness implements four hooks besides pumping and acting as Chromium: `bdk_harness_prepare_start_config`,
`bdk_harness_attach_view` (focus needs a window in a real runtime), `bdk_harness_session_format` (tests that
craft the stub's text format run only for it) and `bdk_harness_newer_session_state`.
