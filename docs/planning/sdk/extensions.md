# BDK extensions API

Status: proposal, 2026-11-15. Based on [../architecture.md](../architecture.md) section 10, the spec's section 8A and [../feasibility.md](../feasibility.md) section 3. Names are placeholders. Nothing here has run: the MV3 probe has not been loaded into any browser. Status labels are in [README.md](README.md) section 2. Tasks: T-EXT-01 to T-EXT-09. Extension support is a public v1 gate.

**Built in M5 (2026-11-20), engine pending.** The commands, surfaces, Swift API and window registration below exist and pass against the stub; the engine side is written and not yet built or run ([roadmap.md](../roadmap.md) M5, [amendments.md](../amendments.md) 2.17). Departures from this proposal are in amendments 2.17, `packages/swift/README.md` and `packages/core/docs/conventions.md` ("Extensions (M5)"). Not built: `extensions.inspectView` in the engine (T-EXT-11, M5); a default presenter for new pages that extension UI opens (T-EXT-12, M5; written 2026-11-23, not yet run on the engine); the store's update check being suppressed when the store is off (T-EXT-13, M6); the engine side of `developer_mode`, `blocklist` and bundled extensions, section 9 (T-EXT-14, M9; configuration, core and stub built 2026-11-25). The matrix is [docs/extensions/compatibility-matrix.md](../../extensions/compatibility-matrix.md), not run until the engine is (T-EXT-10).

## 1. What BDK adds and what it leaves alone

| Part | Owner | BDK work |
|---|---|---|
| Manifest parsing, service workers, content scripts, messaging, storage, permissions, the `tabs` and `windows` APIs | Chromium's extension system, unchanged | None. Every page is a real tab in a real `Browser`. |
| Where an action button, a popup, a side panel or a menu item appears | The application, or BDK's default presenter | Requests, secure views, and a default for each |
| Which sources may install | `bdk.toml` | Policy, replacing Chromium's default-deny for off-store packages |
| Whether an extension can work in this product | BDK | Manifest inspection before install and at update |
| Management UI | An internal page like any other | The `extensions` route and the `extensions.*` commands |

Extension contexts never receive `@bdk/internal/*` or any broker interface (invariant 5). An extension's popup or side panel is a sandboxed, browser-managed view, even when the application places it.

## 2. Swift API

```swift
let extensions = profile.extensions

// Management
let all = try await extensions.list()
let info = try await extensions.install(.unpacked(directoryURL))      // or .crx(fileURL), .updateURL(id:, url:), .store(name:, id:)
try await extensions.setEnabled(false, for: info.id)
try await extensions.uninstall(info.id)
for await event in extensions.events { … }                            // installed, updated, uninstalled, enabled, disabled, loaded, unloaded

// Checking before installing
let report = try await extensions.inspect(.crx(fileURL))              // CompatibilityReport

// Actions
let actions = extensions.actions(for: page)                           // [ActionState] for the page's window, in user order
extensions.activateAction(info.id, on: page, confirmedBy: event)      // what a toolbar click does
for await change in extensions.actionChanges { … }

// Keyboard shortcuts declared by extensions
let commands = try await extensions.commands()                        // [ExtensionCommand]: id, name, description, suggested key
try extensions.performCommand(command, on: page, confirmedBy: event)
```

Value types:

```swift
public struct ExtensionInfo: Identifiable, Sendable {
    public let id: String                    // the 32-character extension identifier
    public let name, version, description: String
    public let icon: NSImage?
    public let manifestVersion: Int
    public let source: Source                // .unpacked, .crx, .updateURL, .store(name), .bundled
    public let isEnabled: Bool
    public let disableReasons: [DisableReason]   // .user, .incompatible, .blocklisted, .permissionsIncreased, .corrupted
    public let permissions: PermissionSummary    // API permissions and host patterns, with Chromium's warning strings
    public let siteAccess: SiteAccess            // .onClick, .onSpecificSites([pattern]), .onAllSites
    public let allowedInPrivate: Bool
    public let allowedFileAccess: Bool
    public let surfaces: Set<Surface>            // .action, .popup, .sidePanel, .contextMenus, .optionsPage, .devTools, .omnibox
    public let optionsPage: URL?
    public let errors: [ExtensionError]          // manifest and runtime errors, for a developer view
    public let compatibility: CompatibilityReport
}

public struct ActionState: Identifiable, Sendable {
    public let id: String                    // extension identifier
    public let title: String
    public let icon: NSImage
    public let badgeText: String?
    public let badgeColor: NSColor?
    public let isEnabled: Bool               // for this page
    public let hasPopup: Bool                // for this page
    public let wantsAccess: Bool             // the extension is waiting for the user to grant access to this site
    public let isPinned: Bool                // the user's choice; the application decides what pinning looks like
}
```

