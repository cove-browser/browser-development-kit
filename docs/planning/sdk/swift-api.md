# BDK Swift API

Status: proposal, 2026-11-15. Signatures are illustrative and have not been compiled. Type and method names are placeholders; names already used in the spec or in [../architecture.md](../architecture.md) are kept. Status labels are in [README.md](README.md) section 2. The Swift SDK is tasks T-SWF-01 to T-SWF-05, over the C ABI in [c-abi.md](c-abi.md). A table form for lookup is in [swift-api-reference.md](swift-api-reference.md).

## 1. Conventions

| Topic | Rule |
|---|---|
| Actor | Every public type is `@MainActor`. Chromium's UI sequence is the main thread, and the C ABI is called only from there. |
| Asynchrony | Work that crosses into Chromium and can take time is `async throws`. Cancelling the Swift task cancels the native work. |
| Events | Each object exposes `events`, an `AsyncSequence`. Several consumers may iterate; each gets every event from the moment it starts. State is also readable as plain properties, so a late subscriber can read the current value. |
| Ownership | A Swift object holds one reference to a native handle. `BDKWebContents` and private profiles have an explicit `close` or `destroy`. Dropping the last Swift reference to a page without closing it closes the page and logs a warning in debug builds. |
| Stale objects | Using an object after it was destroyed throws `BDKError` with code `stale`. It never crashes and never reaches a different object, because handles are generation-checked. |
| Errors | Every throwing call throws `BDKError` (section 16). |
| Values | Requests, events and results are value types and `Sendable`. Objects with identity (`BDKRuntime`, `BDKProfile`, `BDKWebContents`, `BDKWindow`) are classes. |
| Naming | Top-level types carry the `BDK` prefix, as in the spec. Nested types do not. |
| Stability | Experimental until v1. |

## 2. Package

One SwiftPM package with a binary target for the runtime (tooling document 8.1).

| Product | Contents | Status |
|---|---|---|
| `BDK` | Everything in this document except section 17 | Plan |
| `BDKTestSupport` | The stub runtime from `packages/core` and helpers for unit-testing an application without Chromium | Plan (T-ABI-02, T-SWF-01); offering it to applications is proposed |

The package also vends the `bdk` executable, so the build phase uses the CLI that matches the SDK ([cli.md](cli.md)).

## 3. Application object

Plan, M0 and M2, tasks T-MAC-01 and T-MAC-02.

```swift
@MainActor
open class BDKApplication: NSApplication {
    // Conforms to Chromium's application protocols and routes sendEvent(_:) through Chromium's handling.
    // Subclasses that override sendEvent(_:) must call super.
}
```

The application sets `NSPrincipalClass` to `BDKApplication` or to its own subclass. SwiftUI applications do the same in `Info.plist` and use `@NSApplicationDelegateAdaptor` for the delegate. The application owns the delegate, the main menu, the run loop and every window. BDK installs none of them.

| The application does | BDK does |
|---|---|
| Starts normally; calls `BDKRuntime.start` from `applicationWillFinishLaunching` | Checks the ABI, registers the scheme, starts Chromium nested in the running loop, holds a keep-alive |
| Builds its menus and windows | Nothing |
| Handles `application(_:open:)` for URLs and files from Launch Services | Nothing. Opening a URL is "create or reuse a page and navigate". |
| Returns `runtime.terminate(replyingTo:)` from `applicationShouldTerminate` | Cancels that attempt, runs unload handlers, closes host contexts, stops Chromium, then terminates the application again (the second `applicationShouldTerminate` gets `.terminateNow`). Not `.terminateLater`: AppKit's reply loop would nest inside Chromium's and block its shutdown |

## 4. Configuration and runtime

Plan, M2, tasks T-ABI-04 and T-SWF-01.

### 4.1 `BDKConfiguration`

```swift
public struct BDKConfiguration {
    /// Reads and verifies bdk.resolved.json from the bundle.
    public static func load(from bundle: Bundle = .main) throws -> BDKConfiguration

    public let browser: Browser                  // name, internalScheme; read-only
    public var pages: PageRegistrations          // 4.2
    public var commands: CommandRegistrations    // 12
    public var settings: SettingRegistrations    // 12.3
    public var services: ServiceRegistrations    // 11.2
    public var ui: BDKUIHandlers                 // 10; required here for surfaces in custom mode
    public var diagnostics: DiagnosticsRegistrations   // 15
    public var launch: LaunchOptions             // debug and test overrides
}

public struct LaunchOptions {
    public var userDataDirectory: URL?           // tests only; ignored in release builds
    public var logLevel: LogLevel?               // debug builds only
}
```

Nothing in `BDKConfiguration` can change what `bdk.toml` decided about capabilities, surfaces or sources. It adds the things TOML cannot hold: code and secrets.

### 4.2 Page registrations

Defined in `architecture.md` 8.2. Repeated here for completeness.

```swift
public struct PageRegistrations {
    public mutating func register(_ route: String, _ mode: PageMode)
}

public enum PageMode {
    case chromium                                   // serve the built-in page
    case bundled                                    // serve the bundled page already in the resolved file
    case native(@MainActor (NativePageRequest) -> NativePageResult)
    case disabled
}

public struct NativePageRequest {
    public let route: String          // "downloads"
    public let url: URL               // cove://downloads/recent?x=1
    public let path: String           // "/recent"
    public let query: [URLQueryItem]
    public let source: BDKWebContents? // the page whose navigation asked for it
    public let profile: BDKProfile
}

public enum NativePageResult { case handled, blocked }
```

A `native` route loads no page. The navigation is cancelled in the page that asked, and the handler decides what the user sees. `blocked` shows the standard error page.

Built (T-SWF-06, 2026-11-20), done against the stub; the engine side is written and waits for an engine build. `BDKRuntime.start` registers `config.pages` with `bdk_page_handler_register` between the runtime start and the freeze, so a refusal is the start error with the runtime's domain and code (`config.noBuiltin`, `routeUnknown`, `routeInvalid`, `routeMode`, and `handlerMissing` at the freeze). Registering a route twice in Swift keeps the last registration, so one registration per route reaches the ABI. `source` is the application's `BDKWebContents` for the page that asked, or nil when the application does not hold it; `query` is percent-decoded. A request whose profile the application does not hold is answered `blocked`.

### 4.3 `BDKRuntime`

