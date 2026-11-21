# BDK Swift API reference

Status: proposal, 2026-11-16. This is a table form of [swift-api.md](swift-api.md), for lookup. It adds no API. If the two disagree, `swift-api.md` wins and this file is wrong. Signatures are illustrative and have not been compiled; names are placeholders. Status labels are in [README.md](README.md) section 2. Section numbers in the "Ref" column point into `swift-api.md`. Surfaces defined in other documents are listed in section 18 and not repeated.

## 1. How to read the tables

| Mark | Meaning |
|---|---|
| `async` | `async throws`. Crosses into Chromium and can take time. Cancelling the Swift task cancels the native work. |
| `throws` | Synchronous, can throw `BDKError` (section 16). |
| sync | Synchronous and does not throw. |
| `AS` | Exposes an `AsyncSequence`. Several consumers may iterate; each gets every event from the moment it starts. |
| R | Read-only property. |
| RW | Settable property. |
| Plan / Added / Conditional / Later | Status labels from README section 2. A row with no label is Plan. |

Rules that apply to every row:

| Topic | Rule |
|---|---|
| Actor | Every public type is `@MainActor`. |
| Values | Requests, events and results are `Sendable` value types. `BDKRuntime`, `BDKProfile`, `BDKWebContents` and `BDKWindow` are classes. |
| Stale objects | Any call on a destroyed object throws `BDKError` `contents.stale` (or the matching domain). It never crashes. |
| Capability checks | None for Swift. The application has full authority. |
| Naming | Top-level types carry the `BDK` prefix. Nested types do not. |
| Stability | Experimental until v1. |

## 2. Types at a glance

| Type | Kind | Reached from | Owns or does | Ref |
|---|---|---|---|---|
| `BDKApplication` | `NSApplication` subclass | `NSPrincipalClass` | Routes `sendEvent(_:)` through Chromium | 3 |
| `BDKConfiguration` | struct | `BDKConfiguration.load()` | Registrations that TOML cannot hold: code and secrets | 4.1 |
| `BDKRuntime` | class | `BDKRuntime.start(_:)`, `.shared` | The running engine, one per process | 4.3 |
| `BDKProfiles` | class | `runtime.profiles` | List, create, open, rename, delete profiles | 5 |
| `BDKProfile` | class | `profiles.open(…)` | A storage context and every per-profile service | 5 |
| `BDKWebContents` | class | `profile.webContents.create(…)` | One page | 6 |
| `BDKWebView` | `NSView` | `page.view` | The page's native view | 7 |
| `BDKWindow` | class | `runtime.windows.register(…)` | Optional window registration, anchors, delegate | 8.1 |
| `BDKUIHandlers` | class | `runtime.ui`, `config.ui` | One `Surface` per browser-owned surface | 10 |
| `BDKCommands` | class | `runtime.commands` | Application commands, events, generic `invoke` | 11.1, 12 |
| `BDKAutomation` | class | `runtime.automation` | Sessions and the external endpoint | 13 |
| `BDKDiagnostics` | class | `runtime.diagnostics` | Crash reports, logs, net log | 15 |
| `BDKSystem` | class | `runtime.system` | Default-browser registration | 11.1 |
| `BDKError` | struct | thrown | Domain, code, message, details | 16 |

## 3. Application object (Plan, M0 and M2, T-MAC-01, T-MAC-02)

| Member | Signature | Does |
|---|---|---|
| `BDKApplication` | `open class BDKApplication: NSApplication` | Conforms to Chromium's application protocols. A subclass that overrides `sendEvent(_:)` must call `super`. |

| The application does | BDK does |
|---|---|
| Starts normally; calls `BDKRuntime.start` from `applicationWillFinishLaunching` | Checks the ABI, registers the scheme, starts Chromium nested in the running loop, holds a keep-alive |
| Builds its menus and windows | Nothing |
| Handles `application(_:open:)` | Nothing. Opening a URL is "create or reuse a page and navigate". |
| Returns `runtime.terminate(replyingTo:)` from `applicationShouldTerminate` | Cancels that attempt, runs unload handlers, closes host contexts, stops Chromium, then terminates the application again (the second `applicationShouldTerminate` gets `.terminateNow`). Not `.terminateLater`: AppKit's reply loop would nest inside Chromium's and block its shutdown |

## 4. Configuration and runtime (Plan, M2, T-ABI-04, T-SWF-01)

### 4.1 `BDKConfiguration`

| Member | Type or signature | Ref | Does |
|---|---|---|---|
| `load(from:)` | `static func load(from: Bundle = .main) throws -> BDKConfiguration` | 4.1 | Reads and verifies `bdk.resolved.json` |
| `browser` | `Browser` (R) | 4.1 | Name, internal scheme |
| `pages` | `PageRegistrations` | 4.2 | Route modes |
| `commands` | `CommandRegistrations` | 12 | Application commands and events |
| `settings` | `SettingRegistrations` | 12.3 | Application settings |
| `services` | `ServiceRegistrations` | 11.2 | Provider keys and implementations |
| `ui` | `BDKUIHandlers` | 10 | Required here for surfaces in `custom` mode |
| `diagnostics` | `DiagnosticsRegistrations` | 15 | Crash report handler |
| `launch` | `LaunchOptions` | 4.1 | Debug and test overrides |

Nothing here can change what `bdk.toml` decided about capabilities, surfaces or sources.

| `LaunchOptions` member | Type | Notes |
|---|---|---|
| `userDataDirectory` | `URL?` | Tests only. Ignored in release builds. |
| `logLevel` | `LogLevel?` | Debug builds only. |

### 4.2 Page registrations

| Member | Signature | Does |
|---|---|---|
| `register` | `mutating func register(_ route: String, _ mode: PageMode)` | Sets how a built-in route is served. Before start only. |

