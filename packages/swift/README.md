# swift

The macOS SDK: a Swift layer over the C ABI in `packages/core`. Spec: `docs/planning/sdk/swift-api.md`. Tasks
T-SWF-01 to T-SWF-04, T-SWF-06, T-SVC-05, the M7 page and automation APIs (T-AUT-02 to T-AUT-05, T-SVC-09) and the Swift wrappers of T-SVC-02, T-SVC-03, T-SVC-04 and T-SVC-06 are implemented and tested against the in-memory stub runtime. Nothing here has run against
Chromium, and nothing in this package claims engine behaviour works: the real runtime arrives with T-PKG-04 and
the engine tasks.

## Layout

The SwiftPM manifest is the repository root `Package.swift`, because SwiftPM resolves a package by URL only from
the root. Sources stay here; the targets point into `packages/swift` and `packages/core`.

| Target | Path | Contents |
|---|---|---|
| `CBDK` | `Sources/CBDK` | The C module Swift imports: the headers in `packages/core/include/bdk` and `packages/core/generated/bdk_abi_hash.h`, reached through two symlinks in `include/`. `BDK_ABI_VERSION` and `BDK_ABI_HASH` are read from them, never copied. The `CBDKStub` module in the same module map exposes `packages/core/stub/bdk_stub.h`. |
| `BDKStubRuntime` | `packages/core` | The in-memory runtime: the runtime core (`runtime/`) and the stub backend (`stub/`), compiled as C++. It is not a dependency of `BDK`. |
| `BDK` | `Sources/BDK` | The SDK. Depends only on `CBDK`. |
| `BDKTestSupport` | `Sources/BDKTestSupport` | `BDKRuntime.startStub`, the stub's controls (`runtime.stub`) and the driver that pumps the stub. Depends on `BDK`, `BDKStubRuntime` and `BDKStubApplication`. |
| `BDKStubApplication` | `Sources/BDKStubApplication` | The stub's `BDKApplicationBase` (`packages/core/include/bdk_mac/bdk_application.h`), a plain `NSApplication` subclass. The real runtime's framework exports its own. |
| `BDKTests` | `Tests/BDKTests` | Swift Testing suites. All suites are nested in one serialized suite, because the runtime is process-wide. |

`cxxLanguageStandard: .cxx2b` is C++23 (SwiftPM has no `.cxx23` yet). No `unsafeFlags` are used, so the package
can be a dependency.

## Build, test, lint

```sh
pnpm exec nx run swift:build
pnpm exec nx run swift:test
pnpm exec nx run swift:lint
pnpm exec nx run swift:format
```

or `swift build` and `swift test` at the repository root. Tests create `NSWindow`s, so they need a window server
session (a logged-in Mac or the hosted macOS runner).

## What is implemented