```swift
@MainActor
public final class BDKRuntime {
    /// Synchronous: returns when Chromium's browser process is initialised in this process.
    @discardableResult
    public static func start(_ configuration: BDKConfiguration) throws -> BDKRuntime
    public static var shared: BDKRuntime { get }   // traps before start
    public static var isRunning: Bool { get }

    public let info: Info
    public let profiles: BDKProfiles          // 5
    public let contents: BDKWebContentsRegistry // every live page, lookup by id
    public let windows: BDKWindows            // 8
    public let ui: BDKUIHandlers              // 10
    public let commands: BDKCommands          // 12
    public let automation: BDKAutomation      // 13
    public let diagnostics: BDKDiagnostics    // 15
    public let system: BDKSystem              // default-browser registration
    public var events: Events { get }         // AsyncSequence<RuntimeEvent>

    public func terminate(replyingTo application: NSApplication) -> NSApplication.TerminateReply
    public func shutdown(_ options: ShutdownOptions = .init()) async -> ShutdownResult
}

public struct Info: Sendable {
    public let bdkVersion: String             // "0.4.0"
    public let chromiumVersion: String        // "158.0.8127.37"
    public let abiVersion: UInt32
    public let abiHash: String
    public let manifestDigest: String
    public let mode: RuntimeMode              // .prebuilt, .source
    public let isTestedConfiguration: Bool    // false when feature overrides are set
}

public enum RuntimeEvent: Sendable {
    case gpuProcessGone(ProcessGoneReason)         // Chromium restarts it
    case utilityProcessGone(name: String, ProcessGoneReason)
    case automationHostGone(ProcessGoneReason)     // sessions have ended with an error
    case memoryPressure(MemoryPressureLevel)
    case webContentsCreated(BDKWebContents, by: Creator)   // .application, .automation(session), .extension(id); built in M8 for sessions only (.extension: T-EXT-12)
    case webContentsDestroyed(BDKWebContents.ID)
}

public enum ShutdownResult { case completed, cancelled(by: BDKWebContents) }  // an unload handler kept its page open
```

Rules:

- `start` either succeeds or throws with nothing left running. There is no partial start (`architecture.md` section 15).
- `start` can be called once per process. Chromium cannot be restarted in the same process after `shutdown`.
- After `start`, the route table, the capability table and provider choices are frozen. `config.pages.register` has no later equivalent on `BDKRuntime`.
- `shutdown` runs `beforeunload` handlers. If one keeps its page open, the result is `cancelled` and the runtime keeps running.

## 5. Profiles

Plan, M2, task T-SWF-02. A profile is a storage context: cookies, cache, permissions, history, bookmarks, passwords, extensions and settings.

```swift
@MainActor
public final class BDKProfiles {
    public func list() -> [ProfileDescriptor]                       // every profile on disk
    public func create(name: String) async throws -> ProfileDescriptor   // the profiles.create command
    public func open(_ name: String, createIfMissing: Bool = true) async throws -> BDKProfile   // convenience over list, create, open(id:)
    public func open(id: BDKProfile.ID) async throws -> BDKProfile
    public var loaded: [BDKProfile] { get }
    public func rename(_ id: BDKProfile.ID, to name: String) async throws
    public func delete(_ id: BDKProfile.ID) async throws           // refused while pages exist in it
    public var events: Events { get }                               // created, loaded, renamed, deleted
}

public struct ProfileDescriptor: Sendable, Identifiable {
    public let id: BDKProfile.ID     // stable across launches
    public let name: String
    public let lastUsed: Date?
}

@MainActor
public final class BDKProfile: Identifiable {
    public let id: ID
    public var name: String { get }
    public let kind: Kind                       // .persistent, .private(parent: BDKProfile)
    public var isPrivate: Bool { get }

    /// An off-the-record child. Nothing it stores survives close().
    public func openPrivate(_ label: String? = nil) throws -> BDKProfile
    public func close() async throws            // private profiles only; refused while pages exist

    public let webContents: WebContentsFactory  // create(_:), all
    // Services, section 11
    public let settings: BDKSettings
    public let history: BDKHistory
    public let bookmarks: BDKBookmarks
    public let downloads: BDKDownloads
    public let permissions: BDKPermissions      // site settings
    public let cookies: BDKCookies
    public let privacy: BDKPrivacy              // clear browsing data
    public let passwords: BDKPasswords
    public let autofill: BDKAutofill
    public let extensions: BDKExtensions        // extensions.md
    public let translation: BDKTranslationPreferences
    public let search: BDKSearch
    public let favicons: BDKFavicons
    public let topSites: BDKTopSites
    public let scripts: BDKUserScripts          // 6.6
}
```

| Kind | Storage | Extensions | History | Notes |
|---|---|---|---|---|
| Persistent | On disk under the application's data directory | Its own set | Recorded | Any number. The application decides what a profile means to the user. |
| Private | In memory | The parent's, for extensions the user allowed in private | Not recorded | A child of one persistent profile. Several independent private profiles can exist under one parent, each with its own cookies. |

The private kind is expected to map onto a Chromium off-the-record profile. That mapping, and several under one parent, are unverified.

BDK has no notion of a "current" or "default" profile. The application chooses one when it creates a page.

## 6. WebContents

Plan, M2, tasks T-ABI-04 and T-SWF-02. A `BDKWebContents` is one page with its own navigation history and renderer. Internally it is always a Chromium tab; that never appears in the API.

### 6.1 Creation and lifetime

```swift
let page = try profile.webContents.create()
let page = try profile.webContents.create(.init(restoring: saved, deferInitialLoad: true))

public struct Options {
    public var initialURL: URL?
    public var restoring: BDKSessionState?      // 6.8
    public var deferInitialLoad: Bool = false   // do not load until first shown or navigated
    public var opener: BDKWebContents?
    public var userAgent: UserAgentOverride?
    public var isInitiallyMuted: Bool = false
    public var hostContext: BDKHostContext?     // Conditional on decision D1; section 8.3
}
```

```swift
public func close(_ mode: CloseMode = .runUnloadHandlers) async -> CloseResult   // .closed, .cancelledByPage
public func destroy()                                                            // immediate; no unload handlers
```

| State | Meaning |
|---|---|
| Created, never attached | Lives in the profile's parking context. Loads and runs normally. Reports `visibility == .hidden`. |
| Attached | Its view is in a window. Belongs to that window's host context. |
| Detached | Its view was removed. It stays a background page of the window it was last in, or returns to parking if that window closed. |
| Crashed | The renderer is gone. The object stays valid; `reload()` starts a new renderer. |
| Discarded (Added, S15) | Chromium, the application or an extension released the page's renderer to save memory. The object, its identifier, URL, title, favicon and navigation history stay valid. It reloads when its view next becomes visible, or on `reload()`. Form input and scroll position may be lost. |
| Closed | Every call throws `stale`. |

### 6.2 State

All read-only, all on the main actor, each with a matching event.

| Property | Type | Notes |
|---|---|---|
| `id` | `BDKWebContents.ID` | Stable for the life of the page. Not stable across launches. |
| `profile` | `BDKProfile` | |
| `url` | `URL?` | Last committed URL. The branded form for an internal page. |
| `displayURL` | `URL?` | What an address bar should show: the pending URL during a user-initiated navigation, otherwise `url`. Follows Chromium's rules against spoofing. |
| `title` | `String` | |
| `favicon` | `BDKFavicon?` | Image and source URL |
| `isLoading`, `estimatedProgress` | `Bool`, `Double` | |
| `canGoBack`, `canGoForward` | `Bool` | |
| `security` | `SecurityState` | Level (`secure`, `neutral`, `insecure`, `dangerous`), certificate summary, whether an interstitial is showing, whether the user has made a certificate exception |
| `audio` | `AudioState` | `isAudible`, `isMuted` |
| `capture` | `CaptureState` | Whether the page is using the camera, microphone, screen capture, or is being captured. A browser must show these. |
| `visibility` | `Visibility` | `.visible`, `.hidden`, `.occluded`. Follows the native view. |
| `isFocused` | `Bool` | |
| `isActiveInWindow` | `Bool` | Whether Chromium treats it as the active page of its window. See `architecture.md` 4.3. |
| `isCrashed` | `Bool` | |
| `isDiscarded` | `Bool` | Added (S15). See 6.1. |
| `isAutoDiscardable` | `Bool`, settable | Added (S15). `false` exempts the page from Chromium's automatic discarding, for example a pinned page. Visible pages, the active page of each window, and pages that are audible, capturing or connected to a device are never discarded automatically. |
| `zoom` | `ZoomState` | Factor, and whether it is the per-site default |
| `isFullscreen`, `isInPictureInPicture` | `Bool` | |
| `opener` | `BDKWebContents?` | |
| `window` | `BDKWindow?` | The window its view is in |
| `extensionTabID` | `Int` | The identifier extensions see. Survives moves between windows. |