| `PageMode` | Meaning |
|---|---|
| `.chromium` | Serve the built-in page |
| `.bundled` | Serve the bundled page already in the resolved file |
| `.native(handler)` | Load no page. The navigation is cancelled in the page that asked and the handler decides what the user sees. |
| `.disabled` | Route is not served |

| `NativePageRequest` field | Type |
|---|---|
| `route` | `String` (`"downloads"`) |
| `url` | `URL` (`cove://downloads/recent?x=1`) |
| `path` | `String` (`"/recent"`) |
| `query` | `[URLQueryItem]` |
| `source` | `BDKWebContents?` (the page whose navigation asked) |
| `profile` | `BDKProfile` |

`NativePageResult` is `.handled` or `.blocked`. `blocked` shows the standard error page.

### 4.3 `BDKRuntime`

| Member | Signature | Kind | Does |
|---|---|---|---|
| `start` | `static func start(_ configuration:) throws -> BDKRuntime` (`@discardableResult`) | throws | Returns when Chromium's browser process is initialised in this process |
| `shared` | `static var shared: BDKRuntime` | R | Traps before `start` |
| `isRunning` | `static var isRunning: Bool` | R | |
| `info` | `Info` | R | See below |
| `profiles` | `BDKProfiles` | R | Section 5 |
| `contents` | `BDKWebContentsRegistry` | R | Every live page, lookup by id |
| `windows` | `BDKWindows` | R | Section 8 |
| `ui` | `BDKUIHandlers` | R | Section 10 |
| `commands` | `BDKCommands` | R | Section 12 |
| `automation` | `BDKAutomation` | R | Section 13 |
| `diagnostics` | `BDKDiagnostics` | R | Section 15 |
| `system` | `BDKSystem` | R | Default-browser registration |
| `events` | `Events` | AS | `RuntimeEvent` |
| `terminate(replyingTo:)` | `func terminate(replyingTo: NSApplication) -> NSApplication.TerminateReply` | sync | For `applicationShouldTerminate` |
| `shutdown(_:)` | `func shutdown(_ options: ShutdownOptions = .init()) async -> ShutdownResult` | async | Runs `beforeunload` handlers |

| `Info` field | Example |
|---|---|
| `bdkVersion` | `"0.4.0"` |
| `chromiumVersion` | `"158.0.8127.37"` |
| `abiVersion` | `UInt32` |
| `abiHash` | `String` |
| `manifestDigest` | `String` |
| `mode` | `.prebuilt`, `.source` |
| `isTestedConfiguration` | `false` when feature overrides are set |

| `RuntimeEvent` | Payload |
|---|---|
| `gpuProcessGone` | `ProcessGoneReason`. Chromium restarts it. |
| `utilityProcessGone` | `name`, `ProcessGoneReason` |
| `automationHostGone` | `ProcessGoneReason`. Sessions have ended with an error. |
| `memoryPressure` | `MemoryPressureLevel` |
| `webContentsCreated` | `BDKWebContents`, `by: Creator` (`.application`, `.automation(session)`, `.extension(id)`) |
| `webContentsDestroyed` | `BDKWebContents.ID` |

| `ShutdownResult` | Meaning |
|---|---|
| `.completed` | |
| `.cancelled(by:)` | An unload handler kept its page open. The runtime keeps running. |

| Rule | Detail |
|---|---|
| Start | Succeeds or throws with nothing left running. No partial start. |
| Once | `start` is callable once per process. Chromium cannot restart in the same process after `shutdown`. |
| Frozen | After `start` the route table, capability table and provider choices are frozen. |

## 5. Profiles (Plan, M2, T-SWF-02)

### 5.1 `BDKProfiles`

| Member | Signature | Kind | Does |
|---|---|---|---|
| `list` | `func list() -> [ProfileDescriptor]` | sync | Every profile on disk |
| `create` | `func create(name:) async throws -> ProfileDescriptor` | async | The `profiles.create` command |
| `open(_:createIfMissing:)` | `func open(_ name: String, createIfMissing: Bool = true) async throws -> BDKProfile` | async | Convenience over list, create, open(id:) |
| `open(id:)` | `func open(id: BDKProfile.ID) async throws -> BDKProfile` | async | |
| `loaded` | `[BDKProfile]` | R | |
| `rename` | `func rename(_ id:, to:) async throws` | async | |
| `delete` | `func delete(_ id:) async throws` | async | Refused while pages exist in it |
| `events` | `Events` | AS | created, loaded, renamed, deleted |

`ProfileDescriptor`: `id` (stable across launches), `name`, `lastUsed: Date?`.

### 5.2 `BDKProfile`

| Member | Type or signature | Does |
|---|---|---|
| `id` | `ID` | |
| `name` | `String` (R) | |
| `kind` | `Kind` | `.persistent`, `.private(parent:)` |
| `isPrivate` | `Bool` (R) | |
| `openPrivate(_:)` | `func openPrivate(_ label: String? = nil) throws -> BDKProfile` | Off-the-record child. Nothing it stores survives `close()`. |
| `close()` | `async throws` | Private profiles only. Refused while pages exist. |
| `webContents` | `WebContentsFactory` | `create(_:)`, `all` |

| Per-profile service | Type | Section |
|---|---|---|
| `settings` | `BDKSettings` | 11.1 |
| `history` | `BDKHistory` | 11.1 |
| `bookmarks` | `BDKBookmarks` | 11.1 |
| `downloads` | `BDKDownloads` | 11.1 |
| `permissions` | `BDKPermissions` | 11.1 (site settings) |
| `cookies` | `BDKCookies` | 11.1 |
| `privacy` | `BDKPrivacy` | 11.1 (clear browsing data) |
| `passwords` | `BDKPasswords` | 11.1 |
| `autofill` | `BDKAutofill` | 11.1 |
| `extensions` | `BDKExtensions` | [extensions.md](extensions.md) |
| `translation` | `BDKTranslationPreferences` | 11.1 |
| `search` | `BDKSearch` | 11.1 (Added, S1) |
| `favicons` | `BDKFavicons` | 11.1 |
| `topSites` | `BDKTopSites` | 11.1 |
| `scripts` | `BDKUserScripts` | 6.6 |

