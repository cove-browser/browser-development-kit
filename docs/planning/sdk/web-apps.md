# BDK installable web apps

Status: proposal, 2026-11-25. Decided for M9 by the product owner ([../amendments.md](../amendments.md) 2.24). Based on [../architecture.md](../architecture.md) sections 7, 11.1 and 16 and on the shape of [extensions.md](extensions.md). Names are placeholders. Nothing here is built, and no Chromium source was read for it beyond what T-UIR-10 recorded ([ui-requests.md](ui-requests.md) 8.2): every statement about Chromium's web-app system is to be checked at the pin by T-PWA-01 before anything is built on it. Status labels are in [README.md](README.md) section 2.

Not built: the whole document. Tasks T-PWA-01 to T-PWA-05, M9 ([../tasks.md](../tasks.md)).

## 1. What BDK adds and what it leaves alone

| Part | Owner | BDK work |
|---|---|---|
| Manifest parsing, installability, service workers, offline storage, the per-profile registry of installed apps | Chromium's web-app system, unchanged. `WebAppProvider` already runs in every BDK profile. | None |
| Whether a page may offer itself for installation | `bdk.toml`: `[web_apps] enabled`, off by default | The switch; today the preference `profile.web_app.install_by_user_enabled` is forced off |
| The install prompt | The application, or BDK's default presenter | A request (section 4.1). Chromium's install dialog is views and cannot be shown on a BDK page. |
| The window an installed app opens in | The application, or BDK's default presenter | A new-window request of kind `.webApp` (section 4.2). Chromium's app window is a views frame; BDK does not use it. |
| Which profile an app is installed in | The application | Per-profile API (section 5) |
| Push messages | `bdk.toml`: `[services.push]`, `"none"` by default | A provider choice (section 7) |
| Dock icons, Launchpad entries, file and protocol handlers, run at login | Nobody in v1 | Not built (section 8) |
| Management UI | The application, or an internal page | The `webApps.*` commands |

An installed web app is ordinary web content. It gets no `@bdk/internal/*`, no broker interface and no capability, exactly as a page in a tab (invariant 5).

## 2. Swift API

Not built: T-PWA-03 (stub, core, Swift), T-PWA-04 (engine).

```swift
let apps = profile.webApps                                            // BDKWebApps; one registry per profile

// Management
let all = try await apps.list()                                       // [WebAppInfo]
let check = try await apps.installability(of: page)                   // WebAppInstallability; raises no prompt
let info = try await apps.install(.page(page), confirmedBy: event)    // or .url(startURL): section 5
try await apps.uninstall(info.id, confirmedBy: event)
for await event in apps.events { … }                                  // installed, updated, uninstalled, badgeChanged

// Launch
let page = try await apps.launch(info.id)                             // raises a new-window request of kind .webApp
let page = try await apps.launch(info.id, shortcut: info.shortcuts[0])

// A page's relation to an app
page.webApp                                                           // WebAppPageState?: the app, and whether the page is in its scope
```

Value types:

```swift
public struct WebAppInfo: Identifiable, Sendable {
    public let id: String                    // Chromium's app identifier, derived from the manifest id
    public let profileID: BDKProfile.ID      // the profile it is installed in
    public let name: String                  // from the manifest: chosen by the site, not verified
    public let shortName: String?
    public let startURL: URL
    public let scope: URL
    public let origin: String                // what a presenter must show beside the name
    public let displayMode: DisplayMode      // .standalone or .minimalUI, after fallback (section 6)
    public let themeColor: BDKColor?
    public let backgroundColor: BDKColor?
    public let icons: [WebAppIcon]           // size, purpose, image data
    public let shortcuts: [WebAppShortcut]   // name, URL, icons
    public let badge: WebAppBadge?           // .flag or .count(Int); nil when cleared
    public let installedAt: Date
}

public struct WebAppInstallability: Sendable {
    public let isInstallable: Bool
    public let installedID: String?          // set when this profile already has the app
    public let manifest: WebAppInfo?         // what would be installed
    public let errors: [String]              // Chromium's installability errors, for a developer view
}

public struct WebAppPageState: Sendable {
    public let app: WebAppInfo
    public let isInScope: Bool               // false after a navigation leaves the app's scope
}
```

- `install` and `uninstall` take the `NSEvent` that confirmed them, as every granting completion does ([ui-requests.md](ui-requests.md) section 3).
- `install` called by the application raises no request: the application has already asked its user. A page's own `prompt()` raises one (section 4.1).
- Every call fails with `webApps.disabled` while `[web_apps] enabled` is false, and `list` returns nothing.
- The C ABI carries identifiers, strings and image buffers. No AppKit type crosses it: `NSEvent` stays in the Swift binding, as for the other surfaces.