| Area | Types | Task |
|---|---|---|
| Application | `BDKApplication`, a subclass of the runtime's `BDKApplicationBase`: Chromium's `BrowserCrApplication` with the real runtime (event routing, accessibility activation), `NSApplication` with the stub. `BDKRuntime.terminate(replyingTo:)` cancels the first quit, shuts down and quits again | T-SWF-01, T-MAC-02 |
| ABI check | `BDKRuntime.start` passes the version and hash from the headers; a mismatch throws `BDKError` `abi.versionMismatch` or `abi.hashMismatch`, and the message and `details` name the host's and the runtime's values | T-SWF-01 |
| Configuration | `BDKConfiguration` (resolved JSON, bundle URL, `LaunchOptions.userDataDirectory`), `load(from:)` reads `bdk.resolved.json` from a bundle. Route resolution is the CLI's job. | T-SWF-02 |
| Runtime | `BDKRuntime`: `start`, `shared`, `isRunning`, `info`, `events`, `shutdown()`, `terminate(replyingTo:)`. `start` freezes the registrations. | T-SWF-02 |
| Profiles | `BDKProfiles` (`list`, `create`, `open(id:)`, `open(_:createIfMissing:)`, `openPrivate`, `delete`, `loaded`), `BDKProfile` (`info`, `rename`, `close`, `webContents`) | T-SWF-02 |
| Pages | `BDKWebContents`: create, `close()`, `destroy()`, `state()` and state properties, `navigation` (`load`, `reload`, `stop`, `goBack`, `goForward`, `go(toOffset:)`, `go(toIndex:)`, `entries`), `setMuted`, `setZoom`, `zoom` (`factor`, `set(_:scope:)`), `media` (`isMuted`, `exitPictureInPicture()`), `canPerform(_:)` and `perform(_:)` for `PageCommand`, `setUserAgent`, `discard`, `setAutoDiscardable`, `sessionState`, `restore`, `focus`, `events` | T-SWF-02 |
| Errors | `BDKError` with `domain`, `code`, `message`, `details`; `stale` for any object used after destruction, profile close or shutdown | T-SWF-02 |
| Events | `BDKEventSequence`: any number of iterators, each sees every event from when it was created (call `makeAsyncIterator()` before the action, since an iterator created inside a `Task` subscribes when the task first runs) | T-SWF-02 |
| Calls | Asynchronous ABI calls bridge to `async throws` with a continuation resumed once; cancelling the task calls `bdk_call_cancel` and throws `broker.cancelled` | T-SWF-02 |
| Views | `BDKWebView` (hosts `bdk_contents_native_view`, shows URL and title when the runtime reports `platform.unsupported`, registers each `NSWindow` once with `bdk_window_register` and unregisters it when it closes, forwards focus), `BDKWebContentsView` (SwiftUI), `BDKKeyboardDelegate` | T-SWF-03 |
| UI requests | `BDKUIHandlers` (`config.ui`, `runtime.ui`): one `Surface<Request>` per surface with `setHandler(_:)` and `mode`; `BDKUIResponse` (`.handled`, `.useDefault`, `.reject`); `BDKUIRequest` and a typed subclass per surface (see below) | T-SWF-04 |
| Windows | `runtime.windows.register(_:_:)` (public; a page view entering an unregistered window registers it as `.normal`), `BDKWindow` (`nsWindow`, `kind`, `pages`, `activePage`, `extensionWindowID`, `anchors`, `delegate`), `BDKWindowDelegate`, `WindowRequest`, `BDKWebContents.window` | T-SWF-08 |
| Extensions | `profile.extensions` (the generated `BDKExtensions` plus `Sources/BDK/BDKExtensions+Host.swift`: install sources, `actions(for:)`, `actionChanges`, `activateAction`, `performCommand`, `presentDefaultMenu`), `ExtensionPopupRequest`, `ExtensionSidePanelRequest`, `ExtensionInstallRequest`, `ExtensionUninstallRequest` | T-EXT-02 to T-EXT-06 |
| Settings | `BDKSettingKey`, `BDKSettingRegistry` (`config.settings`), typed `settings.get`, `set`, `reset`, `describe` on `BDKSettings`, `config.declarations` | T-SVC-08 |
| Providers | `config.services` (`BDKServiceRegistry`), the provider protocols, `page.translation`, `TranslationRequest` (see below) | T-PRV-01, T-PRV-03, T-PRV-09 |
| Page routes | `config.pages` (`PageRegistrations.register(_:_:)`), `PageMode` (`.chromium`, `.bundled`, `.native`, `.disabled`), `NativePageRequest`, `NativePageResult` | T-SWF-06 |
| Commands | `BDKCommand`, `BDKEvent`, `BDKCommandRegistry` (`config.commands`), `BDKCommands` (`runtime.commands`: `register`, `declare`, `emit`, `events`, `invoke`), `BDKCommandContext`, `BDKDeclarations` | T-SVC-05 |
| Scripting | `page.scripting` (`Scripting`): `evaluate`, `call`, `insertCSS`, `remove`, `frames`, `query`, `snapshot`, `click`, `hover`, `type`, `press`, `scroll`, `waitFor`, over `contents.*` | T-AUT-03, T-AUT-07 |
| Capture | `page.capture` (`PageCapture`): `screenshot`, `pdf`, `save`, over `contents.screenshot`, `pdf` and `save`. `BDKCommands.invokeBinary` returns the bytes of a command marked binary next to its JSON | T-SVC-09 |
| DevTools | `page.devTools` (`DevTools`): `present`, `close`, `state`, `isOpen`, `attachProtocol` returning a `BDKDevToolsProtocol` (`send`, `events`, `detach`); errors in the `devtools` domain | T-AUT-05 |
| Automation | `runtime.automation` (`BDKAutomation`): `makeSession`, `events`, `external.open`; `BDKAutomationSession` (`run`, `grant(contents:)`, `revoke`), `BDKAutomationTask` (`events`, `value`, `cancel`), `BDKAutomationEndpoint` (`socketPath`, `close`) | T-AUT-02, T-AUT-04 |

Pages and profiles dropped without `close()` are closed on deinit with a warning in the log.

### Page routes and page commands

`config.pages.register("downloads", .native { request in … return .handled })` sets how a route is served, as in
architecture.md 8.2. `start` hands each registration to `bdk_page_handler_register` between the runtime start and
the freeze, so a registration the runtime refuses (`config.noBuiltin`, `routeUnknown`, `routeInvalid`, `routeMode`,
and `handlerMissing` at the freeze) is the start error and nothing is left running. Registering a route twice keeps
the last registration, so only one per route crosses the ABI. A native handler runs on the main actor and gets a
`NativePageRequest` (`route`, `url`, `path`, `query`, `source`, `profile`); `source` is the page object the
application holds for the page that asked, nil for one it does not hold. A request whose profile the application
does not hold, or whose JSON is malformed, is answered `.blocked`. The handler's context is released with the
runtime.

