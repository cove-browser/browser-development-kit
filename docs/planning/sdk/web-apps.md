# BDK installable web apps

Status: proposal, 2026-11-25. Decided for M9 by the product owner ([../amendments.md](../amendments.md) 2.24). Based on [../architecture.md](../architecture.md) sections 7, 11.1 and 16 and on the shape of [extensions.md](extensions.md). Chromium's web-app system was read at the pin for T-PWA-01 on 2026-11-25 (read, not run): the findings, with source references, are section 10, and the open items of section 9 are answered there. Status labels are in [README.md](README.md) section 2.

Built (2026-11-25): the configuration and its diagnostics (T-PWA-02), and the stub, the core, the C ABI and the Swift API of sections 2 to 5 (T-PWA-03), tested on the stub. The engine (T-PWA-04, 2026-11-26, on Chromium 158): installs follow `[web_apps] enabled`; a page's `prompt()` and `webApps.install` of a page are `web_app_installs` requests (patch `bdk-let-an-embedder-present-the-web-app-install-dialog`), with a sheet of BDK's as default presenter; `list`, `get`, `installability`, `uninstall` (with `clearSiteData`) and `launch`; the events and badges; a launched page reports the app's display mode in scope and the `WEB_APP` event at the scope's edge, and BDK's app window shows the origin above an out-of-scope page; operating-system integration off (patch `bdk-let-an-embedder-turn-off-web-app-os-integration`), with link capturing and preinstalled apps off at start. Not built on the engine: installing from a URL (`webApps.install {url}` fails with `webApps.notInstallable`), and push (T-PWA-05). Engine scenarios: `tests_engine_web_apps.c` (6) and the network audit with web apps on.

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

Built on the stub (T-PWA-03); the engine is T-PWA-04. The value types are generated from `packages/core/interface/webApps.json`, so they carry the `BDK` prefix of every generated type (`BDKWebAppInfo`, `BDKWebAppIcon`, `BDKWebAppShortcut`, `BDKWebAppBadge`, `BDKWebAppInstallability`); `BDKWebAppPageState`, `BDKWebAppInstallSource` and `WebAppInstallRequest` are written by hand (`packages/swift/Sources/BDK/BDKWebApps+Host.swift`, `UI/BDKUIRequests+WebApps.swift`). The sketches below keep the short names.

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

- `install` and `uninstall` take the `NSEvent` that confirmed them, as every granting completion does ([ui-requests.md](ui-requests.md) section 3). They are the C functions `bdk_web_app_install` and `bdk_web_app_uninstall` (`bdk_web_apps.h`): the command's arguments and invoke options, plus the event; anything but an `NSEvent` fails the call with `ui.gestureRequired`. A command from the host has no gesture to give, as for extensions (conventions.md, "Extensions (M5)", item 7).
- `install` called by the application raises no request: the application has already asked its user. A page's own `prompt()` raises one (section 4.1). The generated `install(url:)` is the command, which always raises one.
- Every call fails with `webApps.disabled` while `[web_apps] enabled` is false; `list` included.
- `launch` answers the page, already adopted, with `page.webApp` set; the same page is the `newContents` of the `.webApp` new-window request that places it. The command underneath (`webApps.launch`) answers `{contents}`, the page's handle; Swift names it `launchContents`.
- `installability(of:)` names the page in the invoke options, as page commands do; the command takes no arguments.
- Icons are by reference: `url` as the manifest declares it, resolved, with `width`, `height` and `purpose`; `data`, a `data:` URL, when the runtime has the image (never on the stub). The ABI carries no image buffer for them.
- The C ABI carries identifiers and strings. No AppKit type crosses it: `NSEvent` stays in the Swift binding, as for the other surfaces.

## 3. Commands

For internal pages and automation sessions, and behind the Swift calls above. Call semantics and marks are in [services.md](services.md) section 2.