| Kind | Storage | Extensions | History | Notes |
|---|---|---|---|---|
| Persistent | On disk under the application's data directory | Its own set | Recorded | Any number |
| Private | In memory | The parent's, for extensions the user allowed in private | Not recorded | Child of one persistent profile. Several independent private profiles can exist under one parent. Mapping onto Chromium off-the-record is unverified. |

BDK has no "current" or "default" profile. The application chooses one when it creates a page.

## 6. WebContents (Plan, M2, T-ABI-04, T-SWF-02)

### 6.1 Creation and lifetime

| Member | Signature | Kind | Does |
|---|---|---|---|
| `create` | `profile.webContents.create(_ options: Options = .init()) throws -> BDKWebContents` | throws | New page in the profile's parking context |
| `close(_:)` | `func close(_ mode: CloseMode = .runUnloadHandlers) async -> CloseResult` | async | `.closed` or `.cancelledByPage` |
| `destroy()` | `func destroy()` | sync | Immediate, no unload handlers |
| `discard()` | `func discard()` | sync | Added, S15. Refused for a visible page. |
| `sessionState()` | `func sessionState() throws -> BDKSessionState` | throws | Section 6.8 |
| `keep(in:)` | `func keep(in context: BDKHostContext?)` | sync | Conditional, decision D1. Section 8.3. |

| `Options` field | Type | Default | Notes |
|---|---|---|---|
| `initialURL` | `URL?` | nil | |
| `restoring` | `BDKSessionState?` | nil | Section 6.8 |
| `deferInitialLoad` | `Bool` | `false` | Do not load until first shown or navigated |
| `opener` | `BDKWebContents?` | nil | |
| `userAgent` | `UserAgentOverride?` | nil | |
| `isInitiallyMuted` | `Bool` | `false` | |
| `hostContext` | `BDKHostContext?` | nil | Conditional, D1 |

| Lifetime state | Meaning |
|---|---|
| Created, never attached | In the profile's parking context. Loads and runs. `visibility == .hidden`. |
| Attached | View is in a window. Belongs to that window's host context. |
| Detached | View was removed. Background page of its last window, or parking if that window closed. |
| Crashed | Renderer is gone. Object stays valid; `reload()` starts a new renderer. |
| Discarded (Added, S15) | Renderer released to save memory. Object, id, URL, title, favicon and history stay valid. Reloads when its view next becomes visible, or on `reload()`. Form input and scroll may be lost. |
| Closed | Every call throws `stale`. |

### 6.2 State

All read-only unless marked, on the main actor, each with a matching event.

| Property | Type | Notes |
|---|---|---|
| `id` | `BDKWebContents.ID` | Stable for the life of the page. Not across launches. |
| `profile` | `BDKProfile` | |
| `url` | `URL?` | Last committed URL. Branded form for an internal page. |
| `displayURL` | `URL?` | What an address bar shows. Pending URL during user-initiated navigation, else `url`. Follows Chromium's anti-spoofing rules. |
| `title` | `String` | |
| `favicon` | `BDKFavicon?` | Image and source URL |
| `isLoading` | `Bool` | |
| `estimatedProgress` | `Double` | |
| `canGoBack`, `canGoForward` | `Bool` | |
| `security` | `SecurityState` | Level (`secure`, `neutral`, `insecure`, `dangerous`), certificate summary, interstitial showing, certificate exception |
| `audio` | `AudioState` | `isAudible`, `isMuted` |
| `capture` | `CaptureState` | Camera, microphone, screen capture in use, or being captured. A browser must show these. |
| `visibility` | `Visibility` | `.visible`, `.hidden`, `.occluded`. Follows the native view. |
| `isFocused` | `Bool` | |
| `isActiveInWindow` | `Bool` | See `architecture.md` 4.3 |
| `isCrashed` | `Bool` | |
| `isDiscarded` | `Bool` | Added, S15 |
| `isAutoDiscardable` | `Bool` (RW) | Added, S15. `false` exempts the page. Visible, active, audible, capturing and device-connected pages are never discarded automatically. |
| `zoom` | `ZoomState` | Factor, and whether it is the per-site default |
| `isFullscreen`, `isInPictureInPicture` | `Bool` | |
| `opener` | `BDKWebContents?` | |
| `window` | `BDKWindow?` | The window its view is in |
| `extensionTabID` | `Int` | What extensions see. Survives moves between windows. |
| `view` | `BDKWebView` | Always the same view |
| `navigation` | `Navigation` | Section 6.3 |
| `scripting` | `Scripting` | Section 6.5 |
| `capture`, `find`, `zoom`, `media`, `translation` | helpers | Section 6.7 |
| `devTools` | helper | Section 14 |

### 6.3 Navigation (`page.navigation`)

| Member | Signature | Kind | Does |
|---|---|---|---|
| `load` | `func load(_ url:, options: LoadOptions = .init()) async throws -> Commit` | async | Returns when the navigation commits. Throws when it fails or is superseded. |
| `reload` | `func reload(bypassingCache: Bool = false)` | sync | |
| `stop` | `func stop()` | sync | |
| `goBack`, `goForward` | `func goBack()`, `func goForward()` | sync | |
| `go(toOffset:)` | `func go(toOffset: Int)` | sync | |
| `go(to:)` | `func go(to entry: Entry)` | sync | |
| `entries` | `[Entry]` (R) | | url, title, timestamp, favicon |
| `currentIndex` | `Int` (R) | | |
| `prune(keeping:)` | `func prune(keeping: Range<Int>)` | sync | Drop entries, for "duplicate without history" |
| `page.navigate(to:)` | convenience | async | Same as `navigation.load(_:)` |

