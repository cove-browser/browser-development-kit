# Building a production browser on BDK

Status: proposal, 2026-11-15. This is the map from "what a shippable browser needs" to the API that provides it, the work that stays with the application, and the milestone in [../roadmap.md](../roadmap.md) where it arrives. It introduces no API of its own; every entry points at another document in this directory. Nothing here has been built, and several rows depend on results that milestone M0 has not produced yet. Status labels are in [README.md](README.md) section 2.

## 1. The division of work

| BDK supplies | The application supplies |
|---|---|
| The engine, as a prebuilt, signed-in-place runtime, and security releases of it | The decision to adopt each release, and the update that ships it to users |
| Profiles, pages, native views, navigation, events | Windows, layout, tabs or spaces or whatever organises pages, and all of their state |
| Chromium's services behind one typed API: history, bookmarks, downloads, passwords, autofill, permissions, cookies, extensions, translation, search | The UI over those services, unless a built-in or bundled internal page is enough |
| A working default presenter for every browser-owned prompt, dialog and page | Custom presenters, only where the product wants its own |
| Extension runtime, default extension UI, install policy, compatibility checks | Toolbar buttons and panels, if it wants extensions in its own layout |
| Branded internal pages with per-page capabilities | The pages it wants to replace, and their grants |
| An automation host with scoped sessions and an audit stream | Agent identity, consent, policy, any CLI or gateway |
| Non-Google defaults for translation, reputation and geolocation | Provider choice, keys, and the disclosures that go with them |
| A network audit showing no Google-directed traffic by default | Its own privacy policy, and consent for anything it adds |

With no application UI code beyond a window and an address field, the result must already be usable for everyday browsing. That is the exit criterion of M3.

## 2. Feature map

"Application work" is what remains after BDK's defaults. "None" means the default is complete.

### 2.1 Shell and pages

| Feature | BDK API | Application work | Status |
|---|---|---|---|
| Application lifecycle | `BDKApplication`, `BDKRuntime.start`, `terminate(replyingTo:)` ([swift-api.md](swift-api.md) 3, 4) | The delegate, menus, windows | Plan, M2 |
| Showing a page | `profile.webContents.create()`, `page.view` (6, 7) | Layout | Plan, M2 |
| Tabs, spaces, pinned pages, split view | None. BDK has no such types. Views attach anywhere; two visible pages in a window are both foreground. | The whole model and its UI | Plan; three or more panes depend on T-ADP-06 (risk R2) |
| Several windows; moving a page between them | Add the view to another window (7) | Window management | Plan, M2 |
| Popups and `target="_blank"` | `NewWindowRequest` ([ui-requests.md](ui-requests.md) 5.6) | Where the new page goes. Default: a plain window. | Plan, M2 to M3 |
| Session restore | `page.sessionState()`, `Options.restoring` (6.8) | Saving and restoring layout and page state. Chromium restores nothing. | Plan, M2 |
| Recently closed pages | None | Keep the session state of closed pages | Application |
| Crash recovery | `processGone` event; `reload()` (6.4) | What a crashed page looks like | Plan, M2 |
| Unresponsive pages | `UnresponsivePageRequest` (5.19) | None | Accepted (S3, 2026-11-25); core, stub and Swift built, engine not (T-UIR-15) |
| Background-page throttling | Chromium's own, unchanged: timer throttling, lower process priority and freezing for pages whose view is hidden. It follows view visibility. | None | Plan, M2 |
| Discarding pages to save memory | Chromium discards a hidden page under critical memory pressure whether BDK plans for it or not, and its Memory Saver does so on a timer when switched on (off by default). BDK surfaces it: `page.isDiscarded`, a `discarded` event, `page.discard()`, `page.isAutoDiscardable`, and `performance.memorySaver.*` settings ([swift-api.md](swift-api.md) 6.1, 6.2) | Show a discarded page as such if the product wants to; mark pages that must never be discarded, such as pinned ones; choose the Memory Saver default | Added (S15) |
| Menus and shortcuts | `BDKKeyboardDelegate`, `PageCommand` (9) | The menu bar and every shortcut | Plan, M2 |
| Fullscreen video | `FullscreenRequest` (5.12) | None | Plan, M2 to M3 |
| Picture-in-picture | Chromium's own window for video; `NewWindowRequest` for document picture-in-picture | None | Plan; unverified outside `BrowserView` |