`page.canPerform(_:)` and `page.perform(_:)` take a `PageCommand`; `canPerform` is false on a closed page and
`perform` throws `contents.stale` there, or `contents.commandDisabled` for a command that cannot be performed now.
`page.zoom.set(_:scope:)` (`.page`, `.site`), `page.media.isMuted` and `page.media.exitPictureInPicture()` are the
page controls. `page.zoom` is a `ZoomControl`; the factor is `page.zoom.factor`.

`profile.search` is the generated `BDKSearch`. Against the stub its commands answer `broker.notImplemented`, so the
tests do not call them. Native routes and page commands are tested in `PageTests.swift` against the stub's route
table (the resolved JSON of `packages/core/tests/tests_pages.c`); a native route commits nothing, so those tests
start the load through the ABI instead of awaiting `navigation.load`.

### Page automation, sessions and the endpoint (M7)

`page.scripting`, `page.capture` and `page.devTools` are thin wrappers: each call is a `contents.*`, `debug.*` or
`devtools.*` command with the page as the invoke options' `contents`, so a closed page throws `contents.stale`. The
stub has no renderer: captures return fixed bytes, `frames` one frame, and the other scripting commands answer
`broker.notImplemented`; real answers are engine scenarios.

Mapping rules this package chose (swift-api.md is silent):

* Types are nested where the spec names them bare: `Scripting.World`, `Scripting.FrameSelector`, `Scripting.Snapshot`,
  `Scripting.InjectedStyle`, `PageCapture.ScreenshotArea`, `PageCapture.PDFOptions`. Frames, elements, snapshot
  kinds and save formats are the generated `BDKFrameInfo` (`ID` is `String`), `BDKElementInfo`, `BDKSnapshotKind` and
  `BDKSaveFormat`.