| `LoadOptions` field | Type | Default |
|---|---|---|
| `transition` | `.typed`, `.link`, `.bookmark`, `.generated`, `.reload`, `.other` | `.typed` |
| `referrer` | `URL?` | nil |
| `isUserInitiated` | `Bool` | `true` |
| `replacesCurrentEntry` | `Bool` | `false` |

| Rule | Detail |
|---|---|
| Failure | A failed navigation still shows Chromium's error page. The error says which. |
| Transition | History ranking and autocomplete use it. Address bar passes `.typed`, bookmark passes `.bookmark`. |
| Internal URLs | The application may navigate to any branded internal URL. A web page may not. |
| Native route | Calls the registered handler and throws `navigation.handledNatively`. |
| Later | A navigation policy hook. Not in v1. |

### 6.4 Events (`page.events`, AS)

| Event | Payload |
|---|---|
| `navigationStarted` | `NavigationInfo`: url, isMainFrame, isSameDocument, isUserInitiated |
| `navigationRedirected` | `NavigationInfo` |
| `navigationCommitted` | `Commit`: url, transition, httpStatus, isSameDocument, isErrorPage |
| `navigationFinished` | `Commit` |
| `navigationFailed` | `NavigationFailure`: url, netError, description |
| `urlChanged`, `titleChanged`, `faviconChanged`, `loadingChanged`, `progressChanged` | |
| `securityChanged`, `audioChanged`, `captureChanged`, `zoomChanged` | |
| `visibilityChanged`, `focusChanged`, `activeChanged` | |
| `fullscreenChanged`, `pictureInPictureChanged` | |
| `hoveredLinkChanged` | `URL?`. For a status bar. |
| `processGone` | `.crashed`, `.killed`, `.outOfMemory`, `.launchFailed` |
| `discarded` | Added, S15. `.memoryPressure`, `.memorySaver`, `.application`, `.extension` |
| `unresponsive`, `responsive` | |
| `closeRequested` | `window.close()`. The application decides. |
| `closed` | |

A renderer crash is an event, not an error and not a UI request. The application decides whether to reload, show a placeholder or replace the page.

### 6.5 Scripting (`page.scripting`; Plan, M7, T-AUT-03)

| Member | Signature | Does |
|---|---|---|
| `evaluate` | `func evaluate(_ source:, in: World = .isolated("app"), frame: FrameSelector = .main, userGesture: Bool = false) async throws -> BDKJSValue` | Run source |
| `call` | `func call(_ function:, arguments: [BDKJSValue], in:, frame:) async throws -> BDKJSValue` | Call a function |
| `insertCSS` | `func insertCSS(_ css:, frame: = .all) async throws -> InjectedStyle` | |
| `remove` | `func remove(_ style: InjectedStyle) async throws` | |
| `frames` | `func frames() async throws -> [FrameInfo]` | id, parent, url, origin |
| `query` | `func query(_ selector:, frame: = .main) async throws -> [ElementInfo]` | |
| `snapshot` | `func snapshot(_ kind: SnapshotKind) async throws -> Snapshot` | `.dom`, `.accessibility`, `.text` |

| Type | Cases |
|---|---|
| `World` | `.page`, `.isolated(String)` |
| `FrameSelector` | `.main`, `.all`, `.frame(FrameInfo.ID)` |

| Rule | Detail |
|---|---|
| Worlds | `.page` can be observed or tampered with by page scripts. `.isolated(name)` shares the DOM and nothing else. Isolated is the default. |
| Values | JSON-serialisable only. Anything else is an error, not a silent `null`. |
| Lifetime | Ends with the document, unless it is a registered user script. |
| Boundaries | Renderer sandbox and same-origin rules. No cross-origin power added. |
| Internal pages | Scripting a bundled or built-in internal page is refused (`contents.internalPage`). |

### 6.6 Persistent scripts (`profile.scripts`)

| Member | Signature | Does |
|---|---|---|
| `register` | `func register(_ script: UserScript) async throws` | Applies to matching pages on every navigation |
| `unregister` | `func unregister(_ id: String) async throws` | |
| `list` | `func list() async throws -> [UserScript]` | |

| `UserScript` field | Type |
|---|---|
| `id` | `String` |
| `matches`, `excludes` | `[String]` |
| `runAt` | `.documentStart`, `.documentEnd`, `.documentIdle` |
| `world` | `World` |
| `allFrames` | `Bool` |
| `javascript`, `css` | `[String]` |

Never matches internal pages or extension pages. Later: a message channel from an isolated world back to Swift, isolated world only.

### 6.7 Capture, find, zoom, media, translation, printing

| Helper | Member | Does | Milestone |
|---|---|---|---|
| `page.capture` | `screenshot(.visible \| .fullPage \| .rect(CGRect)) async throws` | Image | M7, T-SVC-09 (not built) |
| | `pdf(.init(paper:, landscape:, printBackground:)) async throws` | PDF | M7, T-SVC-09 (not built) |
| | `save(to:, as: .htmlOnly \| .completePage \| .singleFile) async throws` | Save page | M7, T-SVC-09 (not built) |
| `page.find` | `start(_ text, options:) throws` | AS of results: matchCount, activeIndex, rect, isFinal. Ends at `stop`, at the next `start`, and when the page closes | M3, T-SWF-07 |
| | `next() throws`, `previous() throws`, `stop(.clearSelection \| .keepSelection) throws` | | |
| | `present() throws` | Ask for the find surface | |
| `page.zoom` | `set(_ factor, scope: .site \| .page) throws` | | M4, T-SWF-06 (not built; the factor is readable) |
| `page.media` | `isMuted` (RW) | | M4, T-SWF-06 (read-only for now) |
| | `exitPictureInPicture()` | | M4, T-SWF-06 (not built) |
| `page.translation` | `state` | `.unavailable`, `.available(detected:)`, `.translating(progress:)`, `.translated(from:to:)`, `.failed(error)` | M6, T-PRV-03 |
| | `translate(to:) async throws` | | |
| | `revert() async throws` | | |
| | `present()` | Ask for the translation surface | |
| `page` | `print() throws` | Goes through the printing surface (a `PrintRequest` with `isUserInitiated` true) | M3, T-SWF-07 |