### 2.2 Navigation and the address bar

| Feature | BDK API | Application work | Status |
|---|---|---|---|
| Back, forward, reload, stop, history list | `page.navigation` (6.3) | Buttons | Plan, M2 |
| Cancelling or redirecting a navigation | Navigation policy ([swift-api.md](swift-api.md) 6.3) | The rules | Product owner, 2026-11-25; built on the stub and the engine (T-SWF-10, T-SWF-11, M9) |
| What to show in the address field | `page.displayURL`, `page.security` (6.2) | The field. Use `displayURL`, not `url`, or the field can be spoofed during a navigation. | Plan, M2 |
| Security indicator and certificate viewer | `page.security`; `permissions.certificateExceptions` | The indicator and a details view | Plan, M3 |
| Typed text: URL or search | `search.classify` ([services.md](services.md) 5.13) | Call it on submit | Added (S1) |
| Suggestions while typing | `search.suggestions` | The dropdown | Added (S1) |
| Search engines | `[search]` in `bdk.toml`; `search.engines` and friends | A default engine. None is compiled in. | Added (S1) |
| Loading progress, hovered link | `estimatedProgress`, `hoveredLinkChanged` | Indicators | Plan, M2 |
| Find in page | `page.find`, `FindRequest` (5.11) | None | Plan, M3 |
| Zoom | `page.zoom` | Menu items | Plan, M2 |
| Error pages | Chromium's, with the browser's name | None. Replacing them is not planned. | Plan |

### 2.3 Browser data

| Feature | BDK API | Application work | Status |
|---|---|---|---|
| Profiles | `runtime.profiles` (5) | A switcher, if the product has profiles | Plan, M2 |
| Private browsing | `profile.openPrivate()` | A way to open one, and an indicator | Plan, M2; mapping unverified |
| History | Recorded by Chromium; `history.*`; the `history` route | None with the built-in page | Plan, M3 |
| Bookmarks | `bookmarks.*`; the `bookmarks` route | A "bookmark this page" control; a bookmarks bar if wanted | Plan, M3 |
| Downloads | `downloads.*`; downloads surface (5.7); the `downloads` route | None | Plan, M3 |
| Favicons and most-visited | `favicons.get`, `topSites.list` (5.14) | Use them in its UI | Added (S12) |
| Clear browsing data | `privacy.clear`; in the built-in settings page | None | Plan, M3 |
| Import from another browser | `bookmarks.import` for HTML only | Anything more | Later |
| Sync between devices | Read, write and change events on every service ([services.md](services.md) section 8) | The entire sync service, its accounts and its encryption | Application, by design |

### 2.4 Credentials and forms

| Feature | BDK API | Application work | Status |
|---|---|---|---|
| Save and fill passwords | Chromium's password manager; `passwords` surface (5.8); Keychain-protected storage | None | Plan, M3 |
| Password management UI | `passwords.*` ([services.md](services.md) 5.9); the `passwords` route | None with the built-in page | Plan, M3 |
| Address and card autofill | `autofill.*`; `autofill` and `autofill_suggestions` surfaces | None | Plan, M3; suggestions surface accepted (S3, 2026-11-25) |
| Passkeys and security keys | `WebAuthnRequest` (5.18), one request per step; `BDKWebAuthnSupport` | None for security keys. Phones: `NSBluetoothAlwaysUsageDescription` and the Bluetooth entitlement. System passkeys: Apple's browser passkey entitlement | Built (M4, T-UIR-09; security keys tested with a virtual key). Phones and system passkeys unverified (T-UIR-12, M9); Touch ID passkeys not offered (T-UIR-11, M9) |
| HTTP authentication | `HTTPAuthRequest` (5.3) | None | Plan, M3 |
| Its own credential store instead of Chromium's | `[services.credentials] provider = "app"`: Chromium still detects forms and fills, and the application's store supplies and receives the credentials ([swift-api.md](swift-api.md) 11.2) | The store, its unlock, and its sync | Added (S14); unverified |
| Compromised-password check | None by default: Chromium's is a Google service and is removed. `[services.leak_check] provider = "app"` adds a check after sign-in and `passwords.check` for saved logins ([services.md](services.md) 5.9), with a warning on the `passwords` surface | The provider: which breach service it asks, that service's terms, and the privacy disclosure | Added (S16); unverified |