### 6.3 Navigation

```swift
public final class Navigation {
    @discardableResult
    public func load(_ url: URL, options: LoadOptions = .init()) async throws -> Commit
    public func reload(bypassingCache: Bool = false)
    public func stop()
    public func goBack()
    public func goForward()
    public func go(toOffset offset: Int)
    public func go(to entry: Entry)
    public var entries: [Entry] { get }          // url, title, timestamp, favicon
    public var currentIndex: Int { get }
    public func prune(keeping range: Range<Int>) // drop entries, for "duplicate without history" and similar
}

public struct LoadOptions {
    public var transition: Transition = .typed   // .typed, .link, .bookmark, .generated, .reload, .other
    public var referrer: URL?
    public var isUserInitiated: Bool = true
    public var replacesCurrentEntry: Bool = false
}
```

`page.navigate(to:)` is a convenience for `page.navigation.load(_:)`, as in the spec's example.

- `load` returns when the navigation commits, and throws when it fails or is superseded. A failed navigation still shows Chromium's error page; the error says which.
- The transition matters. History ranking and autocomplete use it, so an address bar passes `.typed` and a bookmark passes `.bookmark`.
- The application may navigate to any branded internal URL. A web page may not, exactly as with `chrome://` today.
- A navigation to a route in `native` mode calls the registered handler and throws `navigation.handledNatively`.

Later: a navigation policy hook that lets the application cancel or redirect a navigation before it starts. Not in v1; new-window requests cover the common case.

### 6.4 Events

```swift
for await event in page.events { … }

public enum Event: Sendable {
    case navigationStarted(NavigationInfo)        // url, isMainFrame, isSameDocument, isUserInitiated
    case navigationRedirected(NavigationInfo)
    case navigationCommitted(Commit)              // url, transition, httpStatus, isSameDocument, isErrorPage
    case navigationFinished(Commit)
    case navigationFailed(NavigationFailure)      // url, netError, description
    case urlChanged, titleChanged, faviconChanged, loadingChanged, progressChanged
    case securityChanged, audioChanged, captureChanged, zoomChanged
    case visibilityChanged, focusChanged, activeChanged
    case fullscreenChanged, pictureInPictureChanged
    case hoveredLinkChanged(URL?)                 // for a status bar
    case processGone(ProcessGoneReason)           // .crashed, .killed, .outOfMemory, .launchFailed
    case discarded(DiscardReason)                 // Added (S15): .memoryPressure, .memorySaver, .application, .extension
    case unresponsive, responsive
    case closeRequested                           // window.close(); the application decides
    case closed
}
```

Discarding (Added, S15). Every page is a Chromium tab, so Chromium's own discarding applies: under critical memory pressure it discards a hidden page, and with Memory Saver on (`performance.memorySaver.*` settings, off by default) it discards pages that have been hidden for a while. `page.discard()` does the same on request and is refused for a visible page. BDK keeps the `BDKWebContents` valid across a discard.

A renderer crash is an event, not an error and not a UI request. The application decides whether to reload, show its own placeholder or replace the page. Other pages are unaffected.

### 6.5 Scripting

Plan, M7, task T-AUT-03; written in M7; passes on the stub, engine build and test pending (T-AUT-06). The same operations are available to automation sessions as `contents.*` commands. As built (`packages/swift/README.md`): `.isolated("")` and `.isolated("page")` are refused; `.all` sends no frame except to `insertCSS`; `click`, `hover`, `type`, `press`, `scroll` and `waitFor` are built (T-AUT-07): input takes an `InputTarget` (`.selector`, `.point`) and `waitFor` a `WaitCondition` (`.load`, `.url`) and a `Duration`, as listed in `packages/swift/README.md`.

```swift
public final class Scripting {
    public func evaluate(_ source: String, in world: World = .isolated("app"),
                         frame: FrameSelector = .main, userGesture: Bool = false) async throws -> BDKJSValue
    public func call(_ function: String, arguments: [BDKJSValue], in world: World = .isolated("app"),
                     frame: FrameSelector = .main) async throws -> BDKJSValue
    public func insertCSS(_ css: String, frame: FrameSelector = .all) async throws -> InjectedStyle
    public func remove(_ style: InjectedStyle) async throws
    public func frames() async throws -> [FrameInfo]            // id, parent, url, origin
    public func query(_ selector: String, frame: FrameSelector = .main) async throws -> [ElementInfo]
    public func snapshot(_ kind: SnapshotKind) async throws -> Snapshot   // .dom, .accessibility, .text
}

public enum World { case page; case isolated(String) }
public enum FrameSelector { case main, all, frame(FrameInfo.ID) }
```

| Rule | Detail |
|---|---|
| Worlds | `.page` runs with the page's own scripts and can be observed or tampered with by them. `.isolated(name)` shares the DOM and nothing else. Isolated is the default. |
| Values | Arguments and results are JSON-serialisable. Anything else is an error, not a silent `null`. |
| Lifetime | Injected CSS and evaluated state end with the document. Nothing persists across a navigation unless it is a registered user script (6.6). |
| Boundaries | Scripts run inside the renderer's sandbox and same-origin rules. BDK adds no cross-origin power. |
| Internal pages | Scripting a bundled or built-in internal page is refused. Their privilege comes from page identity, and injected code would borrow it. |

### 6.6 Persistent scripts

```swift
try await profile.scripts.register(
    UserScript(id: "reader",
               matches: ["https://*/*"], excludes: [],
               runAt: .documentEnd,                 // .documentStart, .documentEnd, .documentIdle
               world: .isolated("reader"),
               allFrames: false,
               javascript: [source], css: []))
try await profile.scripts.unregister("reader")
let all = try await profile.scripts.list()
```

Registered per profile, applied to matching pages on every navigation, removed on unregister. They never match internal pages or extension pages.

Later: a message channel from an isolated world back to Swift. It must exist only in the isolated world, so that invariant 2, "ordinary websites receive no BDK interface", still holds.

### 6.7 Capture, find, zoom, media, translation