`present()` on find, translation and DevTools, and `print()`, start a flow whose UI is a browser-owned surface. The request takes the normal path in [ui-requests.md](ui-requests.md).

### 6.8 Session state (Plan, M2, T-ADP-07)

| Item | Detail |
|---|---|
| Save | `let state: BDKSessionState = try page.sessionState()` |
| Restore | `profile.webContents.create(.init(restoring: state, deferInitialLoad: true))` |
| Format | `Codable`, opaque, versioned bytes. Navigation entries with scroll positions and form data. |
| Compatibility | State from a newer engine may be refused by an older one with `contents.stateTooNew`. Fall back to the URL list in `state.summary`. |
| Storage | The application stores it, with its own layout. BDK stores none of that. |

## 7. Views (Plan, M2, T-MAC-03, T-SWF-03)

| Member | Signature | Does |
|---|---|---|
| `BDKWebView.contents` | `unowned let contents: BDKWebContents` | |
| `BDKWebView.keyboardDelegate` | `BDKKeyboardDelegate?` | Section 9 |
| `BDKWebView.focusContent()` | `func focusContent()` | Make the page first responder |
| `BDKWebContentsView` | SwiftUI `BDKWebContentsView(page)` | SwiftUI wrapper |

| Behaviour | Detail |
|---|---|
| One view per page | `page.view` always returns the same view. In one superview at a time. |
| Attaching | Adding the view to a window moves the page into that window's host context. No other call. |
| Moving | Adding it to another window moves it. History, renderer, extension tab identifier and `activeTab` grants survive. |
| Hiding | Hiding or removing reports `.hidden`. The page keeps running as a background page. |
| Sizing | Auto Layout or frames. |
| Transient UI | BDK closes page popups, `<select>` menus and HTML fullscreen before a move. |
| Accessibility | BDK sets the accessibility parent on attach. |
| Layer effects | Rounded corners, opacity and transforms on ancestor layers are untested (T-MAC-03). |
| Occlusion | Computed per window, not per view. A covered page still counts as visible. Hide the view to stop rendering. |

There is no BDK window, tab strip, toolbar or split-view type.

## 8. Windows and host contexts

### 8.1 Window registration (Added, S11; optional; not built: T-SWF-08, M5)

By default BDK creates a host context the first time a page view is added to an `NSWindow` and keeps it until the window closes.

| Member | Signature | Does |
|---|---|---|
| `runtime.windows.register` | `func register(_ nsWindow:, _ options: .init(kind:)) -> BDKWindow` | `kind`: `.normal`, `.popup` |
| `BDKWindow.nsWindow` | `NSWindow` | |
| `BDKWindow.kind` | `Kind` | What extensions see in `windows.get` |
| `BDKWindow.pages` | `[BDKWebContents]` (R) | Attached or last attached here |
| `BDKWindow.activePage` | `BDKWebContents?` (R) | |
| `BDKWindow.extensionWindowID` | `Int` (R) | |
| `BDKWindow.anchors` | `[AnchorRole: () -> CGRect]` (RW) | Where default presenters attach |
| `BDKWindow.delegate` | `weak BDKWindowDelegate?` | Requests that target the window |

| `AnchorRole` | Default presenter that uses it |
|---|---|
| `addressBar` | Permission prompts, password and translation offers |
| `extensions` | Default extensions menu |
| `downloads` | Downloads popover |

With no anchor, a default presenter uses the top centre of the page view.

| `BDKWindowDelegate` | Signature |
|---|---|
| `window(_:didReceive:)` | `func window(_ window: BDKWindow, didReceive request: WindowRequest) -> BDKUIResponse`. Return `.useDefault` to let BDK apply it. |

`WindowRequest`: `setFrame(CGRect)`, `activate`, `minimize`, `zoom`, `enterFullscreen`, `exitFullscreen`, `close`.

### 8.2 Active, focused, visible

No API call is involved. BDK reads focus and visibility from the views.

| Application action | Effect |
|---|---|
| A page view becomes first responder | That page becomes the active page of its window |
| A window becomes key | Its context becomes the most recently active one |
| Two page views visible in one window | Represented as an upstream split; both are foreground for translate, find and dialogs |
| Three or more visible | The focused one is active. Others follow the behaviour recorded for risk R2 until T-ADP-06. |

### 8.3 Host contexts (Conditional, decision D1, option (b) only)

| Member | Does |
|---|---|
| `runtime.hostContexts.make()` | New context with no meaning attached by BDK |
| `page.keep(in: context)` | Extensions see the page in a separate window while off screen |
| `page.keep(in: nil)` | Back to the default: a background page of its last window |

### 8.4 Windows Chromium asks for

Popups from `window.open`, undocked DevTools, picture-in-picture and extension-created windows arrive as new-window requests ([ui-requests.md](ui-requests.md) 5.6). BDK creates an `NSWindow` for page content only through the default presenter.

## 9. Keyboard and page commands (Plan, M2, T-MAC-03)