### 2.5 Permissions, security and privacy

| Feature | BDK API | Application work | Status |
|---|---|---|---|
| Site permission prompts | `PermissionRequest` (5.1) | Usage strings in `Info.plist`; entitlements ([configuration.md](configuration.md) section 6) | Plan, M3 |
| Site settings | `permissions.*`; built-in settings page | None | Plan, M3 |
| Camera, microphone and screen-sharing indicators | `page.capture` (6.2); default "sharing" bar | An indicator in its own page UI, if it has one | Plan; the bar accepted (S3, 2026-11-25), not built (T-UIR-13) |
| Sandbox, site isolation, process locks | Unchanged from Chromium (invariant 1) | Nothing, and it cannot turn them off | Plan |
| Certificate errors | `CertificateErrorRequest` (5.14) | None | Plan, M3 |
| Malware and phishing protection | `[services.reputation]`; `ReputationWarningRequest` | Decide whether the default's reduced protection is acceptable; otherwise configure Web Risk or its own provider | Plan, M6; decision D2 |
| Dangerous-download warnings | `DangerousDownloadRequest` (5.7) | None | Plan, M3 |
| HTTPS-only mode, secure DNS, third-party cookie policy | Settings keys ([services.md](services.md) section 6); `[settings.defaults]` | Choose product defaults | Plan and Added (S7) |
| Revocation lists, Certificate Transparency list, root-store updates | Component data through the chosen route | Possibly a mirror | Conditional, D5, T-PRV-06 |
| No Google account, sync, metrics or crash upload | Removed from the runtime; network audit is a release gate | Nothing | Plan, M6 |
| Geolocation | `[services.geolocation]`, system provider by default | Usage string | Plan, M6 |

### 2.6 Content features

| Feature | BDK API | Application work | Status |
|---|---|---|---|
| Page translation | Default local provider; `TranslationRequest` (5.10); `page.translation` | None | Plan, M6; engine is a spike (risk R14) |
| Printing and save as PDF | `page.print()`, `PrintRequest` (5.9), `page.capture.pdf` | A menu item | Plan, M3 |
| Screenshots | `page.capture.screenshot` | A menu item | Plan, M3 |
| Context menus | `ContextMenuRequest` (5.5) | None | Plan, M3 |
| File upload and save | `FilePickerRequest` (5.4) | None | Plan, M3 |
| Web notifications | `NotificationRequest` (5.13) | None | Plan, M3 |
| Links to other applications | `ExternalProtocolRequest` (5.17) | None | Accepted (S3, 2026-11-25); not routed yet (T-UIR-13) |
| Web Bluetooth, USB, HID, Serial | `DeviceChooserRequest` (5.16) | Usage strings and entitlements | Accepted (S3, 2026-11-25); not built (T-UIR-13) |
| Screen sharing | `ScreenCaptureRequest` (5.16) | None | Accepted (S3, 2026-11-25); not built (T-UIR-13) |
| Spell checking | Expected to use the operating system's on macOS | None | Unverified |
| H.264, AAC, HEVC | Through the operating system's decoders, if the spike holds; no bundled proprietary decoder | None expected; legal review pending | Decision D7, T-CHR-12 |
| DRM video (Widevine) | Not available | Nothing it can do in v1 as planned | Deferred, decision D7 |
| Installable web apps | `profile.webApps`, `WebAppInstallRequest`, a `.webApp` new-window request ([web-apps.md](web-apps.md)) | `[web_apps] enabled = true`; for push, `[services.push]` and a project in Google's console | Product owner, 2026-11-25; not built, T-PWA-01 to T-PWA-05, M9. No Dock icon per app in v1 |

### 2.7 Extensions