```swift
// Capture — M3, T-SVC-06
let image = try await page.capture.screenshot(.visible)                 // .visible, .fullPage, .rect(CGRect)
let pdf   = try await page.capture.pdf(.init(paper: .a4, landscape: false, printBackground: true))
try await page.capture.save(to: fileURL, as: .completePage)             // .htmlOnly, .completePage, .singleFile

// Find in page — M3, T-UIR-06; started by the application, T-SWF-07
for await result in try page.find.start("needle", options: .init(matchCase: false)) { … }   // matchCount, activeIndex, rect, isFinal
try page.find.next(); try page.find.previous(); try page.find.stop(.clearSelection)
try page.find.present()                // ask for the find surface; default presenter or the application's handler

// Zoom
try page.zoom.set(1.25, scope: .site)  // .site (Chromium's per-site default) or .page

// Media
page.media.isMuted = true
page.media.exitPictureInPicture()

// Translation — M6, T-PRV-03
page.translation.state                 // .unavailable, .available(detected:), .translating(progress:), .translated(from:to:), .failed(error)
try await page.translation.translate(to: "en")
try await page.translation.revert()
page.translation.present()             // ask for the translation surface

// Printing — M3, T-UIR-04
try page.print()                       // goes through the printing surface
```

`present()` on find, translation and DevTools, and `print()`, are how an application-owned menu item or shortcut starts a flow whose UI is a browser-owned surface. The request then takes the normal path in [ui-requests.md](ui-requests.md).

`page.translation` is built (M6, T-PRV-03; against the stub, engine pending) as `state() async throws`, `translate(to:from:) async throws`, `revert() async throws` and `present() async throws`, with state changes as the page event `.translation(BDKTranslationState)`.

`page.capture` is built in M7 (T-SVC-09; on the engine, build and test pending: T-AUT-06): screenshots are PNG, PDF sizes in inches. Built against the stub (T-SWF-06, 2026-11-20; the engine side is written and waits for an engine build): `page.zoom.set(_:scope:)` and `page.media.isMuted` (settable; the setter ignores a failure on a closed page, `setMuted(_:)` throws) with `exitPictureInPicture()`. `page.zoom` is now a `ZoomControl`: the factor is `page.zoom.factor`. `page.isMuted` and `setMuted(_:)` stay. Find and printing are built (T-SWF-07): the calls throw `contents.stale` on a closed page, like the other page calls. `find.start` returns the results of that search as a stream: one report when it starts and one for each move of the active match, each with `matchCount`, `activeIndex` (from 1, 0 for none), `rect` in the page view's coordinates and `isFinal` (the engine reports while it is still counting). The stream ends at `stop`, at the next `start` and when the page closes. `FindRequest.session` is the same `page.find`. The stub renders nothing, so its searches always report zero matches.

### 6.8 Session state

Plan, M2, task T-ADP-07. Chromium restores nothing on its own. The application saves and restores.

```swift
let state: BDKSessionState = try page.sessionState()   // Codable; opaque, versioned bytes
let restored = try profile.webContents.create(.init(restoring: state, deferInitialLoad: true))
```

- The state holds the navigation entries with their scroll positions and form data, as Chromium serialises them.
- It is tied to the engine. State saved by a newer engine may be refused by an older one with `contents.stateTooNew`; the application then falls back to the URL list in `state.summary`.
- The application stores it wherever it stores its own layout: which window, which pane, pinned or not. BDK stores none of that.

## 7. Views

Plan, M2, tasks T-MAC-03 and T-SWF-03.

```swift
@MainActor
public final class BDKWebView: NSView {
    public unowned let contents: BDKWebContents
    public var keyboardDelegate: BDKKeyboardDelegate?      // section 9
    public func focusContent()                             // make the page first responder
}

// AppKit
containerView.addSubview(page.view)

// SwiftUI
BDKWebContentsView(page)
```

| Behaviour | Detail |
|---|---|
| One view per page | `page.view` always returns the same view. A view is in one superview at a time, so a page is visible in one place. |
| Attaching | Adding the view to a window's hierarchy moves the page into that window's host context through the tab model. No other call is needed. |
| Moving | Adding it to another window moves it. Navigation state, renderer, extension tab identifier and `activeTab` grants survive. |
| Hiding | Hiding the view or removing it reports `.hidden`. The page keeps running as a background page. |
| Sizing | Auto Layout or frames, like any view. |
| Transient UI | BDK closes page popups, `<select>` menus and HTML fullscreen before a move. |
| Accessibility | BDK sets the accessibility parent on attach. |
| Layer effects | Rounded corners, opacity and transforms on ancestor layers are untested. Task T-MAC-03 records what works. |
| Occlusion | Chromium computes occlusion per window, not per view. A page covered by a sibling view still counts as visible; hide the view to stop it rendering. |

There is no BDK window, tab strip, toolbar or split-view type. A split is two views in the application's layout.

## 8. Windows and host contexts

### 8.1 What BDK needs to know about a window

Nothing, by default. The first time a page view is added to an `NSWindow`, BDK creates a host context for that window and keeps it until the window closes.

Registration is optional and adds three things (Added, S11). Built in M5 against the stub (T-SWF-08); the engine side is written, engine build and test pending. Departures (a weak `nsWindow`, `extensionWindowID: Int?`, `WindowRequest` as a struct naming the extension) are in `packages/swift/README.md`.

```swift
let window = runtime.windows.register(nsWindow, .init(kind: .normal))   // .normal, .popup
window.anchors[.addressBar] = { [weak toolbar] in toolbar?.addressFieldRectInWindow ?? .zero }
window.anchors[.extensions] = { … }
window.delegate = self

@MainActor
public final class BDKWindow {
    public let nsWindow: NSWindow
    public let kind: Kind
    public var pages: [BDKWebContents] { get }      // attached or last attached here
    public var activePage: BDKWebContents? { get }
    public var extensionWindowID: Int { get }
    public var anchors: [AnchorRole: () -> CGRect]
    public weak var delegate: BDKWindowDelegate?
}
```

| Addition | Used for |
|---|---|
| Anchors | Where default presenters attach. Roles: `addressBar` (permission prompts, password and translation offers), `extensions` (the default extensions menu), `downloads` (the downloads popover). With no anchor, a default presenter uses the top centre of the page view. |
| Kind | What extensions see in `windows.get`. |
| Delegate | Requests that target the window instead of a page. |

```swift
public protocol BDKWindowDelegate: AnyObject {
    // An extension or a page asked to change the window. Return .useDefault to let BDK apply it to the NSWindow.
    func window(_ window: BDKWindow, didReceive request: WindowRequest) -> BDKUIResponse
}

public enum WindowRequest {
    case setFrame(CGRect), activate, minimize, zoom, enterFullscreen, exitFullscreen, close
}
```

### 8.2 Active, focused, visible

`architecture.md` 4.3 defines the mapping. What the application sees:

| Application action | Effect |
|---|---|
| A page view becomes first responder | That page becomes the active page of its window |
| A window becomes key | Its context becomes the most recently active one |
| Two or more page views are visible in one window | The focused one is the window's active tab. Every one on screen is foreground for find, JavaScript dialogs, permission prompts and translation offers (M8, T-ADP-06; no upstream split, amendments 2.18). Extensions see the focused one as the active tab and the others as background tabs (risk R2) |

No API call is involved. BDK reads focus and visibility from the views.

### 8.3 Host contexts (Conditional, decision D1)

Only if D1 takes option (b). One generic call, with no meaning attached by BDK:

```swift
let context = runtime.hostContexts.make()
page.keep(in: context)     // extensions now see this page in a separate window while it is off screen
page.keep(in: nil)         // back to the default: a background page of its last window
```

An application that never calls it gets the default.

### 8.4 Windows Chromium asks for

Popups from `window.open`, undocked DevTools, picture-in-picture and extension-created windows arrive as new-window requests ([ui-requests.md](ui-requests.md) 5.6). BDK never creates an `NSWindow` for page content unless the default presenter is used.

## 9. Keyboard, shortcuts and page commands

Plan, M2, task T-MAC-03.

```swift
public protocol BDKKeyboardDelegate: AnyObject {
    /// Before the page sees the event. Return true to keep it from the page.
    func webContents(_ page: BDKWebContents, shouldReserve event: NSEvent) -> Bool
    /// After the page declined it. Return true if the application handled it.
    func webContents(_ page: BDKWebContents, didNotHandle event: NSEvent) -> Bool
}
```

| Step | Who |
|---|---|
| 1. Reserved shortcuts | `shouldReserve`. For shortcuts a page must never capture, such as new page, close page, quit. |
| 2. The page | Sees the key event, and may handle it. |
| 3. Unhandled | `didNotHandle`. With no delegate, or when it returns false, BDK sends the event on through the main menu and the responder chain, so ordinary menu key equivalents work. |

Shortcuts, the menu bar and behaviour such as cycling pages are the application's. BDK defines none.

Page commands are what a menu item or toolbar button invokes on the focused page:

```swift
page.canPerform(.print)
page.perform(.print)

public enum PageCommand {
    case back, forward, reload, reloadBypassingCache, stop
    case cut, copy, paste, pasteAndMatchStyle, undo, redo, selectAll, delete
    case find, findNext, findPrevious
    case zoomIn, zoomOut, zoomReset
    case print, savePageAs, viewSource
    case toggleMute, toggleDevTools, toggleFullscreen
}
```

Built against the stub (T-SWF-06, 2026-11-20; the engine side is written and waits for an engine build). `canPerform` is false on a closed page, `perform` throws `contents.stale` there and `contents.commandDisabled` for a command that cannot be performed now.

The standard AppKit edit actions (`cut:`, `copy:`, `paste:`, `selectAll:`, `undo:`, `redo:`) are expected to work through the responder chain when the page is first responder, so an ordinary Edit menu needs no BDK call. Unverified outside Chromium's own window.

## 10. UI request handlers

Plan, M3, tasks T-UIR-01 to T-UIR-06 and T-SWF-04. The surfaces and their payloads are in [ui-requests.md](ui-requests.md). The registration API is the same for all of them.

```swift
public final class BDKUIHandlers {
    public var permissions: Surface<PermissionRequest>
    public var javascriptDialogs: Surface<JavaScriptDialogRequest>
    public var contextMenus: Surface<ContextMenuRequest>
    public var newWindows: Surface<NewWindowRequest>
    // … one property per surface
}

public struct Surface<Request> {
    public mutating func setHandler(_ handler: (@MainActor (Request) -> BDKUIResponse)?)
    public var mode: SurfaceMode { get }          // from bdk.toml: .default, .custom, .disabled
}

public enum BDKUIResponse {
    case handled        // the application owns the request and will complete it through the request object
    case useDefault     // BDK's default presenter shows it
    case reject         // deny or cancel, where that is safe
}
```

```swift
runtime.ui.permissions.setHandler { request in
    guard request.kinds == [.notifications] else { return .useDefault }
    PermissionSheet.show(request)       // later calls request.grant(confirmedBy:) or request.deny()
    return .handled
}
```

- For a surface in `custom` mode, set the handler on `config.ui` before start.
- For a surface in `default` mode, a handler may be set or cleared at any time on `runtime.ui`.
- A handler returns synchronously. Presentation is asynchronous, through the request object.
- A request the application took and then dropped without completing is cancelled when the object is released. It is never granted.

Settled in T-SWF-04 (2026-11-19), against the stub: `BDKUIRequest` is a base class with a subclass per surface, not a protocol; `Surface.setHandler` throws, because the ABI refuses `bdk_ui_set_handler` after the freeze (`config.registrationAfterStart`). A handler set on `config.ui` is installed at start. After the freeze, `runtime.ui` can replace or clear the handler of a surface that had one at start, but cannot add one to another surface; the "default mode, any time" rule above needs the core to allow handler registration after the freeze. `window` is an `NSWindow?` until `BDKWindow` exists. The completions and payload encoding are in [ui-requests.md](ui-requests.md) section 8 and `packages/swift/README.md`.

## 11. Services and providers

### 11.1 Services

Plan, M3, tasks T-SVC-01 to T-SVC-06. Every command in [services.md](services.md) section 5 has a typed Swift wrapper generated from the same interface definition. The command `history.search` is `profile.history.search(…)`. Arguments and results are Swift structs; events are an `AsyncSequence` on the service object.

```swift
// History
let page1 = try await profile.history.search("swift", in: lastWeek, limit: 50)
try await profile.history.delete(urls: [url])
for await change in profile.history.events { … }       // .visited, .removed

// Bookmarks
let bar = try await profile.bookmarks.root(.bar)
let folder = try await profile.bookmarks.createFolder("Reading", in: bar.id)
let node = try await profile.bookmarks.create(url: url, title: "…", in: folder.id, at: 0)
try await profile.bookmarks.move(node.id, to: bar.id, at: 2)

// Downloads
for await event in profile.downloads.events { … }      // .created, .updated, .completed, .interrupted, .removed
try await profile.downloads.pause(id)

// Settings: the same value a settings page reads through @bdk/internal/settings
let block: CookiePolicy = try await profile.settings.get(.privacyThirdPartyCookies)
try await profile.settings.set(.privacyThirdPartyCookies, .block)

// Site permissions
try await profile.permissions.set(.camera, for: origin, to: .block)

// Clear browsing data
try await profile.privacy.clear([.history, .cookies, .cache], since: .lastHour)

// Passwords: listing gives metadata; revealing needs the operating system's authentication
let entries = try await profile.passwords.list()
let secret = try await profile.passwords.reveal(entries[0].id)   // prompts with Touch ID or the login password

// Search and address bar (Added, S1) — not built: T-SVC-10, M4
let kind = try await profile.search.classify("swift.org")          // .url(URL), .search(URL, engine)
for await batch in profile.search.suggestions(for: "swi", in: page) { … }

// Default browser
let isDefault = await runtime.system.defaultBrowser.status()
try await runtime.system.defaultBrowser.request()                  // macOS asks the user
```

Settled in T-SVC-02 to T-SVC-06 (2026-11-19), against the stub: the wrappers are generated (services.md 9) and follow the interface definitions mechanically, with a few names from this section kept by a table in the generator.