* `World.page` sends `world: "page"`; `.isolated(name)` sends the name, and `""` or `"page"` throw
  `broker.invalidArguments` (the engine reads "page" as the page's own world). `FrameSelector.main` and `.all` send no
  `frame`, which the engine reads as the main frame; `.frame(id)` sends the id. `call` builds
  `(<function>)(<JSON arguments>)` and evaluates it. `query` also takes `limit`.
* `screenshot` always asks for PNG. `PDFOptions` paper and margins are in inches; a nil field takes the engine's
  default; `pdf` takes an `onProgress` closure. `Paper.a4` is 8.27 x 11.69, `.letter` 8.5 x 11.
* `DevTools.present()` and `close()` are `async throws` (they are commands, unlike `find.present()`). `isOpen` starts
  false, follows `devtools.stateChanged` (subscribed when `page.devTools` is first read) and is refreshed from
  `devtools.state` after `present()`, `close()` and `state()`.
* `attachProtocol()` subscribes to `debug.event` and `debug.detached` before it attaches, so no event is lost.
  `events` is filtered by the session id and ends at `detach()` or `debug.detached`. `detach()` is synchronous and
  idempotent (the command is sent in a task). `send` after a detach throws `devtools.detached`. A session object
  stays alive until it ends: keep it or call `detach()`.
* Input takes `Scripting.InputTarget` (`.selector(_)` or `.point(x:y:)`, never both, which the optional pairs of
  `contents.click` would allow) and the usual `FrameSelector` (`.all` sends no frame). `click(_:frame:button:clickCount:)`
  takes the generated `BDKMouseButton`; `type(_:into:frame:)`, `press(_:modifiers:)` (names as `KeyboardEvent.key`)
  and `scroll(_:dx:dy:frame:)` take the rest as the command does. The stub answers them `broker.notImplemented`;
  the tests check the arguments through `Scripting.Input`, which builds the command and its arguments.
* `waitFor(_:timeout:)` takes `Scripting.WaitCondition`, `.load(BDKLoadCondition)` (`committed`, `finished`) or
  `.url(pattern)` (`*` is a glob), the two conditions the core answers (conventions.md, "Automation", rule 8), and a
  `Duration` (default 30 s, sent as whole milliseconds rounded up). A deadline is `broker.timeout`. `selector`,
  `script` and `networkIdle` are not exposed: the engine answers them in T-AUT-06, and `query` and `evaluate`
  poll in the meantime.
* `BDKAutomation.Scope.profiles` is `[BDKProfile]?` (nil is every profile); `ContentsScope` is `.all`, `.created`,
  `.explicit([BDKWebContents])` (the pages are granted right after creation). `SessionOptions.capabilities` defaults to
  none.
* `BDKAutomationSession.run` is `throws`, not `async`: `bdk_session_run` is synchronous. `grant(contents:)` throws;
  `revoke()` does not and may be repeated. The runtime keeps a session until it is revoked and a running task until
  it finishes. `BDKAutomationTask.value` follows the FINISHED event (`broker.cancelled`, `automation.sessionRevoked`,
  `automation.hostGone`, `automation.scriptFailed`); the task's `events` end at FINISHED. A task's
  `.commandInvoked(command, error:)` error carries the domain and code only.
* `runtime.automation.events` reports `.sessionCreated`, `.sessionRevoked`, `.commandInvoked` and
  `.contentsCreated(page, bySession:)`. A page a session created is wrapped with `BDKWebContents.adopt`, kept by the
  runtime and destroyed once it reports `closed`; pages the application made are not announced.
* `external.open(directory:authenticate:)`: the closure is `async throws`, runs on the main actor per hello and
  returns `.accept(session:)` or `.refuse(message:)` (the message has a default, so `.refuse()` works); a throw
  refuses with the error's description. At most one endpoint is open; `socketPath` is a `String`.
* `BDKStub.defineScript(_:)` plays a script in the stub's host: `StubScript.frames` holds what BDK sent and
  `send(_:)` answers in the wire protocol; `crashAutomationHost()` and `automationCount(_:)` expose the other hooks.

### UI requests

`config.ui.permissions.setHandler { request in … }` registers a handler before start; the runtime installs the
configuration's handlers between `bdk_runtime_start` and the freeze, because `bdk_ui_set_handler` is refused
afterwards (`config.registrationAfterStart`). `runtime.ui` is the live table: a handler set at start can be replaced
or cleared at any time (a cleared one means use the default); a surface that had none at start cannot gain one after
the freeze, and `setHandler` throws. `Surface.mode` reads `surfaces` in `bdk.resolved.json`.

Every delivered request becomes a `BDKUIRequest` (`id`, `surface`, `contents`, `window` as an `NSWindow`, `profile`,
`initiator`, `anchor`, `payload`, `isCompleted`, `isWithdrawn`, `cancellation`, `useDefaultPresenter()`). The object
owns the ABI handle: a request that is released without completing is cancelled (`bdk_ui_request_release`), so a
handler that returns `.handled` must keep the request. `.useDefault` and `.reject` finish it. A second completion
throws `ui.requestCompleted`; completing a withdrawn request does nothing.

| Surface (`runtime.ui.…`) | Request | Completions |
|---|---|---|
| `permissions` | `PermissionRequest` | `grant(_:duration:confirmedBy:)` (grant), `deny(persist:)`, `dismiss()` (deny) |
| `javascriptDialogs` | `JavaScriptDialogRequest` | `accept(text:suppressFurther:)`, `cancel(suppressFurther:)` |
| `httpAuth` | `HTTPAuthRequest` | `provide(username:password:)`, `cancel()` |
| `clientCertificates` | `ClientCertificateRequest` | `select(_:)`, `cancel()` |
| `filePickers` | `FilePickerRequest` | `choose(_:)`, `cancel()` |
| `contextMenus` | `ContextMenuRequest` | `show(_:at:)`, `perform(_:)`, `dismiss()` |
| `newWindows` | `NewWindowRequest` | `accept()`, `reject()` |
| `downloads` | `DownloadRequest`, `DangerousDownloadRequest` | `proceed(to:)`, `cancel()`; `keep(confirmedBy:)` (grant), `discard()` |
| `passwords` | `PasswordSaveRequest`, `LeakedPasswordRequest` | `save(confirmedBy:)` (grant), `never()`, `dismiss()` |
| `autofill`, `autofillSuggestions` | `AutofillSaveRequest`, `AutofillSuggestionsRequest` | `save(confirmedBy:)`, `accept(_:confirmedBy:)` (grant), `preview(_:)` (`bdk_ui_update` `{"preview"}`, nil clears it; fills nothing), `dismiss()` |
| `printing` | `PrintRequest` | `printWithSystemDialog()`, `savePDF(to:)`, `cancel()` |
| `find` | `FindRequest` | `session`, `close()` |
| `fullscreen`, `exclusiveAccess` | `FullscreenRequest`, `ExclusiveAccessRequest` | `allow()`, `deny()`; `acknowledge()` (the only completion; a rejected or dropped notice goes to the default presenter) |
| `pageHealth` | `UnresponsivePageRequest`, `BlockedPopupRequest` (by the payload's `kind`) | `wait()`, `terminate()`; `allowOnce(_:confirmedBy:)` (grant `{"popup"}`), `alwaysAllow(confirmedBy:)` (grant `{"always":true}`), `dismiss()`; `presentation` (`BDKPageHealthPresentation`) and `perform(_:)` |
| `notifications` | `NotificationRequest` | `shown()` (local), `clicked(action:)`, `closed(byUser:)` |
| `certificateErrors`, `reputationWarnings` | `CertificateErrorRequest`, `ReputationWarningRequest` | `goBack()`, `proceed(confirmedBy:)` (grant; `ui.notOverridable` when Chromium refuses) |
| `devTools`, `externalProtocols` | `DevToolsRequest`, `ExternalProtocolRequest` | `accept()`, `reject()`; `open(remember:confirmedBy:)` (grant), `cancel()` |
| `extensionPopups`, `extensionSidePanels` | `ExtensionPopupRequest`, `ExtensionSidePanelRequest` | Open requests: `accept()` (popup) and `accept(side:)` (panel) send `bdk_ui_update` `{"placed":true}` and stay open; `side` sends `{"side"}`; `sizeChanges` and `visibilityChanges` come from `bdk_ui_set_update_handler`; `reject()` (deny), `close()` (accept `{"action":"close"}`); `closed` is the request's `cancellation` |
| `extensionInstalls`, `extensionUninstalls` | `ExtensionInstallRequest`, `ExtensionUninstallRequest` | `accept(confirmedBy:)`, `confirm(confirmedBy:)` (grant), `cancel()` (deny) |
| `translation` | `TranslationRequest` | `translate(from:to:)`, `revert()`, `always(for:)`, `never(for:)`, `neverForThisSite()`, `dismiss()` (none grants, none needs a gesture) |
| `webAuthn` | `WebAuthnRequest`, one per step of a ceremony (same `ceremonyID`; a new step withdraws the last) | `choose(_:)` (accept `{"mechanism"}`), `providePIN(_:)` (accept `{"pin"}`), `chooseAccount(_:confirmedBy:)` (grant `{"account"}`), `accept()` (the step's own button), `useOtherMechanism()` (accept `{"action":"otherMechanisms"}`), `cancel()` (deny); `presentation` and `perform(_:)` describe and answer the step whatever draws it; `qrCodeImage(scale:)`; `cancelAll` as a handler. `BDKWebAuthnSupport.current` (main actor) says which mechanisms the application can offer and what it lacks for the others; phones also need `[services.webauthn] phones = true` in the running runtime's configuration (`.configuration(…)` while it is off) |
| `deviceChoosers`, `screenCapture` | `BDKRawUIRequest` | `complete(_:arguments:confirmedBy:)` with "grant", "deny" or "accept" |

Completion mapping (provisional, settled here for T-UIR-01 to T-UIR-06): a completion that grants (permission, a kept
dangerous download, a saved password or card, an accepted suggestion, a proceed past an interstitial, opening an
external protocol, a WebAuthn account, a raw "grant") is `"grant"` and takes the `NSEvent` as the gesture handle. Every other positive
answer is `"accept"`, every negative one is `"deny"`, and the arguments JSON carries the rest (`{"kinds":[…],
"duration":"thisVisit"}`, `{"username":…,"password":…}`, `{"paths":[…]}`, `{"action":"perform","item":id}` and so
on; the code in `Sources/BDK/UI` is the reference until the interface definitions fix them). Completions only
narrow what was asked: a grant for a kind that was not requested, a certificate or menu item the request did not
offer, or several files for a single-file picker throw a `ui.*` error before the ABI is called.

Payload fields are the camelCase names in ui-requests.md; a missing or malformed field takes a default, so a
handler always sees the request. Handles inside a payload (`opener`, `newContents`, `previewContents`, `inspected`,
`frontend`) are decimal strings, since handles exceed 2^53. A page such a field carries (`newContents`, `frontend`,
`previewContents`) is the application's once it reads the field, like a page it created, and a page the application
releases is destroyed. A request handed to the default presenter is the exception: the runtime then holds the page
until it closes, so the default window stays open without the application keeping a reference.

### Commands and events

`config.commands.register(OpenTab.self) { input, context in … }` and `config.commands.declare(TabsChanged.self)` collect
commands and events before start. A `BDKCommand` states its name, `input`/`output` shape (`BDKShape` over `BDKType`:
the interface language of `packages/core/interface`), named `types`, `requiresUserGesture`, `profile` and
`reportsProgress`, next to its `Codable` input and output, because Swift cannot reflect a `Codable` type's fields.
`start` validates names (`config.nameNotApp`, `config.duplicateName`) before the runtime starts, then registers each
with `bdk_app_command_register` and `bdk_app_event_declare`. The handler is `async`, runs on the main actor and gets
a `BDKCommandContext` (`caller`, `profile`, `report(progress:message:)`, `isCancelled`, `checkCancelled()`). When
the caller cancels, the handler's task is cancelled (the ABI has only `bdk_app_command_is_cancelled`, so a 10 ms poll
drives it). A thrown `BDKError` reaches the caller with its domain and code; any other error is
`broker.handlerFailed`; an input that does not decode is `broker.invalidArguments`.

`runtime.commands.invoke(OpenTab.self, input)` and `invoke("app.name", arguments:)` call by name with `onProgress`;
`events(Event.self)` and `events(named:)` subscribe (`bdk_subscribe_named`) and end with the runtime;
`emit(_:to:)` takes `.all`, `.profile`, `.page` or `.session`. `runtime.commands.register` and `declare` work only
between start and the freeze, which `BDKRuntime.start` does at once; they exist for tests and fail afterwards with
`config.registrationAfterStart`.

Declarations for code generation: `config.declarations` (`BDKDeclarations`: commands, events and settings; `config.commands.declarations` has no settings) needs no runtime.
`declarations.write(to:)` writes `bdk.commands.json`, `{"version":1,"commands":[…],"events":[…],"settings":[…]}` sorted by name with
sorted keys, so equal input gives equal bytes. Each command entry is an interface-definition entry (`name`,
`capability`, `callers`, `profile`, `args`, `result`, optional `requires`, `progress`, `types`) with the same type
expressions as `packages/core/interface`. The extractor (T-JS-02, T-CLI-04) is a small executable target of the
application that builds its configuration the same way and calls `write(to:)`; the generator reads the JSON.

### Providers and page translation

`config.services.translation.register(MyProvider())`, `.reputation.register(…)` and `.geolocation.register(…)` collect
the implementations of a service whose `provider` is `app` in `bdk.toml`; `config.services.google.apiKey` is the key of a
service whose provider is `google` (it goes to `bdk_start_config_t.google_api_key`). `BDKRuntime.start` registers each
with `bdk_provider_register` before the freeze, so start errors are `BDKError`s: `config.providerMissing` (an `app`
service with no provider, found at the freeze), `config.providerNotApp`, `config.descriptorInvalid`, `config.keyMissing`
and `config.keyUnused`. A registry is a value; registering twice for a service keeps the last.

| Protocol | Required | Optional operations (refining protocols, offered to BDK only when the provider conforms) |
|---|---|---|
| `BDKTranslationProvider` | `supportedPairs()`, `translate(_:pair:context:)` | `BDKTranslationModelManaging` (`prepare(_:progress:)`, `removeModel(_:)`), `BDKLanguageDetecting` (`detectLanguage(of:)`) |
| `BDKReputationProvider` | `check(_:context:)` | `BDKDownloadChecking` (`check(_ download:)`), `BDKReputationUpdating` (`update()`) |
| `BDKGeolocationProvider` | `positions(_:)`, an `AsyncThrowingStream<BDKPosition, Error>` | |

Each call runs the provider in a task on the main actor and answers with `bdk_provider_resolve` or `bdk_provider_reject`:
a `BDKError` in the `provider` domain keeps its code, any other error is `provider.remoteError` with its description.
The core checks every answer against the contract (`provider.contractViolation`; conventions.md, "Providers"), for
instance that a navigation verdict is not `dangerousDownload`. When BDK no longer wants an answer (the page navigated,
the translation was reverted, the watch stopped, shutdown) the task is cancelled. A geolocation watch reports each
position of the stream with `bdk_provider_report` and ends when the stream finishes (the call resolves with null) or
BDK cancels it, which ends the iteration and so triggers the stream's `onTermination`. A `BDKError` with the code
`permissionDenied`, `positionUnavailable` or `timeout` thrown by the stream is reported to the page as the position
error. `BDKTranslationModelManaging.prepare` reports progress through its closure.

`page.translation` (swift-api.md 6.7) is `state()`, `translate(to:from:)`, `revert()` and `present()`: the page commands
`translation.state`, `translate`, `revert` and `present`, invoked by name with the page in the options (the core finds
the profile). They are not generated, because they act on a page. `BDKWebContents.Event.translation(BDKTranslationState)`
carries `BDK_CONTENTS_EVENT_TRANSLATION`. The profile's provider, languages, models and preferences are the generated
`profile.translation`. `TranslationRequest` is the offer (and what `present()` raises); later phases are the events.

Against the stub, `BDKStub` also sets and reads a page's text (`setPageText(_:language:on:)`, `pageText(of:)`: the stub
detects, offers and translates as the engine would after a load), loads a reputation list (`loadReputationList(_:)`) and
starts, reads and stops a page's geolocation watch (`startGeolocationWatch(on:highAccuracy:)`, `lastGeolocation(of:)`,
`stopGeolocationWatch(on:)`). `CBDKStub` exports `bdk_stub_providers.h` for them.