| Feature | BDK API | Application work | Status |
|---|---|---|---|
| Running extensions | Chromium's system, unchanged | None | Plan, M5 |
| Finding and using actions with no toolbar | Default extensions menu ([extensions.md](extensions.md) 4.7) | None | Plan, M5 |
| Actions, popups and side panels in the product's layout | `ActionState`, `ExtensionPopupRequest`, `ExtensionSidePanelRequest` | Buttons and containers | Plan, M5; side panel needs a patch |
| Installing | `extensions.install`; sources in `[extensions.install]` | A way for users to get extensions: its own catalogue, CRX links, or a named store it enables after its own legal review (decision D4) | Plan, M5 |
| Management page | The `extensions` route | None | Plan, M5 |
| First-party extensions | `[extensions.bundled]` | The extensions | Added (S6) |
| Extension keyboard shortcuts | `extensions.commands`, `performCommand` | Binding keys | Plan, M5 |
| Knowing what will not work | `CompatibilityReport`; the published matrix | Tell users, where the matrix lists a gap | Plan, M5 |

### 2.8 Product surface

| Feature | BDK API | Application work | Status |
|---|---|---|---|
| Browser name and internal URLs | `[browser]` | A name and a scheme, fixed before the first release | Plan, M4 |
| Application icon | None. It is the application's own `AppIcon` in Xcode, and macOS uses it for the Dock, Finder, system permission prompts and the default-browser list. | The icon | Application |
| Logo inside built-in pages and default UI; helper names; notification icon | `[browser] logo`, defaulting to the application icon; helper names and the alerts-helper icon are set by `bdk build sign` ([configuration.md](configuration.md) 3.1.1) | None, or a logo file | Added (S13); unverified |
| New-tab page | BDK's minimal default; or `Pages/NewTab` | Its own page, usually | Plan, M4 |
| Settings | Chromium's page by default; or `Pages/Settings` with `@bdk/internal/settings` | Its own page, if wanted; plus settings for its own features, registered in Swift | Plan, M4 |
| Any other internal page | `Pages/<Name>`; `native` routes | The pages | Plan, M4 |
| Default browser | `system.defaultBrowser`; URL and document types in `Info.plist`; `application(_:open:)` | The prompt and the URL handler | Plan, M3 |
| DevTools | `page.devTools`, `DevToolsRequest` (5.15) | Docking, if wanted | Plan, M7 |
| Localisation | Chromium's strings for default UI, in the system language | Its own UI's strings | Plan |
| Accessibility | Chromium's, with the accessibility parent set by BDK | Its own UI's accessibility | Plan, M2; VoiceOver pass in T-MAC-03 |
| Crash reports | `[diagnostics]`, `onCrashReport` ([swift-api.md](swift-api.md) 15) | A place to send them and the user's consent | Added (S4) |
| Usage analytics | None | Its own, with consent | Application |
| Agents and scripted automation | `runtime.automation`, `@bdk/automation` | Identity, consent, policy, gateway | Plan, M7 |

## 3. What BDK will never supply

A product plan must budget for each of these.

| Need | Why it is the application's |
|---|---|
| An updater | BDK never updates itself or the application (invariant 10). The application ships its own updates and, with them, each new runtime. |
| A sync service | Excluded by the spec |
| Telemetry | Excluded by the spec |
| Tab, space and window UI and their persistence | BDK prescribes no organisation model |
| Onboarding, a default-browser prompt, import flows | Product decisions |
| Agent permissions and consent | Excluded from v1 by the spec |
| A reputation service equal to Chrome's | No open equivalent exists (risk R5). A licence is the application's to buy. |
| Codec and DRM licences | Decision D7: system decoders, pending legal review; Widevine deferred |
| An extension store | Decision D4; store terms belong to each product |
| Legal review of providers, feeds and store terms | Per legal entity |

## 4. Differences from Chrome a product must disclose

From the plan as it stands. The list will change with the M0 results and the compatibility matrix.

| Area | Difference |
|---|---|
| Reputation | The default provider gives reduced protection: known-threat lists, no real-time checks, no download reputation |
| Passwords | No leak detection unless the application supplies a leak-check provider, and then it is that provider's data, not Google's. No weak or reused password check. |
| Extensions | No Chrome Web Store unless the application enables it (decision D4). `identity.getAuthToken`, `sessions` and `tabGroups` are absent. Behaviour with off-screen pages and with three or more visible pages differs (risks R2, R9). |
| Internal pages | The address shown is `<scheme>://…`; the origin inside the page and in DevTools is `chrome://…` (decision D3) |
| Media | H.264, AAC and HEVC through the system's decoders if the T-CHR-12 spike holds, otherwise open codecs only. No DRM streaming; Widevine is deferred (decision D7). |
| Translation | A local engine with fewer language pairs and different quality from Google's |
| Security patches | A gap between each upstream security release and the application's update (risk R4) |
| Google services | No sign-in, no sync, no Google-backed suggestions or safe browsing unless the application configured them with its own key |