## 3. Commands

For internal pages and automation sessions, and behind the Swift calls above. Call semantics are in [services.md](services.md) section 2.

| Command | Capability | Marks | Does |
|---|---|---|---|
| `extensions.list` | `extensions.list` | | Every installed extension with the fields of `ExtensionInfo` except errors |
| `extensions.get` | `extensions.list` | | One, with errors |
| `extensions.inspect` | `extensions.list` | | A compatibility report for a package or an installed extension |
| `extensions.install` | `extensions.manage` | G U | From a source the configuration allows. Always raises Chromium's install prompt. |
| `extensions.uninstall` | `extensions.manage` | G U | Raises a confirmation |
| `extensions.setEnabled` | `extensions.manage` | | Enabling after a permissions increase raises the permission prompt |
| `extensions.setSiteAccess` | `extensions.manage` | | On click, on listed sites, or on all sites |
| `extensions.setAllowedInPrivate` | `extensions.manage` | | Refused when `allow_in_private = "never"` |
| `extensions.setAllowedFileAccess` | `extensions.manage` | | |
| `extensions.setPinned` | `extensions.manage` | | Stores the user's pin state. Presentation is the application's. |
| `extensions.reload` | `extensions.manage` | | For unpacked extensions |
| `extensions.checkForUpdates` | `extensions.manage` | P | For sources that have an update URL |
| `extensions.openOptions` | `extensions.list` | U | Raises an options-page request |
| `extensions.inspectView` | `extensions.manage` | U | Open DevTools on the service worker or a view |
| `extensions.clearErrors` | `extensions.manage` | | |
| `extensions.commands`, `extensions.setCommandShortcut` | list, manage | | Declared keyboard commands and the user's bindings |

Events: `extensions.installed`, `extensions.updated`, `extensions.uninstalled`, `extensions.enabledChanged`, `extensions.loaded`, `extensions.unloaded`, `extensions.errorAdded`.

Marks are defined in `services.md` section 2: G gesture, U raises a UI request, P progress.

## 4. UI surfaces

Each follows the pattern in [ui-requests.md](ui-requests.md) sections 1 to 3. Modes are set under `[extensions.ui]`.

| Key | `default` | `custom` | `disabled` |
|---|---|---|---|
| `actions` | Actions are listed in BDK's extensions menu (4.7) | The application renders buttons from `ActionState` and calls `activateAction` | No action UI. Extensions that require an action are refused at install. |
| `popups` | A transient window anchored to the action's entry | The application receives a secure view and places it | `action.openPopup` and popup-on-click are unavailable. Extensions whose action has only a popup are refused. |
| `side_panels` | A panel attached to the trailing edge of the page's window, resizable by the user | The application embeds the secure view anywhere, at any size, inside its own container (4.3) | `chrome.sidePanel` is absent. Extensions that require it are refused. |
| `context_menus` | Extension items appear in the default page context menu | Items arrive inside `ContextMenuRequest.items` | `chrome.contextMenus` is absent |
| `options_pages` | Opens the options page as a new page through a new-window request | The same request, for the application to place | Options pages cannot be opened |
| `devtools` | Extension panels load in the default DevTools front end | The same, in the application's DevTools host | DevTools extension pages are not loaded; reported as a gap |

"Refused" means with a reason naming the surface, at install and again at an update that adds the requirement (section 6).

### 4.1 Actions

There is no request for showing a button. Action state is data: `extensions.actions(for:)` and its change stream. An application in `custom` mode draws what it likes.

What a click does is decided by Chromium, not by the application:

```swift
let outcome = extensions.activateAction(id, on: page, confirmedBy: event)
// .dispatchedClick   action.onClicked fired; activeTab granted for this page
// .popupRequested    a PopupRequest follows
// .sidePanelRequested
// .accessRequested   the extension needs site access; a permission request follows
// .blocked(reason)
```

Rules:

- Activation targets one page, the one the user is looking at. `activeTab` is granted for that page and revoked when it navigates to another origin; as in Chrome, it is kept across navigations within the origin.
- Activation needs a user gesture. An automation session cannot activate an action.
- With several pages visible, the application passes the focused page. That is the same page `tabs.query({active: true, currentWindow: true})` returns.