| `BDKKeyboardDelegate` method | When | Return |
|---|---|---|
| `webContents(_:shouldReserve:)` | Before the page sees the event | `true` keeps it from the page. For shortcuts a page must never capture. |
| `webContents(_:didNotHandle:)` | After the page declined it | `true` if the application handled it. Otherwise BDK sends it through the main menu and responder chain. |

| Page command API | Does |
|---|---|
| `page.canPerform(_ command: PageCommand) -> Bool` | Not built: T-SWF-06, M4 |
| `page.perform(_ command: PageCommand)` | Invoked by a menu item or toolbar button on the focused page. Not built: T-SWF-06, M4 |

| `PageCommand` group | Cases |
|---|---|
| Navigation | `back`, `forward`, `reload`, `reloadBypassingCache`, `stop` |
| Editing | `cut`, `copy`, `paste`, `pasteAndMatchStyle`, `undo`, `redo`, `selectAll`, `delete` |
| Find | `find`, `findNext`, `findPrevious` |
| Zoom | `zoomIn`, `zoomOut`, `zoomReset` |
| Page | `print`, `savePageAs`, `viewSource` |
| Toggles | `toggleMute`, `toggleDevTools`, `toggleFullscreen` |

Shortcuts, the menu bar and page cycling are the application's. Standard AppKit edit actions are expected to work through the responder chain when the page is first responder. Unverified outside Chromium's own window.

## 10. UI request handlers (Plan, M3, T-UIR-01 to T-UIR-06, T-SWF-04)

| Member | Signature | Does |
|---|---|---|
| `BDKUIHandlers.<surface>` | `Surface<Request>`, one per surface (`permissions`, `javascriptDialogs`, `contextMenus`, `newWindows`, …) | Full list in [ui-requests.md](ui-requests.md) |
| `Surface.setHandler(_:)` | `func setHandler(_ handler: (@MainActor (Request) -> BDKUIResponse)?) throws` | Set or clear. Throws `config.registrationAfterStart` for a surface that had no handler at start (T-SWF-04) |
| `Surface.mode` | `SurfaceMode` (R) | From `bdk.toml`: `.default`, `.custom`, `.disabled` |

| `BDKUIResponse` | Meaning |
|---|---|
| `.handled` | The application owns the request and completes it through the request object |
| `.useDefault` | BDK's default presenter shows it |
| `.reject` | Deny or cancel, where that is safe |

| Rule | Detail |
|---|---|
| `custom` mode | Set the handler on `config.ui` before start |
| `default` mode | A handler may be set or cleared at any time on `runtime.ui` |
| Return | A handler returns synchronously. Presentation is asynchronous, through the request object. |
| Dropped request | A taken request released without completing is cancelled. Never granted. |

## 11. Services and providers

### 11.1 Services (Plan, M3, T-SVC-01 to T-SVC-06)

Every command in [services.md](services.md) section 5 has a typed wrapper generated from the same interface definition. `history.search` is `profile.history.search(…)`. Arguments and results are structs; events are an `AsyncSequence` on the service object.

| Service | Examples from the spec | Events (AS) |
|---|---|---|
| `profile.history` | `search(_, in:, limit:, cursor:) async throws -> SearchResult`, `delete(urls:) async throws` | `.visited`, `.removed` |
| `profile.bookmarks` | `roots()`, `createFolder(_, in:, at:)`, `create(url:, title:, in:, at:)`, `move(_, to:, at:)` (all `async throws`) | `.created`, `.changed`, `.moved`, `.removed`, `.reordered` |
| `profile.downloads` | `pause(id)` and the rest of the manage set | `.created`, `.updated`, `.completed`, `.interrupted`, `.removed` |
| `profile.settings`, `runtime.settings` | `get(_ key)`, `get(_ key, as:)`, `set(_ key, _ value)` (keys are strings until T-SVC-05) | `.changed` |
| `profile.permissions` | `set(origin:, kind:, value:)` | `.changed` |
| `profile.privacy` | `clear(types:, range:, origins:, onProgress:)` | `.cleared` |
| `profile.passwords` | `list()` (metadata only), `reveal(_ id)` (needs OS authentication) | `.changed`, `.issuesChanged` |
| `profile.search` (Added, S1; not built: T-SVC-10, M4) | `classify(_) -> .url(URL) \| .search(URL, engine)`, `suggestions(for:, in:)` | `suggestions` is AS of batches |
| `runtime.system.defaultBrowser` | `status() async throws`, `request() async throws` (macOS asks the user) | |
| `runtime.about` | `info() async throws`, `caller() async throws` (`runtime.info`, `runtime.caller`) | |
| `runtime.commands` | `invoke(_ name, _ input, profile:) async throws -> Output` | Generic entry point for commands with no wrapper, including `app.` commands |

Swift calls are not capability-checked (`architecture.md` 9.1). The rules the generator follows, and the places this table departs from [swift-api.md](swift-api.md) 11.1, are in swift-api.md 11.1 ("Settled in T-SVC-02 to T-SVC-06").

### 11.2 Provider registration (Plan, M6, T-PRV-01 to T-PRV-09)

A provider is chosen in `bdk.toml`. Swift supplies keys, and the implementation when `provider = "app"`.

| Call | When |
|---|---|
| `config.services.google.apiKey = …` | Only if a service uses `provider = "google"` |
| `config.services.translation.setKey(_)` | Named provider that needs a key |
| `config.services.translation.register(_)` | `provider = "app"` |
| `config.services.reputation.register(_)` | `provider = "app"` |
| `config.services.geolocation.register(_)` | `provider = "app"` |
| `config.services.credentials.register(_)` | `provider = "app"`. Added, S14. |
| `config.services.leakCheck.register(_)` | `provider = "app"`. Added, S16. |

### 11.3 Provider protocols

All are `Sendable` and carry `var descriptor: ProviderDescriptor` (id, display name, privacy: `.onDevice` or `.remote(host:, policyURL:)`).