### Services

The wrappers for the built-in commands (`history`, `bookmarks`, `downloads`, `settings`, `permissions`, `cookies`,
`privacy`, `passwords`, `autofill`, `system`, `runtime`; tasks T-SVC-02 to T-SVC-06) are generated. `packages/js/codegen`
writes `Sources/BDK/Generated/BDKServices.swift` from `packages/core/interface/*.json`; the file is committed and
`nx run js-codegen:codegen-check` fails when it is stale. Do not edit it: change the definition, or the small naming
table at the top of `packages/js/codegen/src/swift.ts`, and run `nx run js-codegen:codegen`. The hand-written part is
`Sources/BDK/Services/BDKServiceScope.swift` (encoding, the call, the event merge) and the typed `settings` helpers.

| Interface | Swift |
|---|---|
| Namespace, profile mode `required` | `struct BDKHistory` and so on, on `BDKProfile`: `profile.history` |
| `optional` (`settings`) | on both: `profile.settings` and `runtime.settings` |
| `none` (`runtime`, `system`) | on `BDKRuntime`: `runtime.about`, `runtime.system.defaultBrowser` |
| `ns.group.method` | a nested service: `profile.autofill.cards.list()` |
| `profiles` | not generated; `runtime.profiles` and `BDKProfile` use the ABI's profile functions |
| Named type | `BDK` + name, `Codable`, `Sendable`, `Equatable`; enums are `String` enums |
| `timestamp`, `url`, `json`, `bytes` | `Date` (milliseconds on the wire), `URL`, `BDKJSValue`, `Data` |
| Command | `async throws` method; optional arguments default to `nil`; a first argument `id` or `key` has no label |
| One-field result | the field itself (`history.visits` returns `[BDKHistoryVisit]`) |
| Several-field result | a nested struct (`BDKHistory.SearchResult`) |
| `progress` | an `onProgress:` parameter, called on the main actor before the result |
| `gesture` | nothing: a gesture is a page's, and Swift needs none |
| `authentication` | nothing in the signature: the OS prompt appears, a refusal throws `capability.reauthenticationFailed` |
| Events of a namespace | `service.events`, an `AsyncStream` of the namespace's `Event` enum, subscribed when the property is read |