### 4.2 Popups

```swift
public final class ExtensionPopupRequest: BDKUIRequest {
    public let extensionID: String
    public let secureView: NSView                // browser-managed; the application positions and sizes its container
    public var preferredSize: CGSize { get }     // changes as the popup lays out; observe sizeChanges
    public var sizeChanges: SizeChanges { get }
    public let trigger: Trigger                  // .userClick, .openPopupAPI, .command
    public func accept()                         // the application has put secureView on screen
    public func reject()
    public func close()                          // the application closes it
    public var closed: Closed { get }            // fires when the popup closed itself (window.close(), blur, navigation)
}
```

| Behaviour | Rule |
|---|---|
| Focus | The popup must be able to become key and take keyboard input. A container that cannot become key breaks most popups. |
| Closing | BDK closes the popup when it loses focus, when its page calls `window.close()`, and when the page it was opened for navigates or is hidden. A custom host must dismiss its container when `closed` fires. |
| Staying open | A popup being inspected with DevTools stays open on blur, as in Chrome. |
| Size | The popup sizes itself, within Chromium's minimum and maximum. The container follows `preferredSize`. |
| What the application may not do | Read or script the popup, draw over its content, or keep it alive after `closed`. The view is opaque to the application. |

### 4.3 Side panels

```swift
public final class ExtensionSidePanelRequest: BDKUIRequest {
    public let extensionID: String
    public let scope: Scope                      // .window or .page(BDKWebContents): the extension chose per-tab or global
    public let secureView: NSView
    public let title: String
    public let icon: NSImage?
    public var visibilityChanges: VisibilityChanges { get }   // when a page-scoped panel should show and hide
    public func accept(side: Side = .trailing)   // .leading, .trailing: what the extension reads from sidePanel.getLayout()
    public var side: Side { get set }            // update it if the application moves the panel
    public func reject()
    public func close()
    public var closed: Closed { get }
}
```

What the application controls, by mode:

| | `default` | `custom` |
|---|---|---|
| Position | The trailing edge of the page's window. Fixed. | Anywhere the application puts `secureView`: either edge, inside its own sidebar, a floating panel, another window |
| Size | Chromium's default width; the user drags the divider and BDK remembers the width | The application's. The content reflows to the size of the view. The application should keep the width at or above Chromium's side-panel minimum, because extensions lay out for it. |
| Frame around the content | BDK's: a header with the extension's title and icon, and a close button, in Chromium's appearance | The application's own container: header, close control, background, border, corner radius, shadow, show and hide animation. `title` and `icon` are supplied for the header. |
| The content itself | The extension's page | The same. The application cannot restyle, script or draw over it, in either mode. It follows the profile's `appearance.colorScheme`. |

The default presenter has no position or styling options on purpose: a product that cares where the panel sits is expected to use `custom`, which needs only a container view and a call to `accept`. If a setting for the default presenter turns out to be wanted, the place for it is the window registration ([swift-api.md](swift-api.md) 8.1), not `bdk.toml`. Clipping the secure view to rounded corners from a parent layer is unverified.

| Behaviour | Rule |
|---|---|
| Reported side | `chrome.sidePanel.getLayout()` tells an extension whether its panel is on the left or the right. A custom host passes the side to `accept` so the answer is true. A panel that is on neither side reports the nearer one. |
| Scope | A panel scoped to a page should be shown only while that page is the focused one. BDK tells the application when to show and hide through `visibilityChanges`; a custom host follows it. |
| Several panels | One extension panel is open per window at a time, as in Chrome. Opening another closes the first. |
| Split layouts | With two pages visible, a page-scoped panel belongs to the focused page. Unverified; recorded for the compatibility matrix (risk R9). |
| Dependency | Chromium creates the side panel only when a `BrowserView` exists. This needs a patch (task T-EXT-03). Until M0.5 runs, whether `sidePanel.open` works is unknown. |

### 4.4 Context-menu items

Extension items are part of `ContextMenuRequest.items`, with `source == .extension(id)` ([ui-requests.md](ui-requests.md) 5.5). They appear under one submenu per extension when an extension contributes more than one, as in Chrome. Performing one grants `activeTab` and fires `contextMenus.onClicked` with the right page.

### 4.5 Options pages