| Command | Capability | Marks | Arguments | Does |
|---|---|---|---|---|
| `webApps.list` | `webApps.list` | | `{}` | Every app installed in the caller's profile, as `WebAppInfo`; icons by reference |
| `webApps.get` | `webApps.list` | | `{id}` | One; `webApps.notFound` |
| `webApps.installability` | `webApps.list` | | `{}`, the page in the invoke options | `WebAppInstallability` for the page; `broker.invalidArguments` with no page |
| `webApps.install` | `webApps.manage` | G U | `{url?}`; with none, the page in the invoke options | Raises the install request, so a page or session can never install silently. `webApps.notInstallable` (the errors in the message), `webApps.cancelled` when not granted |
| `webApps.uninstall` | `webApps.manage` | G | `{id, clearSiteData?}` | Removes the app and, when asked, its site data |
| `webApps.launch` | `webApps.list` | U | `{id, shortcutUrl?}` | Creates the page in the app's profile and raises a new-window request of kind `.webApp`; answers `{contents}`. A `shortcutUrl` that is not one of the app's shortcuts is `broker.invalidArguments` |

Events, all with capability `webApps.list` and the profile of the app: `webApps.installed {app}`, `webApps.updated {app}` (a reinstall, or a manifest update applied), `webApps.uninstalled {id}`, `webApps.badgeChanged {id, badge}` (`badge` null when cleared).

Capabilities, both grantable to pages and to sessions: `webApps.list` and `webApps.manage`.

Errors are in the domain `webApps`: `disabled`, `privateProfile`, `notFound`, `notInstallable`, `cancelled`, and `gestureRequired` for a page's `prompt()` without user activation. The core refuses for every runtime, before a backend sees the call (`packages/core/runtime/web_apps.cc`): every command while web apps are off, and in a private profile `install`, `uninstall` and `launch` (`privateProfile`), `get` (`notFound`), `list` (`[]`) and `installability` (not installable). `runtime.info` reports `webApps {enabled}` and `push {provider}`; the core adds them to every runtime's answer.

## 4. UI surfaces

### 4.1 Install prompt

Surface key `web_app_installs` in `[ui.surfaces]`, `BDK_SURFACE_WEB_APP_INSTALLS` (37, the place of the ui-requests.md table after the T-UIR-14 surfaces). Modes: default and custom. There is no `disabled`: turning installation off is `[web_apps] enabled = false`.

Payload: `{app, source, screenshots}`: `app` a `WebAppInfo` as the manifest declares it (`installedAt` 0), `source` `page` or `command`, `screenshots` icons. Completions: `grant` (Swift `install(confirmedBy:)`; `install` is accepted as the same) needs the gesture; `deny` (Swift `cancel()`), a rejection, a drop, a withdrawal and the default with no presenter able to ask all install nothing: a page's `prompt()` resolves "dismissed", a command fails with `webApps.cancelled`. The request names the page (`contents`) when a page asked or the command named one, and always the profile it installs into.

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

A launch is a new-window request ([ui-requests.md](ui-requests.md) 5.6) with a new kind, `.webApp`, and the app in `request.webApp`. The page already exists, in the app's profile, loading the start URL (or the shortcut's). Payload: `{kind: "webApp", disposition: "newWindow", targetURL, features: {}, userGesture, newContents, webApp}`, with no opener; `accept` keeps the page, and `deny`, a rejection or a drop destroy it, as for every new-window request.

Scope is reported on the page: a page launched as an app gets the contents event `BDK_CONTENTS_EVENT_WEB_APP` (12) with detail `{appId, isInScope}` whenever a committed navigation leaves the app's scope or comes back into it (Swift: `BDKWebContents.Event.webAppScope`, and `page.webApp.isInScope`). It starts in scope, so there is no event for the first load.

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
| The application installs into a profile of its choice | `otherProfile.webApps.install(.url(startURL), confirmedBy:)`. BDK loads the URL in a hidden page of that profile, reads its manifest and installs. Read at the pin (T-PWA-01): yes, through two scheduler calls and no dialog (section 10.3). |
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
| Manifest updates | Applied by Chromium, except a change of name or icon | A change of name or icon is held, as Chromium holds it until its app window's review dialog; `WebAppInfo.hasPendingUpdate` is true and `webApps.updated` fires. Applying it is Later (section 10.4) |
| `file_handlers`, `protocol_handlers`, `share_target`, `launch_handler`, run at login | No | They need operating-system registration (section 8). The APIs report themselves unavailable. |
| Installing a page that has no manifest ("install this site as an app") | Later | |
| Isolated Web Apps, sub-apps, preinstalled and default apps, sync of installed apps | No | Off. The network audit covers that nothing is fetched for them. |