Cancelling the task calls `bdk_call_cancel` and throws `broker.cancelled`. A command the runtime does not implement
throws `broker.notImplemented`. A command whose `callers` leaves out `swift` gets no wrapper. The decisions and the
places the wrappers differ from the examples in `swift-api.md` are in swift-api.md section 11.1.

Against the stub, `BDKStub` also controls the services: `setAuthentication(.accept)` answers the operating system's
prompt, `holdServices(true)` keeps commands running so a test can cancel them, and `serviceCount(_:)` reads what
reached a service. The stub implements only some of the commands (conventions.md, "The broker", rule 12) and emits
none of the service events, so the event streams are tested for decoding and lifetime, not delivery.

### Driving the stub

The stub has no event loop. `BDKTestSupport` installs a scheduler into an internal hook (`EventLoop` in `BDK`,
`package` access, not public API) so that the stub is pumped from the main queue after every ABI call, plus a 5 ms
timer for work posted with `bdk_post_to_ui_thread`. `await`ing a call in a `@MainActor` test therefore works.
`runtime.stub.pump()` runs the queue synchronously when a test wants a deterministic point. `runtime.stub.withAutomaticPumpingPaused { … }` stops the automatic pumping while its body runs,
so a test can start a load and act on it (supersede it, stop it) before the stub finishes it; without the pause the
5 ms timer can complete the load first.