An options page opens as a new-window request of kind `.newPage` whose `initiator` is the extension. Extensions that set `open_in_tab: false` expect an embedded dialog inside the management page; with a custom management page, they open as a page.

### 4.6 Install, permission and uninstall prompts

```swift
public final class ExtensionInstallRequest: BDKUIRequest {
    public let kind: Kind                        // .install, .permissionsIncrease, .optionalPermissions, .reEnable
    public let extensionName: String, icon: NSImage?
    public let source: ExtensionInfo.Source
    public let warnings: [String]                // Chromium's permission warnings, in its wording and order
    public let hostWarnings: [String]
    public let compatibility: CompatibilityReport
    public func accept(confirmedBy event: NSEvent) throws
    public func cancel()
}

public final class ExtensionUninstallRequest: BDKUIRequest {
    public let extensionName: String
    public let requestedBy: Initiator            // the user, or another extension through chrome.management
    public func confirm(confirmedBy event: NSEvent) throws
    public func cancel()
}
```

Rules, from the spec's section 8A:

- These surfaces have no `disabled` mode and no way to pre-approve. Every install, every permissions increase and every optional-permission grant is shown and needs a gesture.
- A custom presenter must show every warning string it was given. BDK cannot verify that it did; the spec makes it the application's obligation.
- Bundled extensions (section 9) are the one case with no prompt, because they are part of the signed application.

### 4.7 The default extensions menu

For an application that draws no extension UI at all. One entry point, attached to the page's window at the `extensions` anchor, or in the window's title bar accessory area when no anchor is registered. It lists every action with its icon and badge, activates one on click, hosts the popup, and offers "manage extensions", which navigates to the `extensions` route.

The application can open it from its own menu or shortcut:

```swift
extensions.presentDefaultMenu(in: window)
```

Where exactly it sits with no anchor is open in the spec and is settled in task T-EXT-02.

### 4.8 DevTools extension pages

An extension with a `devtools_page` adds panels to DevTools. Supported in the default, undocked front end first (task T-EXT-08). With a replaced front end (`[devtools] frontend = "<path>"`), DevTools extension panels are not loaded, and the matrix says so.

## 5. Installation sources

| Source | Swift | Config key | Default | Notes |
|---|---|---|---|---|
| Unpacked directory | `.unpacked(URL)` | `[extensions.install] unpacked` | On | Subject to `developer_mode` |
| Local CRX | `.crx(URL)` | `crx` | On | BDK replaces Chromium's off-store default-deny with this setting and keeps the prompt (task T-EXT-05) |
| Self-hosted update URL | `.updateURL(id:url:)` | `update_urls` | Any | Installs and then updates from an update manifest |
| Named store | `.store(name:id:)` | `stores` | None | Decision D4: available, off unless the application lists it. A store name is not a promise of access: store terms and endpoints can change (risk R10). |
| Application bundle | none; declared in `bdk.toml` | `[extensions.bundled]` | None | Accepted (S6, 2026-11-25). Section 9. `install` refuses the kind `bundled` |
| Drag and drop, or a `.crx` link on a web page | none | follows `crx` | | Arrives as a download; the application calls `install(.crx(…))` if it wants to offer it |

A source that is switched off is refused with `extension.sourceNotAllowed`, naming the key. Switching a source off does not disable extensions already installed from it.

Updates: checked on Chromium's schedule for extensions with an update URL, and on demand with `extensions.checkForUpdates`. An update that increases permissions disables the extension until the user approves. An update that becomes incompatible is refused and the installed version keeps running.

## 6. Compatibility checks

Before installation and before applying an update, BDK inspects the manifest and produces a report.

```swift
public struct CompatibilityReport: Sendable {
    public let verdict: Verdict                  // .compatible, .degraded, .incompatible
    public let findings: [Finding]
}

public struct Finding: Sendable {
    public let severity: Severity                // .blocking, .degrading, .informational
    public let code: String                      // "surface.sidePanel.disabled", "api.identity.getAuthToken.unavailable", …
    public let subject: String                   // the manifest key or permission
    public let message: String                   // names the cause and, where there is one, the bdk.toml key
}
```

| Checked | Blocking when |
|---|---|
| Manifest version | Not one the engine loads |
| `minimum_chrome_version` | Newer than the engine |
| Required permissions | One is for an API that is disabled in this product or absent from BDK |
| Required surfaces | The action has only a popup and popups are disabled; a side panel is declared and side panels are disabled |
| Host handlers | A surface is in `custom` mode and the application registered no handler. This is also a start error, so it should never reach an install. |
| Blocklist | The identifier is in `[extensions.install] blocklist` |