| Protocol | Method | Signature | Does |
|---|---|---|---|
| `BDKTranslationProvider` | `supportedPairs` | `async throws -> [LanguagePair]` | |
| | `prepare` | `(_ pair, progress:) async throws` | Model download, if any |
| | `translate` | `(_ texts, pair:, context:) async throws -> [String]` | |
| | `detectLanguage` | `(of:) async throws -> LanguageGuess?` | `nil` means use Chromium's detector |
| `BDKReputationProvider` | `check(_ url:, context:)` | `async -> Verdict` | |
| | `check(_ download:)` | `async -> Verdict` | |
| | `update` | `async throws` | List-based providers |
| `BDKGeolocationProvider` | `positions` | `(_ options) -> AsyncThrowingStream<Position, Error>` | |
| `BDKCredentialProvider` (Added, S14; unverified) | `credentials(for:)` | `async throws -> [Credential]` | Asked when a page with a login form loads |
| | `all` | `async throws -> [CredentialSummary]` | For a management page. No passwords. |
| | `save` | `(_ credential) async throws` | After the user confirmed a save or update |
| | `remove` | `(_ id) async throws` | |
| | `changes` | `AsyncStream<Void>` | Store changed or unlocked. Chromium re-queries. |
| `BDKLeakCheckProvider` (Added, S16; unverified) | `check` | `(_ candidate: LeakCheckCandidate) async throws -> LeakVerdict` | Called once after a sign-in and once per saved login during `passwords.check` |

| Supporting type | Cases or fields |
|---|---|
| `Verdict` | `safe`, `unknown`, `malware`, `phishing`, `unwantedSoftware`, `dangerousDownload` |
| `LeakVerdict` | `notFound`, `leaked` |
| `LeakCheckCandidate` | `origin`, `username`, `password` (clear), `reason` (`.signIn`, `.savedLoginsCheck`) |

| Rule | Detail |
|---|---|
| Same contract | Default UI, Swift and `@bdk/internal/translation` behave the same whichever provider is chosen. |
| Verdicts | A provider reports. It does not present. The interstitial cannot be suppressed. |
| Cancellation | Navigating away or reverting a translation cancels the provider's task. |
| Privacy descriptor | Shown in default UI wherever page content or a URL leaves the device. `.remote` with no policy URL is a start error. |
| Protection level | The default reputation provider's descriptor says "reduced protection". |
| Missing registration | `provider = "app"` with none registered is a start error. |
| Credentials | Chromium keeps form detection, save and suggestion requests and fill, and reads and writes through the provider instead of its login database. A locked store returns nothing and signals `changes` on unlock. Passwords reach the browser process when a form is found. `passwords.*` commands operate on the provider's data. |
| Leak check | The provider gets the password in clear and decides what is sent. A hash prefix compared on device is the expected form. A thrown error means "could not check" and flags nothing. With no provider there is no check, and BDK never calls Google's. |

## 12. Application commands, events and settings (Plan, M3, T-SVC-05)

Swift is the source of truth. Declarations are extracted at build time to generate TypeScript ([javascript-api.md](javascript-api.md) section 7).

| Piece | Form | Does |
|---|---|---|
| Command | `struct X: BDKCommand { static let name = "app.…"; struct Input: Codable, Sendable; struct Output: Codable, Sendable }` | Declares a command |
| Register | `config.commands.register(X.self) { input, context in … }` | Handler, before start |
| Event | `struct X: BDKEvent { static let name = "app.…" }` | Declares an event |
| Declare | `config.commands.declare(X.self)` | |
| Emit | `runtime.commands.emit(_, to: .all \| .profile(_) \| .page(_) \| .session(_))` | |
| Setting | `config.settings.register(BDKSettingKey<T>("app.…"), default:, scope: .profile, exposedToPages: Bool)` | Application setting, read through `settings.get` and `settings.set`; typed `profile.settings.get(key)` and `set(key, value)` (also on `runtime.settings`). Built in T-SVC-08 |

| `CommandContext` member | Type |
|---|---|
| `caller` | `Caller`: `.application`, `.page(route:, contents:)`, `.automation(session:)` |
| `profile` | `BDKProfile?` |
| `report(progress:message:)` | sync |
| `isCancelled` | `Bool` (R) |

| Rule | Detail |
|---|---|
| Name | Must start with `app.`. Anything else is a start error. |
| Capability | A caller needs the capability equal to the command name, or a matching `app.….*` wildcard. Checked before the handler runs. |
| Registration | Before start only. |
| Typing | `Codable` input and output. No raw Swift execution, no arbitrary selectors, no pointers. |
| Caller | The handler is told who called. |
| Confirmation | A command declared `requiresUserGesture` is refused from a page without a gesture in that page. |
| Event delivery | A page or session receives an application event only if it holds the capability with the event's name. |
| `exposedToPages` | `false` keeps a key out of reach of every page, whatever its capabilities. |

## 13. Automation (Plan, M7, T-AUT-01 to T-AUT-04)

| Member | Signature | Does |
|---|---|---|
| `runtime.automation.makeSession` | `func makeSession(_ options: .init(name:, capabilities:, scope:)) throws -> BDKAutomationSession` | `scope` (Added, S10): `.init(profiles:, contents: .all \| .created \| .explicit([page]))` |
| `session.run` | `func run(_ name:, arguments:) async throws -> Task` | |
| `task.events` | AS | `.progress`, `.log`, `.commandInvoked(name)` |
| `task.value` | `BDKJSValue` (`async`) | |
| `task.cancel()` | sync | |
| `session.grant(contents:)` | sync | Widen an explicit scope |
| `session.revoke()` | sync | Stops its tasks, closes its channel |
| `runtime.automation.events` | AS | Session created, revoked; every command a session invoked |
| `runtime.automation.external.open` | `func open(_ handler: (Hello) -> Decision) throws -> Endpoint` | Off unless `[automation.external] enabled = true`. The handler returns `.refuse` or `.accept(session:)`. |
| `endpoint.socketPath` | `String` | User-restricted local socket |
| `endpoint.close()` | sync | |