- A namespace is a `struct` (`BDKHistory`, `BDKBookmarks`, ...). Profile-scoped namespaces hang on `BDKProfile` (`profile.history`), application-scoped ones on `BDKRuntime`. `settings` has both: `profile.settings` and `runtime.settings` (the application's own store). `runtime.info` is `runtime.about.info()`, because `BDKRuntime.info` is the ABI version. `system.defaultBrowser.status` is `runtime.system.defaultBrowser.status()`; `autofill.cards.list` is `profile.autofill.cards.list()`.
- `profiles.*` has no generated wrapper: `runtime.profiles` and `BDKProfile` use the ABI's own profile functions.
- Named types carry the `BDK` prefix (`BDKHistoryEntry`, `BDKTimeRange`, `BDKPermissionValue`). `timestamp` is `Date` (milliseconds on the wire), `url` is `URL`, `json` is `BDKJSValue`, `bytes` is `Data`. Enums are `String` enums without a case for a value a newer runtime might add; one that does not decode fails the call with `runtime.malformedResult`.
- A result with one field returns that field (`history.visits` returns `[BDKHistoryVisit]`, `passwords.reveal` returns `String`); one with several returns a nested struct (`BDKHistory.SearchResult`, with `items` and `nextCursor`). A paginated command takes `limit` and `cursor` and the caller loops.
- Names that differ from the examples above: `history.search(_ text, in: range, limit:, cursor:)`, `history.delete(urls:)`, `bookmarks.roots()` (there is no `root(.bar)`), `bookmarks.create(url:, title:, in:, at:)`, `bookmarks.createFolder(_ title, in:, at:)`, `bookmarks.move(_ id, to:, at:)`, `settings.get(_ key) -> BDKJSValue` with `get(_ key, as: Value.self)` and `set(_ key, _ value)` for any `Codable` value (keys are strings; typed `BDKSettingKey` waits for T-SVC-05's setting registration), `permissions.set(origin:, kind:, value:)` (the kind is a string, not an enum), `privacy.clear(types:, range:, origins:, onProgress:)`, `passwords.reveal(_ id)`.
- Marks. A command marked P takes `onProgress`. A command marked G (gesture) needs nothing from Swift: the gesture is a page's, and the host never needs one, so no `NSEvent` parameter exists. A command marked A (authentication) shows the operating system's prompt whoever calls; a refusal throws `capability.reauthenticationFailed`. Cancelling the Swift task cancels the call (`broker.cancelled`).
- Events: `profile.history.events` is an `AsyncStream<BDKHistory.Event>`. Reading the property subscribes at once (unlike `BDKEventSequence`, which subscribes when iterated), so events after that point are delivered; the stream ends when it is dropped or the runtime shuts down.

The generic entry point exists for commands a wrapper does not cover, including `app.` commands:

```swift
let result: Output = try await runtime.commands.invoke("app.tabs.open", Input(url: url), profile: profile)
```

Swift calls are not capability-checked. The application is in the browser process and has full authority (`architecture.md` 9.1).

### 11.2 Providers

Plan, M6, tasks T-PRV-01 and T-PRV-09. A provider is chosen in `bdk.toml`. Swift supplies keys, and the implementation when `provider = "app"`.

```swift
config.services.google.apiKey = Secrets.googleAPIKey            // only if a service uses provider = "google"
config.services.translation.setKey(Secrets.deepLKey)            // for a named provider that needs one
config.services.translation.register(MyTranslationProvider())   // when provider = "app"
config.services.reputation.register(MyReputationProvider())
config.services.geolocation.register(MyGeolocationProvider())
config.services.credentials.register(MyCredentialProvider())    // when provider = "app" (Added, S14)
config.services.leakCheck.register(MyLeakCheckProvider())       // when provider = "app" (Added, S16)
```

```swift
public protocol BDKTranslationProvider: Sendable {
    var descriptor: ProviderDescriptor { get }      // id, display name, privacy: .onDevice or .remote(host:, policyURL:)
    func supportedPairs() async throws -> [LanguagePair]
    func prepare(_ pair: LanguagePair, progress: @Sendable (Double) -> Void) async throws   // model download, if any
    func translate(_ texts: [String], pair: LanguagePair, context: TranslationContext) async throws -> [String]
    func detectLanguage(of sample: String) async throws -> LanguageGuess?   // nil: use Chromium's detector
}

public protocol BDKReputationProvider: Sendable {
    var descriptor: ProviderDescriptor { get }      // includes the protection level shown to users
    func check(_ url: URL, context: ReputationContext) async -> Verdict
    func check(_ download: DownloadCandidate) async -> Verdict
    func update() async throws                       // list-based providers
}

public enum Verdict: Sendable { case safe, unknown, malware, phishing, unwantedSoftware, dangerousDownload }

public protocol BDKGeolocationProvider: Sendable {
    var descriptor: ProviderDescriptor { get }
    func positions(_ options: PositionOptions) -> AsyncThrowingStream<Position, Error>
}
```

Credentials (Added, S14; unverified):

```swift
public protocol BDKCredentialProvider: Sendable {
    var descriptor: ProviderDescriptor { get }
    func credentials(for origin: URL) async throws -> [Credential]   // username and password; asked when a page with a login form loads
    func all() async throws -> [CredentialSummary]                   // for a management page; no passwords
    func save(_ credential: Credential) async throws                 // after the user confirmed a save or update request
    func remove(_ id: Credential.ID) async throws
    var changes: AsyncStream<Void> { get }                            // the store changed or was unlocked; Chromium re-queries
}
```

With `provider = "app"`, Chromium's password manager keeps doing form detection, the save and suggestion requests and the fill, and reads and writes through this protocol instead of its own login database. A locked store returns nothing and signals `changes` when it unlocks. The provider hands passwords to the browser process when a form is found, not when the user picks a suggestion, because that is when Chromium asks. `passwords.*` commands operate on the provider's data.

Leak check (Added, S16; unverified):

```swift
public protocol BDKLeakCheckProvider: Sendable {
    var descriptor: ProviderDescriptor { get }      // what leaves the device, and to which host
    func check(_ candidate: LeakCheckCandidate) async throws -> LeakVerdict
}

public struct LeakCheckCandidate: Sendable {
    public let origin: URL
    public let username: String
    public let password: String                     // in clear; the provider decides what, if anything, is sent
    public let reason: Reason                       // .signIn, .savedLoginsCheck
}

public enum LeakVerdict: Sendable { case notFound, leaked }
```

With `[services.leak_check] provider = "app"`, BDK calls `check` once after a sign-in and once per saved login during `passwords.check` ([services.md](services.md) 5.9). The provider gets the password in clear because a breach service needs something derived from it, and only the provider knows what. Sending a hash prefix and comparing the returned range on the device is the expected form; sending the password or its full hash is not, and BDK cannot prevent it, so the descriptor must say what is sent. A thrown error means "could not check" and flags nothing. With no provider configured there is no check at all; BDK ships none and never calls Google's.

| Rule | Detail |
|---|---|
| Same contract for every provider | Default UI, Swift and `@bdk/internal/translation` behave the same whichever provider is chosen. |
| Verdicts drive Chromium's interstitials | A provider reports. It does not present. The interstitial cannot be suppressed by a provider or by a surface mode. |
| Cancellation | Navigating away or reverting a translation cancels the provider's task. |
| Privacy descriptor | Shown in default UI wherever page content or a URL leaves the device. A `.remote` provider with no policy URL is a start error. |
| Protection level | The default reputation provider's descriptor says "reduced protection", and default UI repeats it (`architecture.md` 11.1). |
| Missing registration | `provider = "app"` with no registered provider is a start error. |

Built in M6 (2026-11-20, against the stub; engine written and not yet built): `config.services.google.apiKey`, `config.services.translation.register(_:)`, `.reputation.register(_:)` and `.geolocation.register(_:)`, with the protocols above. The optional operations are refining protocols, so BDK asks a provider only for what it implements: `BDKTranslationModelManaging` (`prepare`, `removeModel`), `BDKLanguageDetecting` (`detectLanguage`), `BDKDownloadChecking` and `BDKReputationUpdating`. Types carry the `BDK` prefix (`BDKVerdict`, `BDKPosition`, `BDKLanguagePair`, `BDKProviderDescriptor`, which is generated). Not built: `setKey` (no named provider in this runtime takes a key, T-PRV-08); `credentials` (S14, a proposal) and `leakCheck` (S16, T-PRV-13).

## 12. Application commands, events and settings

Plan, M3, task T-SVC-05. Swift is the source of truth. Declarations are extracted at build time to generate TypeScript ([javascript-api.md](javascript-api.md) section 7).

### 12.1 Commands

```swift
struct OpenTab: BDKCommand {
    static let name = "app.tabs.open"
    struct Input: Codable, Sendable { var url: URL; var inBackground: Bool? }
    struct Output: Codable, Sendable { var id: String }
}

config.commands.register(OpenTab.self) { input, context in
    let tab = try await TabStore.shared.open(input.url, profile: context.profile, background: input.inBackground ?? false)
    return .init(id: tab.id)
}

public struct CommandContext: Sendable {
    public let caller: Caller          // .application, .page(route:, contents:), .automation(session:)
    public let profile: BDKProfile?
    public func report(progress: Double, message: String?)
    public var isCancelled: Bool { get }
}
```

| Rule | Detail |
|---|---|
| Name | Must start with `app.`. Anything else is a start error. |
| Capability | A caller needs the capability equal to the command name, or a matching `app.….*` wildcard. The broker checks it before the handler runs. |
| Registration | Before start only. |
| Typing | Input and output are `Codable`. No raw Swift execution, no arbitrary selectors, no pointers. |
| Caller | The handler is told who called. A handler that changes what the user sees should treat a page caller and an automation caller differently if the product requires it. |
| Confirmation | A command can be declared `requiresUserGesture`. The broker then refuses a call from a page that did not come from a user gesture in that page. |

### 12.2 Events

```swift
struct TabsChanged: BDKEvent {
    static let name = "app.tabs.changed"
    var count: Int
}
config.commands.declare(TabsChanged.self)
runtime.commands.emit(TabsChanged(count: 12), to: .profile(profile))   // .all, .profile, .page(contents), .session(session)
```

A page or session receives an application event only if it holds the capability with the event's name.

Settled in T-SVC-05 (2026-11-19): a `BDKCommand` also states the `input` and `output` shape (`BDKShape`, `BDKType`, `types`), because Swift cannot reflect a `Codable` type's fields; `requiresUserGesture`, `profile` and `reportsProgress` are static properties too. Registration goes on `config.commands` (a value, so code generation can build one without a runtime) and names are validated at start. `runtime.commands` invokes by type or by name, subscribes with `events(_:)`, and emits with `emit(_:to:)`. `BDKDeclarations.write(to:)` writes the JSON that the generator reads; the format is in `packages/swift/README.md`.

### 12.3 Settings

```swift
config.settings.register(
    BDKSettingKey<Double>("app.sidebar.width"), default: 240, scope: .profile, exposedToPages: true)
```

Application settings are stored with browser settings and read through the same `settings.get` and `settings.set` commands. `exposedToPages: false` keeps a key out of reach of every page, whatever its capabilities.

Settled in T-SVC-08 (2026-11-20):

- `BDKSettingKey<Value>` (`Value: Codable & Sendable`) names a setting; `config.settings` is a value (`BDKSettingRegistry`) like `config.commands`, `scope` defaults to `.profile` and `exposedToPages` to `false`. The name must start with `app.` (`config.nameNotApp`), a name registered twice is `config.duplicateName`, and a default that cannot be encoded is `config.settingDefaultInvalid`; all are start errors, and the core checks the first two again in `bdk_app_setting_register`.
- The type of a key is the JSON kind of its default (`boolean`, `number`, `string`, `array`, `object`; a null default makes it `json`, which takes any value). `settings.set` with another kind is `broker.invalidArguments`.
- `profile.settings.get(key)`, `set(key, value)`, `reset(key)` and `describe(key)` sit beside the string overloads, on `profile.settings` and on `runtime.settings`. A `profile` scoped key needs a profile; an `application` scoped key has one value for every profile.
- The shared core answers `app.` keys, so the stub and the engine behave the same; the engine only keeps the values, in `BDK App Settings.json` in the profile's directory (profile scope) or in the user data directory (application scope). A private profile keeps none. An unregistered `app.` key reads as `null` and is refused for `set`, `reset` and `describe`. `settings.changed` is emitted for a change.
- `exposedToPages: false` refuses every page and every session with `capability.denied`, whatever capabilities it holds, for all five `settings.*` commands (`getMany` too, if any key is hidden). Swift calls are never refused. One `getMany` cannot mix `app.` keys with browser keys.
- `config.declarations` (`BDKDeclarations`) lists commands, events and `settings` (`key`, `type`, `default`, `scope`, `exposedToPages`) for code generation.

## 13. Automation

Plan, M7, tasks T-AUT-01 to T-AUT-04; written in M7; passes on the stub, engine build and test pending (T-AUT-06). As built: `session.run` throws instead of being `async`; `external.open(directory:authenticate:)` takes an async authenticator; pages a session creates arrive as `runtime.automation.events` `.contentsCreated(page, bySession:)` and as `runtime.events` `.webContentsCreated(page, by: .automation(session: name))` (T-AUT-07); `.application` and `.extension` are not emitted yet. The host and its trust model are in `architecture.md` 9.3; the JavaScript side is in [javascript-api.md](javascript-api.md) section 5.

```swift
let session = try runtime.automation.makeSession(.init(
    name: "research-agent",
    capabilities: ["contents.read", "contents.control", "contents.script", "history.read", "app.tabs.open"],
    scope: .init(profiles: [profile], contents: .created)))   // .all, .created (only pages the session opens), .explicit([page])

let task = try await session.run("summarise", arguments: ["url": .string(url.absoluteString)])
for await event in task.events { … }        // .progress, .log, .commandInvoked(name) for auditing
let result = try await task.value           // BDKJSValue
task.cancel()

session.grant(contents: page)               // widen an explicit scope
session.revoke()                            // stops its tasks, closes its channel

for await event in runtime.automation.events { … }   // session created, revoked; every command a session invoked
```

External tools, off unless `[automation.external] enabled = true`:

```swift
let endpoint = try runtime.automation.external.open { hello in
    // hello.token is whatever the client presented. The application decides who it is.
    guard let grant = AgentRegistry.verify(hello.token) else { return .refuse }
    return .accept(session: try runtime.automation.makeSession(grant.sessionOptions))
}
endpoint.socketPath     // a user-restricted local socket; hand it to the application's own CLI or gateway
endpoint.close()
```

| BDK provides | The application provides |
|---|---|
| Sessions with a fixed capability set and scope (scope is Added, S10) | Who an agent is, what it may be granted, and consent UI |
| The broker check on every call | Policy |
| Revocation | When to revoke |
| An audit event for every command a session invokes | Storage and display of the audit trail |
| A local, authenticated channel | The CLI, the MCP or ACP gateway, and token issuance |

A page created by a session starts in the parking context and is not on screen. The application receives `webContentsCreated(_, by: .automation(session))` and decides whether to show it.

## 14. DevTools and the protocol

Plan, M7, task T-AUT-05; written in M7; passes on the stub, engine build and test pending (T-AUT-06). As built, `present()` and `close()` are `async throws`, and a detached protocol session throws `devtools.detached`.

```swift
page.devTools.present()                         // goes through the DevTools surface
page.devTools.close()
page.devTools.isOpen

let cdp = try await page.devTools.attachProtocol()              // refused if [devtools] protocol = false
let doc = try await cdp.send("DOM.getDocument", ["depth": 1])
for await event in cdp.events { … }
cdp.detach()
```

| Mode in `bdk.toml` | `present()` |
|---|---|
| `frontend = "default"` | Opens Chromium's front end |
| `frontend = "default"` with `overlay` | The same, with the application's CSS and JavaScript loaded into it |
| `frontend = "<path>"` | Opens the application's packaged front end |
| `frontend = "disabled"` | Throws `devtools.disabled` |

The front end arrives as a DevTools request carrying a `BDKWebContents` for the front end. The application can put that view in its own window, or dock it beside the inspected page in its own layout. With no handler, the default presenter opens an undocked window. That a DevTools front end can be hosted as an ordinary page view is unverified.

The protocol is in-process only. Nothing listens on a network port, and no configuration can change that.

## 15. Diagnostics

Added (S4), except logging.

```swift
config.diagnostics.onCrashReport { report in
    // crash_reports = "app". report.fileURL is a minidump; report.process is .browser, .renderer, .gpu, .utility.
    await CrashUploader.shared.enqueue(report)      // the application's own service, with the user's consent
}

runtime.diagnostics.crashReports()                   // completed reports on disk
runtime.diagnostics.logURL                           // the current log file
try await runtime.diagnostics.startNetLog(to: url)   // for support cases; the user must be told it captures traffic metadata
try await runtime.diagnostics.stopNetLog()
```

BDK uploads nothing. A report leaves the machine only through code the application wrote.

## 16. Errors

```swift
public struct BDKError: Error, Sendable {
    public let domain: Domain
    public let code: String          // stable, lower camel case
    public let message: String       // for developers; not localised
    public let details: [String: BDKJSValue]
}
```

| Domain | Typical codes |
|---|---|
| `abi` | `versionMismatch`, `hashMismatch` |
| `runtime` | `alreadyStarted`, `notStarted`, `runtimeMissing`, `runtimeTampered`, `engineTooOld`, `profileTooNew`, `shutdownInProgress` |
| `config` | `resolvedMissing`, `resolvedInvalid`, `registrationAfterStart`, `handlerMissing`, `providerMissing`, `keyMissing`, `nameNotApp`, `routeUnknown` |
| `profile` | `notFound`, `inUse`, `nameTaken`, `isPrivate` |
| `contents` | `stale`, `crashed`, `stateTooNew`, `internalPage` |
| `navigation` | `aborted`, `superseded`, `blocked`, `handledNatively`, `netError` |
| `broker` | `unknownCommand`, `invalidArguments`, `cancelled`, `timeout`, `payloadTooLarge`, `notImplemented` |
| `capability` | `denied`, `gestureRequired`, `reauthenticationFailed`, `outOfScope` |
| `extension` | `incompatible`, `sourceNotAllowed`, `notFound`, `surfaceDisabled`, `installCancelled` |
| `provider` | `unavailable`, `unsupportedPair`, `modelDownloadFailed`, `remoteError` |
| `automation` | `disabled`, `hostGone`, `sessionRevoked`, `scriptNotFound`, `scriptFailed` |
| `ui` | `requestCompleted`, `gestureRequired`, `surfaceDisabled`, `surfaceMode`, `surfaceUnknown`, `completionNotAllowed`, `notOverridable`, `widensRequest` |
| `devtools` | `disabled`, `protocolDisabled`, `alreadyAttached` |
| `platform` | `keychainUnavailable`, `notDefaultBrowserCandidate` |

Start errors are `abi`, `runtime` and `config`. Each message names what to fix. An application should show a start error instead of a blank window, and `bdk doctor` reports most of them before launch.

## 17. Test support

```swift
import BDKTestSupport

let runtime = try BDKRuntime.startStub(.init(routes: […], capabilities: […]))
let profile = try await runtime.profiles.open("Test")
let page = try profile.webContents.create()
runtime.stub.simulateNavigation(page, to: url, title: "Example")
runtime.stub.raise(PermissionRequest.fixture(.camera, origin: "https://example.com"), on: page)
```

The stub implements the C ABI in memory. It lets an application unit-test its tab model, its UI-request handlers, its command handlers and its session code on any machine, with no Chromium. It passes the same conformance suite as the real runtime, so it cannot drift from it (`architecture.md` section 5). It renders nothing and runs no JavaScript.

## 18. A complete small browser

```swift
import AppKit
import BDK

@main
final class AppDelegate: NSObject, NSApplicationDelegate {
    private var window: NSWindow!
    private var page: BDKWebContents!

    static func main() {
        let app = BDKApplication.shared          // NSPrincipalClass = BDKApplication
        let delegate = AppDelegate()
        app.delegate = delegate
        app.run()
    }

    func applicationWillFinishLaunching(_ notification: Notification) {
        do {
            var config = try BDKConfiguration.load()
            config.pages.register("downloads", .native { request in
                DownloadsPanel.shared.show()
                return .handled
            })
            try BDKRuntime.start(config)
        } catch {
            StartFailure.present(error)          // never continue without a runtime
        }
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        Task { @MainActor in
            let runtime = BDKRuntime.shared
            let profile = try await runtime.profiles.open("Personal")
            page = try profile.webContents.create()

            window = NSWindow(contentRect: .init(x: 0, y: 0, width: 1200, height: 800),
                              styleMask: [.titled, .closable, .resizable, .miniaturizable],
                              backing: .buffered, defer: false)
            window.contentView = page.view       // attaching creates the host context
            window.makeKeyAndOrderFront(nil)

            Task {
                for await event in page.events {
                    switch event {
                    case .titleChanged: window.title = page.title
                    case .processGone: page.navigation.reload()
                    case .closeRequested: _ = await page.close()
                    default: break
                    }
                }
            }
            try await page.navigate(to: URL(string: "https://example.com")!)
        }
    }

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        BDKRuntime.shared.terminate(replyingTo: sender)
    }
}
```

With this and the smallest `bdk.toml`, every browser-owned surface uses its default presenter: permission prompts, dialogs, downloads, printing, passwords, the extensions menu and the built-in internal pages. That is the M3 exit criterion in [../roadmap.md](../roadmap.md).