What inspection cannot prove, and the documentation must say so: optional permissions requested later, APIs chosen at runtime, and behaviour that depends on Chrome's own UI. Those fail explicitly when they happen:

- A disabled or unsupported API is absent from the extension's `chrome` object, so feature detection works.
- A call that exists and cannot be satisfied rejects with an error that names the reason.
- Every such failure is counted per extension and shown in `ExtensionInfo.errors`, so a developer view can list them.

With `install_policy = "allow_all"`, a blocking finding becomes a warning in the install prompt and the extension installs.

## 7. Extension APIs that need the host

Most extension APIs need nothing from BDK. These touch UI or product structure, and this is how each is planned to be satisfied. Every row is a plan, not a test result; the published matrix comes from the suite (task T-EXT-07).

| API | Needs | Planned behaviour |
|---|---|---|
| `action` | Action surface | State and activation as in 4.1. `openPopup` raises a popup request. |
| `sidePanel` | Side-panel surface | 4.3 |
| `contextMenus` | Context-menu surface | 4.4 |
| `commands` | The application owns shortcuts | BDK exposes the declared commands and the user's bindings. The application binds keys and calls `performCommand`. With no application support, the default extensions menu lists them and nothing is bound. |
| `tabs.create`, `windows.create` | New-window request | Kind `.newPage` or `.extensionWindow`. If the application rejects, the call rejects. |
| `tabs.update({active})`, `tabs.highlight` | Layout | Raises an activation request to the window delegate. The application may ignore it; the call then resolves without a change, and the matrix records that. |
| `tabs.move`, `tabs.group`, `tabGroups` | Tab order and groups | BDK has no order and no groups. `tabs.move` between windows moves the page's view only if the application's delegate does it. `tabGroups` and `tabs.group` work on the host context's tab model and the application shows no groups: degraded (M5; amendments 2.17). |
| `tabs.query` | Host contexts | Pages attached to a window are that window's tabs. Off-screen pages follow decision D1. The parking context is hidden from extensions if the M0 probe shows it must be. |
| `tabs.discard`, `tabs.duplicate`, `tabs.remove` | Application | Delivered as requests to the window delegate, with a default that does the obvious thing to the page |
| `windows.update` | Window delegate | `WindowRequest`, default applied to the `NSWindow` |
| `sessions` | Session restore | BDK has no session history. `sessions.getRecentlyClosed` returns an empty list; `restore` rejects. A gap. |
| `omnibox` | Address bar | Extension keywords and their suggestions arrive through `search.suggestions` (Added, S1). Without S1 this API has no host. |
| `notifications` | Notifications surface | |
| `downloads`, `downloads.setShelfEnabled`, `downloads.setUiOptions` | Downloads surface | The downloads service; the UI calls are accepted and passed to the presenter as hints |
| `identity.launchWebAuthFlow` | New-window request | A popup-kind request |
| `identity.getAuthToken`, `identity.getProfileUserInfo` | Google account | Absent. No sign-in exists. |
| `debugger` | A visible notice | Chrome shows a bar while an extension debugs a page. BDK raises a `page_health`-style notice the application must show; the default presenter is a bar at the top of the page view. |
| `desktopCapture`, `tabCapture` | Screen-capture surface | [ui-requests.md](ui-requests.md) 5.16 |
| `permissions.request` | Install-request surface | Kind `.optionalPermissions` |
| `management` | Uninstall prompt | |
| `bookmarks`, `history`, `topSites`, `cookies`, `browsingData`, `storage`, `scripting`, `webRequest`, `declarativeNetRequest`, `alarms`, `runtime`, `i18n`, `webNavigation`, `offscreen` | Nothing | Chromium's implementation, unchanged |
| `search` | Default engine | Uses the engine from the `search` service; absent without one |
| `tts`, `ttsEngine` | Platform speech | Expected to work through Chromium's macOS implementation; unverified |
| `privacy`, `proxy`, `contentSettings`, `fontSettings` | Settings | Chromium's implementation; changes show as `source: extension` in `settings.describe` |
| `webstore`, `webstorePrivate` | Store | Only with a store source (decision D4) |
| `enterprise.*`, `platformKeys`, `loginState` | Enterprise and ChromeOS | Absent |

## 8. Active tab and window mapping

What an extension observes, given the application's layout. From `architecture.md` 4.1 and 4.3; all of it is to be confirmed by the M0 probe (task T-EXT-01).