## 3. Commands

For internal pages and automation sessions, and behind the Swift calls above. Call semantics and marks are in [services.md](services.md) section 2.

| Command | Capability | Marks | Does |
|---|---|---|---|
| `webApps.list` | `webApps.list` | | Every app installed in the caller's profile, with the fields of `WebAppInfo`; icons by reference |
| `webApps.get` | `webApps.list` | | One |
| `webApps.installability` | `webApps.list` | | The report for a page |
| `webApps.install` | `webApps.manage` | G U | Raises the install request, so a page or session can never install silently |
| `webApps.uninstall` | `webApps.manage` | G | Removes the app and, when asked, its site data |
| `webApps.launch` | `webApps.list` | U | Raises a new-window request of kind `.webApp` |

Events: `webApps.installed`, `webApps.updated`, `webApps.uninstalled`, `webApps.badgeChanged`.

Capabilities, both grantable to pages and to sessions: `webApps.list` and `webApps.manage`.

## 4. UI surfaces

### 4.1 Install prompt

Surface key `web_app_installs` in `[ui.surfaces]`. Modes: default and custom. There is no `disabled`: turning installation off is `[web_apps] enabled = false`.

```swift
public final class WebAppInstallRequest: BDKUIRequest {
    public let app: WebAppInfo                   // name, origin, icons, scope as the manifest declares them
    public let source: Source                    // .page (beforeinstallprompt's prompt()), .command (webApps.install)
    public let screenshots: [WebAppIcon]         // from the manifest, when it has any
    public func install(confirmedBy event: NSEvent)   // installs into the page's profile
    public func cancel()                         // the page's prompt() resolves with "dismissed"
}
```

| | |
|---|---|
| Raised by | A page calling `prompt()` on its `beforeinstallprompt` event, with a user gesture as the web requires; `webApps.install` from a page or a session |
| Default presenter | A sheet on the page's window: the app's icon and name, its origin, Install and Cancel, with Cancel the default |
| What a presenter must show | The origin, next to the name. The name and icon are chosen by the site and can imitate another product. The default presenter always shows it; a custom presenter is given the same facts (ui-requests.md section 3). |
| Gesture | `install` is a granting completion: refused without an `NSEvent` (`ui.gestureRequired`) |
| With `[web_apps] enabled = false` | No request is ever raised. `beforeinstallprompt` does not fire, as today. |

### 4.2 The app's window

A launch is a new-window request ([ui-requests.md](ui-requests.md) 5.6) with a new kind, `.webApp`, and the app in `request.webApp`. The page already exists, in the app's profile, loading the start URL (or the shortcut's).

| | |
|---|---|
| Raised by | `webApps.launch`; a `window.open` or `target="_blank"` from an app page to a URL inside its scope |
| Custom | The application places `newContents.view` wherever it likes: its own window, a pinned sidebar entry, a split pane. It is the application's page from then on. |
| Default presenter | A plain `NSWindow` titled with the app's name, no address bar, sized from the last launch. When the page leaves the app's scope the window shows the page's origin in a bar above it, until it returns. |
| Out of scope | `page.webApp.isInScope` turns false and a state event fires. A custom presenter must then show the origin: a standalone window with no address bar is otherwise a place to imitate another site. |
| Links from elsewhere | A link clicked in an ordinary tab stays in that tab, whether or not an installed app covers it. Chromium's link capturing is off. |
| No `disabled` | As for every new-window request |

`page.webApp` is set only on a page that was launched as an app. Navigating an ordinary tab to an installed app's start URL does not make it one.

## 5. Profiles

An app is installed in exactly one profile and uses that profile's cookies, storage, service workers, permissions and extensions.

| Case | Behaviour |
|---|---|
| A page asks to be installed | It is installed in the page's profile |
| The application installs into a profile of its choice | `otherProfile.webApps.install(.url(startURL), confirmedBy:)`. BDK loads the URL in a hidden page of that profile, reads its manifest and installs. Conditional: whether Chromium's installer can be driven this way without a visible tab is checked in T-PWA-01. |
| The same app in two profiles | Two independent installs, with separate storage. The identifier is the same; `WebAppInfo.profileID` tells them apart. |
| Launch | Always in the profile it is installed in. There is no cross-profile launch. |
| A private profile | `install` fails with `webApps.privateProfile`, no page in it receives `beforeinstallprompt`, and `list` is empty. The parent's apps are not launchable in it. |
| Deleting a profile | Removes its apps with the rest of its data |

## 6. What an installed app can do