## Differences from swift-api.md

* Calls that are asynchronous in the ABI are `async` here: `BDKProfiles.list()` is `async throws`, not synchronous.
* `BDKProfiles.openPrivate()` opens an ephemeral profile of the runtime. The ABI has no parent profile, so there is
  no `BDKProfile.openPrivate` and no `.private(parent:)` kind (`Kind` is `.persistent` or `.ephemeral`).
* `BDKProfile.close()` is synchronous and works for any profile; it also closes the pages in it.
* Navigation calls `goBack`, `reload` and the like are `throws`, because the ABI can reject them.
* `BDKWebView.contents` is a weak optional rather than `unowned`, so a view that outlives its page cannot crash.
* `BDKWebContents.close()` waits for the `closed` event and then destroys the page; `CloseMode` and `CloseResult`
  need the unload-handler flow, which the ABI does not have.
* `BDKRuntime.shutdown()` returns nothing: `ShutdownResult.cancelled(by:)` needs unload handlers.
* `RuntimeEvent` is `BDKRuntime.Event` and has `.webContentsCreated(page, by: Creator)` for pages an automation
  session creates (the same page as `runtime.automation.events` `.contentsCreated`). `Creator` is `.application`,
  `.automation(session: String)` and `.extension(String)`. `.automation` carries the session's name, not a
  `BDKAutomationSession`: a session from the external endpoint has no object in the application, and names are what
  the audit events use. `.application` and `.extension` are never emitted, because the runtime gives no signal for
  a page the application or an extension creates (`contents.created` has `createdBy` kinds `swift` and `session`
  only). The events are the ABI's kinds (swift-api.md 6.4 lists more). `RuntimeEvent` has `registrationsFrozen`,
  `profileOpened`, `profileClosed`, `shutdownStarted` and `unknown`.
* `BDKKeyboardDelegate` is on the page (`keyboardDelegate`), with a forwarding property on the view. An unhandled key
  the delegate declines is offered to the main menu.
* `BDKJSValue` exists here because error details need it; scripting reuses it (T-AUT-03).
* `BDKTestSupport` offers `scriptNavigation` and `simulateKey` rather than `simulateNavigation` and `raise(...)`, and `makeUnresponsive(_:)`, `makeResponsive(_:)` and `blockPopup(on:url:)` for page health.