| BDK provides | The application provides |
|---|---|
| Sessions with a fixed capability set and scope | Who an agent is, what it may be granted, consent UI |
| The broker check on every call | Policy |
| Revocation | When to revoke |
| An audit event for every command a session invokes | Storage and display of the audit trail |
| A local, authenticated channel | The CLI, MCP or ACP gateway, token issuance |

A page created by a session starts in the parking context and is not on screen. The application receives `webContentsCreated(_, by: .automation(session))` and decides whether to show it.

## 14. DevTools and the protocol (Plan, M7, T-AUT-05)

| Member | Signature | Does |
|---|---|---|
| `page.devTools.present()` | sync | Goes through the DevTools surface |
| `page.devTools.close()` | sync | |
| `page.devTools.isOpen` | `Bool` (R) | |
| `page.devTools.attachProtocol()` | `async throws -> CDPSession` | Refused if `[devtools] protocol = false` |
| `cdp.send` | `func send(_ method: String, _ params: …) async throws -> …` | |
| `cdp.events` | AS | |
| `cdp.detach()` | sync | |

| `bdk.toml` `[devtools] frontend` | `present()` does |
|---|---|
| `"default"` | Opens Chromium's front end |
| `"default"` with `overlay` | Same, with the application's CSS and JavaScript loaded |
| `"<path>"` | Opens the application's packaged front end |
| `"disabled"` | Throws `devtools.disabled` |

The front end arrives as a DevTools request carrying a `BDKWebContents`. With no handler the default presenter opens an undocked window. Hosting the front end as an ordinary page view is unverified. The protocol is in-process only. Nothing listens on a network port.

## 15. Diagnostics (Added, S4; logging is not)

| Member | Signature | Does |
|---|---|---|
| `config.diagnostics.onCrashReport` | `(_ report) async` | With `crash_reports = "app"`. `report.fileURL` is a minidump. `report.process`: `.browser`, `.renderer`, `.gpu`, `.utility`. |
| `runtime.diagnostics.crashReports()` | sync | Completed reports on disk |
| `runtime.diagnostics.logURL` | `URL` (R) | Current log file |
| `runtime.diagnostics.startNetLog(to:)` | `async throws` | The user must be told it captures traffic metadata |
| `runtime.diagnostics.stopNetLog()` | `async throws` | |

BDK uploads nothing.

## 16. Errors

`BDKError: Error, Sendable` has `domain`, `code` (stable, lower camel case), `message` (for developers, not localised) and `details: [String: BDKJSValue]`.

| Domain | Typical codes |
|---|---|
| `abi` | `versionMismatch`, `hashMismatch` |
| `runtime` | `alreadyStarted`, `notStarted`, `runtimeMissing`, `runtimeTampered`, `engineTooOld`, `profileTooNew`, `shutdownInProgress` |
| `config` | `resolvedMissing`, `resolvedInvalid`, `registrationAfterStart`, `handlerMissing`, `providerMissing`, `keyMissing`, `nameNotApp`, `routeUnknown` |
| `profile` | `notFound`, `inUse`, `nameTaken`, `isPrivate` |
| `contents` | `stale`, `crashed`, `stateTooNew`, `internalPage` |
| `navigation` | `aborted`, `superseded`, `blocked`, `handledNatively`, `netError` |
| `broker` | `unknownCommand`, `invalidArguments`, `cancelled`, `timeout`, `payloadTooLarge` |
| `capability` | `denied`, `gestureRequired`, `reauthenticationFailed`, `outOfScope` |
| `extension` | `incompatible`, `sourceNotAllowed`, `notFound`, `surfaceDisabled`, `installCancelled` |
| `provider` | `unavailable`, `unsupportedPair`, `modelDownloadFailed`, `remoteError` |
| `automation` | `disabled`, `hostGone`, `sessionRevoked`, `scriptNotFound`, `scriptFailed` |
| `ui` | `requestCompleted`, `gestureRequired`, `surfaceDisabled`, `surfaceMode`, `surfaceUnknown`, `completionNotAllowed`, `notOverridable`, `widensRequest` |
| `devtools` | `disabled`, `protocolDisabled`, `alreadyAttached` |
| `platform` | `keychainUnavailable`, `notDefaultBrowserCandidate` |

Start errors are `abi`, `runtime` and `config`. Show one instead of a blank window. `bdk doctor` reports most of them before launch.

## 17. Test support (`BDKTestSupport`, Plan, T-ABI-02, T-SWF-01)

| Member | Does |
|---|---|
| `BDKRuntime.startStub(.init(routes:, capabilities:))` | Starts the in-memory runtime |
| `runtime.stub.simulateNavigation(_, to:, title:)` | |
| `runtime.stub.raise(_ request, on: page)` | e.g. `PermissionRequest.fixture(.camera, origin:)` |

The stub implements the C ABI in memory, passes the same conformance suite as the real runtime, renders nothing and runs no JavaScript. Offering it to applications is proposed.

## 18. Where other Swift surfaces are defined

| Surface | Document |
|---|---|
| Every UI request type, payload and completion | [ui-requests.md](ui-requests.md) |
| `BDKExtensions` and extension UI | [extensions.md](extensions.md) |
| Service command list, capabilities, settings catalogue | [services.md](services.md) |
| Package products (`BDK`, `BDKTestSupport`) and the `bdk` executable | `swift-api.md` section 2 |
| A complete small browser (about 60 lines) | `swift-api.md` section 18 |