| Manifest or web feature | v1 | Notes |
|---|---|---|
| `display`: `standalone`, `minimal-ui` | Yes | The page's `display-mode` media query reports it. `fullscreen`, `window-controls-overlay`, `tabbed` and `borderless` fall back to `standalone`. |
| `scope`, `start_url`, `id`, `theme_color`, `background_color`, `icons` | Yes | Reported in `WebAppInfo` |
| `shortcuts` | Reported | In `WebAppInfo.shortcuts`; the application decides where to show them. No Dock menu. |
| Badging (`navigator.setAppBadge`) | Reported | `webApps.badgeChanged` and `WebAppInfo.badge`. Nothing is drawn by default: there is no per-app Dock icon. |
| Notifications | Yes | Through the `notifications` surface ([ui-requests.md](ui-requests.md) 5.13), attributed to the browser application, not to the web app |
| Offline use, Cache Storage, IndexedDB | Yes | Chromium's, unchanged |
| Push | Only with `[services.push]` configured | Section 7 |
| Manifest updates | Applied by Chromium | A change of name or icon is the open item in section 9 |
| `file_handlers`, `protocol_handlers`, `share_target`, `launch_handler`, run at login | No | They need operating-system registration (section 8). The APIs report themselves unavailable. |
| Installing a page that has no manifest ("install this site as an app") | Later | |
| Isolated Web Apps, sub-apps, preinstalled and default apps, sync of installed apps | No | Off. The network audit covers that nothing is fetched for them. |

## 7. Push

Web Push is a web API of its own: a site in an ordinary tab can subscribe, installed or not. It is configured separately from `[web_apps]` for that reason, and because Chromium delivers push through Google's messaging service (GCM/FCM), which invariant 9 allows only when the application asks for it.

| `[services.push] provider` | Behaviour |
|---|---|
| `"none"` (default) | `PushManager.subscribe()` rejects, `permissionState()` answers `denied`, and the runtime makes no contact with Google's messaging hosts |
| `"google"` | Chromium's own push client. The application supplies its Google credentials from Swift before start, never in `bdk.toml` (as [../amendments.md](../amendments.md) 2.3). Missing credentials are a start error. |

What a developer must know, and where BDK says it:

- `provider = "google"` requires a project the developer sets up in Google's console, under the developer's own account and terms. Google sees every subscription and can meter or refuse it. What exactly has to be created there (an API key, a sender project, or both) is not verified: T-PWA-05 settles it from the source at the pin and writes it here and in [configuration.md](configuration.md) 3.8.
- Push is delivered only while the browser is running. BDK has no background mode.
- `bdk build` and `bdk doctor` print the warning `services.push_google` whenever the provider is `"google"`, naming the console requirement and this section.
- `bdk build` and `bdk doctor` print the note `web_apps.push_unconfigured` when `[web_apps] enabled = true` and the push provider is `"none"`: installed apps work, and receive no push messages.
- `runtime.info` reports `webApps.enabled` and `push.provider`, so an application can show the state in its own settings.

A push provider written by the application (`provider = "app"`, its own push service) is Later. The key is a provider key so that it can be added.

## 8. Not in v1: operating-system integration

Chromium on macOS gives each installed app an app shim: a small application bundle in `~/Applications`, signed ad hoc, that shows a Dock icon and talks to the browser process. Shims are what make file handlers, protocol handlers, run at login, per-app notification attribution and "open from Spotlight" work. BDK does not build or register them in v1:

- they add a second signed artefact per installed app, created on the user's machine, to an application whose signing BDK already rewrites (architecture 13);
- they are macOS-only code, and every other platform does this differently (architecture 14).

An application gets the same user-facing result inside its own UI from `webApps.list`, `launch` and the badge events. A Dock icon per app is Later, as `[web_apps] os_integration`.

## 9. What is still open

| Item | Settled by |
|---|---|
| Whether a page launched as an app can live in a BDK host context without Chromium's app-type `Browser`, and still report its display mode and scope | T-PWA-01 (risk R32) |
| Which entry points reach Chromium's install dialog, and whether routing them needs a patch or an embedder hook | T-PWA-01 |
| Installing into a chosen profile from a URL with no visible tab | T-PWA-01 |
| What Chromium does when a manifest update changes an app's name or icon (it has a review dialog, which is views), and whether BDK raises a request, applies it or holds it | T-PWA-01; the answer goes in section 6 |
| What push needs from the developer's Google project, and whether `InitializeAndOverrideAPIKey()` supplies it | T-PWA-05 |
| Whether Background Sync and Periodic Background Sync work for an installed app while the browser runs | T-PWA-04, as recorded results |