## 7. Push

Web Push is a web API of its own: a site in an ordinary tab can subscribe, installed or not. It is configured separately from `[web_apps]` for that reason, and because Chromium delivers push through Google's messaging service (GCM/FCM), which invariant 9 allows only when the application asks for it.

| `[services.push] provider` | Behaviour |
|---|---|
| `"none"` (default) | Pages have no Push API (the engine starts Chromium with the Blink feature `PushMessaging` off, T-PWA-05): `PushManager` and `registration.pushManager` are absent, so a site finds push unsupported, and the runtime makes no contact with Google's messaging hosts, which Chromium's push client checks in with at the first subscription. This departs from the plan (`subscribe()` rejecting, `permissionState()` answering `denied`): Chromium has no embedder switch for that without a patch, and an absent API is how BDK turns off the other APIs it does not offer. Scenario `engine_privacy_with_no_push_provider_pages_have_no_push_api` |
| `"google"` | Chromium's own push client. The application supplies its Google credentials from Swift before start, never in `bdk.toml` (as [../amendments.md](../amendments.md) 2.3). Missing credentials are a start error. |

What a developer must know, and where BDK says it:

- `provider = "google"` requires a project the developer sets up in Google's console, under the developer's own account and terms. Google sees every subscription and can meter or refuse it. What exactly has to be created there is not verified. Read at the pin for T-PWA-01 (section 10.6): Chromium's push client uses no API key and no project of the developer's, so the requirement may be one of terms rather than credentials; T-PWA-05 settles it and writes it here and in [configuration.md](configuration.md) 3.8. Until then the runtime asks for the application's Google key at start, as for the other Google providers.
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

| Item | Settled by | Answer (T-PWA-01, 2026-11-25, read, not run) |
|---|---|---|
| Whether a page launched as an app can live in a BDK host context without Chromium's app-type `Browser`, and still report its display mode and scope | T-PWA-01 (risk R32) | **Yes; the fallback of R32 is not needed.** The display mode comes from the page's `WebContentsDelegate`, which is BDK's own; scope is a registrar lookup. Section 10.1 |
| Which entry points reach Chromium's install dialog, and whether routing them needs a patch or an embedder hook | T-PWA-01 | Every install path that shows a dialog goes through one function; routing it needs one small patch. The Web Install API (`navigator.install()`, `<install>`) has views dialogs of its own and stays off in v1. Section 10.2 |
| Installing into a chosen profile from a URL with no visible tab | T-PWA-01 | Yes, with no patch: the manifest is read in a hidden page, then two scheduler calls install without a dialog. Not through Chromium's user-install command, which needs a visible page. Section 10.3 |
| What Chromium does when a manifest update changes an app's name or icon (it has a review dialog, which is views), and whether BDK raises a request, applies it or holds it | T-PWA-01; the answer goes in section 6 | Chromium applies other changes silently and holds an identity change as a pending update, shown only from an app window's menu. BDK holds it too and reports `hasPendingUpdate`; applying it is Later. Section 10.4 |
| What stays off, and what keeps it off | T-PWA-01 | Link capturing, preinstalled apps, operating-system integration and app shims need a feature off or a patch; sync and Isolated Web Apps are already off. Section 10.5 |
| What push needs from the developer's Google project, and whether `InitializeAndOverrideAPIKey()` supplies it | T-PWA-05 | Read for T-PWA-01, not settled: the push client uses no Google API key at all. Section 10.6 |
| Whether Background Sync and Periodic Background Sync work for an installed app while the browser runs | T-PWA-04, as recorded results | Open |

## 10. Chromium's web-app system at the pin (T-PWA-01)

Read at `158.0.8127.37` in the managed checkout, 2026-11-25. Nothing was built or run: every row is from reading the source, and T-PWA-04's engine scenarios are what confirm it. Paths are under `chrome/browser/` unless they start with another top-level directory.

### 10.1 An app's page in a BDK host context (risk R32)