## 5. Launch checklist

### 5.1 Identity, fixed before the first public release

- [ ] `[browser] name` and an explicit `internal_scheme`
- [ ] `[storage] directory` and `keychain_item`, set explicitly
- [ ] Bundle identifier, team and signing identity in Xcode
- [ ] `[browser.user_agent] brand`, if the product wants one
- [ ] An application icon in the Xcode asset catalog, and `[browser] logo` if built-in pages should show a different mark

### 5.2 Configuration review

- [ ] `bdk doctor` reports no errors and every warning is understood
- [ ] Every `[pages.capabilities.*]` grant is the minimum its page needs. `bdk doctor` lists them in one place.
- [ ] No page is granted a capability because "it might need it later"
- [ ] `[pages] diagnostics` and the `flags` route are set deliberately
- [ ] Every surface in `custom` mode has a handler, and each handler was tested against the stub for deny, use-default and grant-with-gesture
- [ ] `[extensions.install]` sources match what the product supports and what its legal review allows
- [ ] `[settings.defaults]` reviewed; none weakens a protection without the product saying so
- [ ] `[chromium.features]` is empty, or each entry has an owner and a reason
- [ ] `[automation.external] enabled` is false unless the product has an authenticator

### 5.3 Security and privacy

- [ ] Reputation provider chosen; if the default, "reduced protection" is stated in the product's documentation
- [ ] Each remote provider has a key from Swift or a build setting, a privacy URL, and a disclosure
- [ ] Keys shipped in the application are restricted by API and quota, or the endpoint remap with a proxy is used
- [ ] The network audit was run with the product's configuration, not only BDK's default
- [ ] Custom presenters for permission, install, credential and interstitial surfaces show every piece of text the request carries
- [ ] Every `app.` command reachable from a page or a session was reviewed as an attack surface: what can a compromised settings page do with it?
- [ ] Crash reports go nowhere without consent
- [ ] The App Sandbox entitlement is not set; resource entitlements match what the product uses

### 5.4 Packaging and release

- [ ] Release build passes `bdk doctor --bundle`
- [ ] Signed with the application's identity, helpers included, and notarized
- [ ] Third-party notices are reachable at the `credits` route
- [ ] The application's updater delivers a full bundle including the runtime
- [ ] Upgrade from the previous release tested with a real profile; rollback tested against `[runtime] downgrade`
- [ ] The compatibility matrix for the BDK version is linked from the product's documentation

### 5.5 Operations, for as long as the browser ships

- [ ] Someone is subscribed to BDK security releases
- [ ] A target exists for how many days after a BDK security release the product ships
- [ ] In source mode: a machine and a person who can rebuild the runtime for each security release, and a rehearsal of doing it
- [ ] `bdk doctor` runs in CI and fails the build when the engine is older than the product's limit

## 6. The order things become possible

| After milestone | An application can |
|---|---|
| M2 | Show pages in its own windows, navigate, move pages between windows, survive a renderer crash, save and restore sessions, use private profiles |
| M3 | Be used for everyday browsing with no custom UI: history, bookmarks, downloads, passwords, autofill, printing, permissions, interstitials |
| M4 | Have its own name, scheme and internal pages, with granted capabilities |
| M5 | Run real extensions, with default or custom UI |
| M6 | Translate pages, locate the user and warn about dangerous sites with no Google service; pass the network audit |
| M7 | Run automation scripts and build an agent gateway; customise DevTools |
| M9 | Be created with `bdk init`, checked with `bdk doctor`, upgraded and rolled back, and shipped from a documented release |

The **Added** items in [README.md](README.md) section 6 have no milestone yet. A browser that ships without S1 (address-bar services), S2 (stable storage identifiers) and S3 (the remaining surfaces) would be missing things users notice in the first minute, so those three are the ones to place first. S15 (page discarding) is next: Chromium discards pages under memory pressure with or without it, so the choice is only whether the application is told.