* UI requests (T-SWF-04): `BDKUIRequest` is a class, not a protocol; `window` is an `NSWindow?` (there is no
  `BDKWindow` yet); `profile` is optional; `setHandler` throws (it can fail after the freeze); a surface with two
  request kinds (`downloads`, `passwords`) takes `BDKUIRequest`; `NewWindowRequest.newContents` and the other
  page-carrying fields are optional; `BDKMenuItem` has no `icon`; there is no `ProviderDescriptor` beyond the
  generated `BDKProviderDescriptor`. `AutofillSuggestionsRequest.preview(_:)` throws (`ui.unknownSuggestion`) and
  sends an update rather than completing. `BlockedPopupRequest.allowOnce` takes a `BlockedPopup` and the confirming
  event, and `alwaysAllow` the event, where ui-requests.md 5.19 proposed a URL and no event.
* Windows (T-SWF-08): `BDKWindow.nsWindow` is weak and optional (the registry must not keep an `NSWindow` alive);
  `extensionWindowID` is `Int?` (nil for the ABI's -1); `WindowRequest` is a struct with `action` (the doc's enum) and
  `extensionID`, so the delegate learns which extension asked; anchor rectangles are given in AppKit window
  coordinates (bottom-left origin) and converted to the ABI's top-left origin, and an empty rectangle means no anchor;
  `register` returns the existing `BDKWindow` unchanged (kind included) for a window already registered. The
  provider and request handler are installed at registration, so setting `anchors` or `delegate` later needs no ABI
  call.
* Extensions (T-EXT-02 to T-EXT-06): `profile.extensions` is the generated struct, so values are the generated
  `BDKExtensionInfo`, `BDKExtensionActionState`, `BDKExtensionCommand` and `BDKCompatibilityReport`, not the doc's
  `ExtensionInfo` and `ActionState`; icons are `data:` URL strings with `iconImage` (`NSImage?`) and `badgeNSColor`
  helpers. `actions(for:)` is `async throws` (it is the `extensions.actions` command with the page in the invoke
  options), not synchronous. `activateAction` and `performCommand` are synchronous and throw; their
  `ActivationOutcome` is nested in `BDKExtensions` and `.blocked` carries `reason:`. `install(_:)` and `inspect(_:)`
  take `BDKExtensionSource` values (`.unpacked(_:)`, `.crx(_:)`, `.updateURL(id:url:)`, `.store(name:id:)`).
  `ExtensionInstallRequest.source` is `BDKExtensionSourceKind`, and `ExtensionUninstallRequest.requestedBy` is
  `Requester` (`.user`, `.extension(id:)`). `closed` is a `BDKUICancellation`. The popup and panel `secureView` is an
  empty `NSView` when the runtime delivers no native view (the stub).
* Commands (T-SVC-05): `BDKCommand` carries its shape; `BDKCommandContext.caller` is `BDKCommandCaller`;
  `runtime.commands` has no `.profile(profile)` convenience beyond `invoke(…, profile:)`.

## Not implemented, and what adds it

| Left out | Why | Task |
|---|---|---|
| Wrappers for services with no definition yet (extensions, favicons, top sites) | The wrappers are generated, so each arrives with its definition | T-ABI-03, T-SWF-04, T-SVC-06 |
| `config.services.<service>.setKey(_:)`, and the credentials and leak-check providers | `bdk_provider_set_key` always fails `config.keyUnused`: no provider in this runtime takes a key that way (the Google key is `google.apiKey`). The credential and leak-check providers (S14, S16) are proposals | — |
| `BDKProfiles.events` for the `profiles.*` events | `profiles` is a native namespace | T-ABI-03 |
| Host contexts (`runtime.hostContexts`) | Conditional on decision D1; no ABI | T-ADP-06 |
| Real popups, side panels, install prompts, anchors and window requests | Only the stub implements them so far: a popup or panel `secureView` and the engine's behaviour arrive with the engine tasks. The stub records outcomes and updates only for requests raised through `bdk_stub_raise_ui_request`, not for the popup an activation raises | T-EXT-02 to T-EXT-06 (engine) |
| `BDKRuntime.contents` lookup by id, `BDKProfiles.events`, `ProfileDescriptor.lastUsed`, profile rename by id | Not in the ABI (a rename needs an open profile handle) | T-ABI-03 |
| `displayURL`, favicon, `capture`, `isCrashed`, `isDiscarded`, `isAutoDiscardable` getter, `isFullscreen`, opener, `extensionTabID`, `hoveredLinkChanged`, discard events (renderer loss is `.rendererGone`) | `bdk_contents_state_t` and the event kinds do not carry them | T-ABI-03 |
| Navigation `LoadOptions`, `prune`, `deferInitialLoad`, `userAgent` and `isInitiallyMuted` creation options | Not in `bdk_contents_options_t` or `bdk_contents_load` | T-ABI-03 |
| `RuntimeMode`, `bdkVersion`, `chromiumVersion`, `manifestDigest` in `BDKRuntime.Info` | Only `bdk_runtime_manifest_json` exists and its schema is not fixed | T-CHR-08 |
| A Swift test target against the real runtime | The runtime needs an app bundle around it; the real runtime is tested from the minimal browser's end-to-end tests and the C engine suite (`nx run core:test-engine`) instead | — |
| The runtime as a binary target | | T-PKG-04 |