| Finding | Source |
|---|---|
| The `display-mode` media query reads the page's `WebContentsDelegate::GetDisplayMode`, sent to the renderer with the visual properties. A BDK page's delegate is `EngineContents`, which does not override it today, so every page reports `browser` | `content/public/browser/web_contents_delegate.h:582`, `content/browser/web_contents/web_contents_impl.cc:5504-5506`, `content/browser/renderer_host/render_widget_host_impl.cc:1135` |
| Chrome's own answer is `BrowserWebContentsDelegate::GetDisplayMode`: `standalone` (or `minimal-ui`, `window-controls-overlay`, `tabbed`, `unframed`) only for an app-type `Browser` with an `AppBrowserController` | `ui/browser_web_contents_delegate/browser_web_contents_delegate.cc:1271-1308` |
| The `AppBrowserController` is made from the `Browser`'s `app_name` when its features are created, whatever window the `Browser` has, so an app-type host context with a `BdkBrowserWindow` would get one. Not taken: the controller drives the app frame (`custom tab bar`, menus, the update dialog below) and was not read for views assumptions | `ui/browser_window/internal/browser_window_features.cc:280-282`, `ui/web_applications/web_app_launch_utils.cc:701-725` |
| Scope is a registrar question: `WebAppRegistrar::IsUrlInAppScope(url, app_id)` and `FindBestAppWithUrlInScope`; the tab helper keeps the in-scope app of every page and offers `SetIsInAppWindow`, which only the app browser controllers call | `banners/app_banner_manager_desktop.cc:254-257`, `web_applications/web_app_tab_helper.h:85-96` |
| Chromium's launch (`WebAppUiManagerImpl::LaunchWebApp` and the launch process) creates an app `Browser` and its views frame. BDK does not call it: the engine creates the page itself, in the app's profile | `ui/web_applications/web_app_ui_manager_impl.cc:562-567` |

Decision: an app's page lives in an ordinary host context (`TYPE_NORMAL`). The engine (T-PWA-04) answers `GetDisplayMode` from `EngineContents` with the app's effective display mode while the page is an app page, checks scope against the registrar at each committed navigation (the `WEB_APP` contents event of section 4.2), and calls `WebAppTabHelper::SetIsInAppWindow(app_id)` so that Chromium's own readers of "in an app window" (`webapps_client_desktop.cc:158-163`, `manifest_update_manager.cc:86-87`) see it. All overlay code, no patch. Not taken with it: the launch queue and `launch_handler` (not in v1, section 6).

### 10.2 Install dialogs

| Entry point | Path to the dialog | Source |
|---|---|---|
| A page's `beforeinstallprompt` `prompt()` | `AppBannerManagerDesktop::ShowBannerUi` → `CreateWebApp` → `web_app::CreateWebAppFromManifest` → `FetchManifestAndInstall` with `OnWebAppInstallShowInstallDialog` as its dialog callback | `banners/app_banner_manager_desktop.cc:213-231`, `:274-281`; `ui/web_applications/web_app_dialog_utils.cc:351-415` |
| The machine-learning install promotion | `OnMlInstallPrediction` → `CreateWebApp`, as above | `banners/app_banner_manager_desktop.cc:196-202` |
| The omnibox install icon and the menu | `ShowPwaInstallDialog`, `CreateWebAppFromCurrentWebContents`: Chrome's own window, not reachable from a BDK page | `ui/web_applications/web_app_dialog_utils.cc:287`, `:435-467` |
| `navigator.install()` for the page's own manifest | `WebInstallServiceImpl` → `WebAppUiManager::TriggerInstallDialogForManifestInstall` → `CreateWebAppForManifestInstall` → `OnWebAppInstallShowInstallDialog` | `web_applications/web_install_service_impl.cc:1110`, `ui/web_applications/web_app_ui_manager_impl.cc:633-645`, `ui/web_applications/web_app_dialog_utils.cc:418-432` |
| `navigator.install()` for another URL, and its launch prompt | A background install that opens a tab, then `ShowWebInstallAppLaunchDialog` (views) | `web_applications/web_install_service_impl.cc:1249`, `web_app_ui_manager_impl.cc:648-656`, `web_app_command_scheduler.h:258-267` |
| `navigator.install()` where installs are off | `TriggerInstallNotSupportedDialog` → `ShowInstallNotSupportedDialog` (views), also with `profile.web_app.install_by_user_enabled` false, which is BDK's state today | `web_applications/web_install_service_impl.cc:754-760`, `:983-984`; `ui/web_applications/web_app_ui_manager_impl.cc:312-328` |

