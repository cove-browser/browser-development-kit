# swift

The macOS SDK: a Swift layer over the C ABI in `packages/core`. Spec: `docs/planning/sdk/swift-api.md`. Tasks
T-SWF-01 to T-SWF-03 are implemented and tested against the in-memory stub runtime. Nothing here has run against
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
| `BDKTestSupport` | `Sources/BDKTestSupport` | `BDKRuntime.startStub`, the stub's controls (`runtime.stub`) and the driver that pumps the stub. Depends on `BDK` and `BDKStubRuntime`. |
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
| Application | `BDKApplication` (an `NSApplication` subclass; Chromium's protocol conformance and event routing arrive with T-MAC-01 and T-MAC-02) | T-SWF-01 |
| ABI check | `BDKRuntime.start` passes the version and hash from the headers; a mismatch throws `BDKError` `abi.versionMismatch` or `abi.hashMismatch`, and the message and `details` name the host's and the runtime's values | T-SWF-01 |
| Configuration | `BDKConfiguration` (resolved JSON, bundle URL, `LaunchOptions.userDataDirectory`), `load(from:)` reads `bdk.resolved.json` from a bundle. Route resolution is the CLI's job. | T-SWF-02 |
| Runtime | `BDKRuntime`: `start`, `shared`, `isRunning`, `info`, `events`, `shutdown()`, `terminate(replyingTo:)`. `start` freezes the registrations. | T-SWF-02 |
| Profiles | `BDKProfiles` (`list`, `create`, `open(id:)`, `open(_:createIfMissing:)`, `openPrivate`, `delete`, `loaded`), `BDKProfile` (`info`, `rename`, `close`, `webContents`) | T-SWF-02 |
| Pages | `BDKWebContents`: create, `close()`, `destroy()`, `state()` and state properties, `navigation` (`load`, `reload`, `stop`, `goBack`, `goForward`, `go(toOffset:)`, `go(toIndex:)`, `entries`), `setMuted`, `setZoom`, `setUserAgent`, `discard`, `setAutoDiscardable`, `sessionState`, `restore`, `focus`, `events` | T-SWF-02 |
| Errors | `BDKError` with `domain`, `code`, `message`, `details`; `stale` for any object used after destruction, profile close or shutdown | T-SWF-02 |
| Events | `BDKEventSequence`: any number of iterators, each sees every event from when it was created (call `makeAsyncIterator()` before the action, since an iterator created inside a `Task` subscribes when the task first runs) | T-SWF-02 |
| Calls | Asynchronous ABI calls bridge to `async throws` with a continuation resumed once; cancelling the task calls `bdk_call_cancel` and throws `broker.cancelled` | T-SWF-02 |
| Views | `BDKWebView` (hosts `bdk_contents_native_view`, shows URL and title when the runtime reports `platform.unsupported`, registers each `NSWindow` once with `bdk_window_register` and unregisters it when it closes, forwards focus), `BDKWebContentsView` (SwiftUI), `BDKKeyboardDelegate` | T-SWF-03 |

Pages and profiles dropped without `close()` are closed on deinit with a warning in the log.

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
* The events are the ABI's kinds (swift-api.md 6.4 lists more). `RuntimeEvent` has `registrationsFrozen`,
  `profileOpened`, `profileClosed`, `shutdownStarted` and `unknown`.
* `BDKKeyboardDelegate` is on the page (`keyboardDelegate`), with a forwarding property on the view. An unhandled key
  the delegate declines is offered to the main menu.
* `BDKJSValue` exists here because error details need it; scripting reuses it (T-AUT-03).
* `BDKTestSupport` offers `scriptNavigation` and `simulateKey` rather than `simulateNavigation` and `raise(...)`.

## Not implemented, and what adds it

| Left out | Why | Task |
|---|---|---|
| Page and command registration, services, settings, providers on `BDKConfiguration` | The ABI declares `bdk_app_*`, `bdk_page_handler_register` and `bdk_provider_register`, but their Swift shape depends on the interface definitions | T-ABI-03, T-SWF-04, T-SVC-01 |
| UI-request handlers (`BDKUIHandlers`) | `bdk_ui_*` is declared; the handler API is its own task. The stub raises requests; with no handler they take the default. | T-SWF-04 |
| `BDKWindow`, anchors, window delegate, host contexts | `bdk_window_register` takes a window and a kind and nothing more yet; the other `bdk_window_*` functions are not declared | T-UIR-01, T-ADP-06 |
| Page commands (`perform`, `canPerform`), find, capture, print, translation, scripting, user scripts | Not declared in the ABI | T-UIR-04, T-UIR-06, T-SVC-06, T-AUT-03 |
| `BDKRuntime.contents` lookup by id, `BDKProfiles.events`, `ProfileDescriptor.lastUsed`, profile rename by id | Not in the ABI (a rename needs an open profile handle) | T-ABI-03 |
| `displayURL`, favicon, `capture`, `isCrashed`, `isDiscarded`, `isAutoDiscardable` getter, `isFullscreen`, opener, `extensionTabID`, `hoveredLinkChanged`, process-gone and discard events | `bdk_contents_state_t` and the event kinds do not carry them | T-ABI-03 |
| Navigation `LoadOptions`, `prune`, `deferInitialLoad`, `userAgent` and `isInitiallyMuted` creation options | Not in `bdk_contents_options_t` or `bdk_contents_load` | T-ABI-03 |
| `RuntimeMode`, `bdkVersion`, `chromiumVersion`, `manifestDigest` in `BDKRuntime.Info` | Only `bdk_runtime_manifest_json` exists and its schema is not fixed | T-CHR-08 |
| Chromium application protocols and event routing in `BDKApplication`; the real native view; `@testable` conformance against the real runtime | Need the engine | T-MAC-01, T-MAC-02, T-MAC-03 |
| The runtime as a binary target | | T-PKG-04 |