| Application state | Extension sees |
|---|---|
| One page visible in window W | W has one active tab |
| Two pages visible in W, B focused | Both are tabs of W; B is active. `tabs.query({active: true, currentWindow: true})` returns B. |
| The user focuses A | `tabs.onActivated` for A |
| A page's view is removed and not placed elsewhere | It remains a tab of its last window, inactive (or of an application host context, decision D1) |
| A page moves to another window | Same tab identifier; `tabs.onDetached` then `tabs.onAttached` |
| A page is created and never attached | A tab in the parking context. Whether `windows.getAll` lists that context is the probe's tenth assertion. |
| The application closes a page | `tabs.onRemoved` |

## 9. Bundled extensions (Accepted, S6, 2026-11-25)

Built in T-EXT-14 (2026-11-25) for the configuration, the core and the stub, with conformance tests (`ext_developer_mode_*`, `ext_blocklisted_*`, `ext_bundled_*`): the schema and resolver (configuration.md 3.7), the core reading `developer_mode`, `blocklist` and `bundled` at start (`runtime/extension_policy.cc`), `developer_mode = "off"` refusing unpacked directories (`extension.sourceNotAllowed`), a blocklisted identifier refused from every source (`extension.blocklisted`, which `allow_all` does not override), and in the stub each bundled extension loaded with no prompt in every persistent profile, refused for uninstall (`extension.notRemovable`) and, with `required = true`, for disabling (`extension.notRemovable`). The build copies each directory into `Contents/Resources/<path>` and prints its declared permissions in `bdk build extensions` (cli.md section 5, `packages/cli/src/core/steps/extensions.ts`), and `bdk doctor --bundle` checks each copy against the digest in the application's resolved file. Not built (T-EXT-14, M9): the engine loading bundled extensions (Chromium's unpacked loader with the component-like "cannot uninstall" management policy, `engine/extensions/management_policy.cc`), honouring the blocklist (Chromium's `ExtensionManagement` blocklist) and `developer_mode` (`extensions::util::SetDeveloperModeForProfile`, which the engine now sets on for every unpacked load); the setting `extensions.developerMode` that `"user"` refers to; an extension already installed whose identifier is blocklisted at a later start being disabled with the reason `blocklisted`; the scenario that a bundled extension loads with no prompt in every persistent profile on the engine.

Declared under `[extensions.bundled]`. The build copies each into the application bundle and records its digest in the resolved file.

| Property | Behaviour |
|---|---|
| Load | At start, in every persistent profile, before the first navigation |
| Prompt | None. The extension is covered by the application's signature. |
| Permissions | Whatever its manifest declares. The build prints them, so a reviewer sees what the product grants itself. |
| Uninstall | Not offered to the user |
| Disable | Offered, unless `required = true` |
| Update | With the application. It has no update URL. |
| Private profiles | Follows `allow_in_private`, like any other |
| BDK privileges | None. A bundled extension is still an extension and gets no broker interface. |

## 10. Private profiles

| Setting | Effect |
|---|---|
| `allow_in_private = "user"` | Each extension is off in private profiles until the user allows it, as in Chrome. The manifest's `incognito` mode (`spanning`, `split`, `not_allowed`) is honoured. |
| `allow_in_private = "never"` | No extension runs in a private profile. `setAllowedInPrivate` is refused. |

Incognito compatibility is listed as open in the spec. The probe suite needs cases for both manifest modes before the matrix claims support.

## 11. What is still open

| Item | Settled by |
|---|---|
| Whether `sidePanel` works at all without `BrowserView` | T-EXT-03 implements `SidePanelUI` with no patch; engine run pending |
| What extensions observe for off-screen pages | Decision D1, T-ADP-08 |
| Whether the parking context must be hidden from `windows.getAll` | Settled from the source in M5: it is listed, so a patch hides it; the probe scenario runs with the engine |
| Placement of the default extensions menu with no anchor | Settled in M5: a trailing title-bar accessory button (amendments 2.17) |
| Chrome Web Store as a source | Decided (D4): a named source per application. T-EXT-09 |
| `commands` with application-owned shortcuts | T-EXT-04; the API above is the proposal |
| `omnibox` | Depends on addition S1 |
| Popup focus and close behaviour inside an application-owned container | T-EXT-02 |
| Whether the default side-panel presenter needs a position option | After M5, from use. Custom mode already covers it. |