The one convergence is `OnWebAppInstallShowInstallDialog` (`ui/web_applications/web_app_dialog_utils.cc:119`): it receives the `WebAppInstallInfo` and the acceptance callback, and picks the simple, detailed or DIY views dialog (`:85-116`, `:153-218`). `AppBannerManagerDesktop::CreateWebApp` is not virtual and its only seam is a factory marked for testing (`app_banner_manager_desktop.cc:60-74`), and `WebAppUiManager::Create` has no embedder seam (`ui/web_applications/web_app_ui_manager_impl.cc:308-310`). So:

- **Patch** `bdk-let-an-embedder-present-the-web-app-install-dialog`: at the top of `OnWebAppInstallShowInstallDialog`, an embedder-set presenter takes the install info, the initiating page and the acceptance callback; BDK raises the `web_app_installs` request and answers the callback. One function in one file, in the shape of the other `bdk-let-an-embedder-present-*` patches.
- **Off at start** (`WebAppInstallation` and `InstallElement`, both enabled on macOS at the pin: `third_party/blink/renderer/platform/runtime_enabled_features.json5:7327-7334`, generated `features_generated.cc` `kWebAppInstallation`, `kInstallElement`), whatever `[web_apps] enabled` says. Their binder is added only when one is on (`chrome_browser_interface_binders.cc:551-556`). With them on, `navigator.install()` reaches a views dialog in every configuration, including today's (the not-supported dialog), which is the crash T-UIR-10 removed for the others; T-UIR-10 did not list it. The Web Install API is Later. Both are off at start since T-PWA-06 (`chromium_main_mac.mm`; scenario `engine_dialogs_page_initiated_install_apis_are_absent`, with installs off and on).

### 10.3 Installing from a URL into a chosen profile

| Finding | Source |
|---|---|
| Chromium's user install (`FetchManifestAndInstall`) aborts unless the page is visible, and again if it is hidden while the command runs | `web_applications/commands/fetch_manifest_and_install_command.cc:241-243`, `:302-332` |
| `FetchInstallInfoFromInstallUrl(manifest_id, install_url)` loads the URL in the command manager's own hidden web contents and returns the `WebAppInstallInfo`; it needs the manifest id beforehand and fails with `kWrongManifestId` on another | `web_applications/web_app_command_scheduler.h:173-186`, `commands/fetch_install_info_from_install_url_command.cc:200-216` |
| `InstallFromInfoWithParams` installs an info with the given `WebAppInstallParams` and no dialog | `web_applications/web_app_command_scheduler.h:199-208` |

So BDK installs from a URL in three steps, with no patch: load the URL in a hidden page of the target profile (a page of its parking context) and read the manifest id through the page's installable data; `FetchInstallInfoFromInstallUrl`; `InstallFromInfoWithParams` with a user install source (which one is T-PWA-04's) and the install state of section 10.5. The application's confirmed install of a page uses the same last two steps, so neither depends on the page being on screen.

### 10.4 Manifest updates

| Finding | Source |
|---|---|
| `ManifestUpdateManager` runs `ScheduleManifestSilentUpdate` when a page in an app's scope loads; changes that are not security-sensitive are applied silently (`kAppSilentlyUpdated`, also icon changes under the small-difference threshold) | `web_applications/manifest_update_manager.cc:135-160`, `commands/manifest_silent_update_command.cc:254-302` |
| A name or icon change is recorded as `pending_update_info` (`kPendingUpdateRecorded_*`) and not applied | `commands/manifest_silent_update_command.cc:260-272` |
| The pending update is offered only from an app window: `WebAppBrowserController::HasPendingUpdate` and `TriggerAppUpdateOrMigrationDialog` show `ShowWebAppReviewUpdateDialog` (views); `ScheduleApplyPendingManifestUpdate` applies it | `ui/web_applications/web_app_browser_controller.cc:273-293`, `:880-889`; `web_applications/web_app_command_scheduler.h:239-247` |

Decision: BDK holds it, as Chromium does when nobody opens the menu. The app keeps its installed name and icon (the anti-imitation property the dialog protects), `WebAppInfo.hasPendingUpdate` turns true and `webApps.updated` fires. A command that shows the change and applies it (`webApps.applyUpdate`, a request with a gesture) is Later.

### 10.5 What stays off

| What | Today at the pin | What keeps it off in BDK |
|---|---|---|
| Link capturing | On: `PwaNavigationCapturing` enabled by default, default state `reimpl_default_on` (`content/public/common/content_features.cc:1611-1620`, `web_applications/link_capturing_features.cc:39-50`) | Disable `PwaNavigationCapturing` at start (T-PWA-04) |
| Preinstalled apps | `DefaultWebAppInstallation` enabled (`common/chrome_features.cc:164-166`), checked first by `PreinstalledWebAppManager::Load` (`web_applications/preinstalled_web_app_manager.cc:814-821`) | Disable it at start |
| Operating-system integration and app shims | A user install sets `INSTALLED_WITH_OS_INTEGRATION` (`commands/fetch_manifest_and_install_command.cc:680-681`; the default of `WebAppInstallParams`, `web_app_install_params.h:89-96`), and `OsIntegrationManager::Synchronize` then runs the shortcut (app shim), file, protocol, shortcut-menu, run-on-login and uninstall sub-managers (`os_integration/os_integration_manager.cc:199-212`, `:228-266`) | **Patch** `bdk-let-an-embedder-turn-off-web-app-os-integration`: no sub-managers when the embedder says so, so `Synchronize` returns at once (`:263-266`). The install state stays `INSTALLED_WITH_OS_INTEGRATION`, which badging and notification attribution filter on (`web_applications/web_app_filter.cc:84-91`), so nothing else changes |
| Badging | `BadgeManager` binds `navigator.setAppBadge` for every frame and gives changes to a per-platform delegate, on macOS the Dock (`badging/badge_manager.cc:64-79`, `:270-288`; `chrome_browser_interface_binders.cc:544-545`) | No patch: BDK replaces the delegate with `BadgeManager::SetDelegate` (public) and emits `webApps.badgeChanged` |
| Private profiles | No `WebAppProvider` for an off-the-record profile, and `AreWebAppsUserInstallable` is false there (`web_applications/web_app_utils.cc:225-257`, `:274-282`) | Nothing more; the core refuses first (section 3) |
| Installs by users | `IsWebAppInstallByUserPolicyEnabled` reads the policy value BDK forces off today (`web_applications/web_app_utils.cc:244-251`; `chromium/bdk/engine/ui_browser_dialogs.cc:407`) | `[web_apps] enabled` sets that default instead (T-PWA-04) |
| Isolated Web Apps | `IsolatedWebApps` disabled outside ChromeOS (`content/public/common/content_features.cc:675-680`) | Nothing |
| Sync of installed apps | Rides on the sync service, which BDK never starts (sign-in is removed). The sync bridge was not re-read | Nothing; the network audit (T-PWA-04) checks it |

### 10.6 Push and the console (the reading half of T-PWA-05)

| Finding | Source |
|---|---|
| The desktop GCM client talks to `android.clients.google.com/checkin`, `android.clients.google.com/c2dm/register3` and `mtalk.google.com` | `google_apis/gcm/engine/gservices_settings.cc:32-37` |
| It authenticates with the device identity a check-in returns (`AidLogin <android id>:<security token>`), not with a Google API key; the check-in describes itself as `DEVICE_CHROME_BROWSER` with Chrome's build | `google_apis/gcm/engine/registration_request.cc:43`, `:231-232`; `checkin_request.cc:146-150` |
| No code under `google_apis/gcm` or `components/gcm_driver` reads `google_apis::GetAPIKey` | (searched; nothing found) |
| A Web Push subscription is keyed by the site's VAPID application server key, and its endpoint is `fcm.googleapis.com` | `push_messaging/push_messaging_service_impl.cc:1018-1025`, `components/push_messaging/push_messaging_constants.cc:7-15` |

So `InitializeAndOverrideAPIKey()` does not configure push, and Chromium's push client needs no project in Google's console: it uses the device registration every Chrome makes. What `provider = "google"` asks of a developer is therefore a terms question (a browser that is not Chrome checking in as one), not a credential, and T-PWA-05 must settle it before the console steps are written. Until then T-PWA-02 keeps the rule this document states, "missing credentials are a start error", by treating `[services.push] provider = "google"` like the other Google providers: it needs the application's Google key at start (`config.keyMissing`), and the key is then not unused. T-PWA-05 replaces the rule with what push really needs.
