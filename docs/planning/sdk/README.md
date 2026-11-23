# BDK SDK surface

Status: proposal, 2026-11-15. This directory describes everything a browser developer touches when building on BDK: configuration, the Swift API, browser-owned UI requests, the service commands, the JavaScript SDKs, the C ABI and the CLI. It follows the BDK Design Specification v0.7 and [../architecture.md](../architecture.md). Nothing here has been built, and no signature has been compiled.

Every type, method, key, command and flag name is a placeholder. The spec leaves individual signatures to the implementation agent, so these documents fix what must exist and how the parts fit, and leave the spelling open. Where a name already appears in the spec or in `architecture.md`, it is reused unchanged.

## 1. Documents

| Document | Read it for |
|---|---|
| [configuration.md](configuration.md) | Every `bdk.toml` table and key, the resolved file, `Info.plist` and entitlement requirements, environment variables, validation errors |
| [swift-api.md](swift-api.md) | The Swift SDK: application object, runtime, profiles, WebContents, views, windows, keyboard, commands, providers, automation, errors, test support |
| [swift-api-reference.md](swift-api-reference.md) | The same Swift SDK as tables, for lookup: every type, member, event, protocol and error. Adds nothing to `swift-api.md`. |
| [ui-requests.md](ui-requests.md) | Every browser-owned surface: request payload, completions, default presenter, modes, and the security rule that applies |
| [services.md](services.md) | The broker: call semantics, the capability list, every service command and event, the settings catalogue |
| [extensions.md](extensions.md) | Extension management, UI surfaces, installation sources, and how each extension API that needs host UI is satisfied |
| [javascript-api.md](javascript-api.md) | `Pages/` conventions, `@bdk/internal/*`, the automation host and `@bdk/automation`, DevTools customisation, code generation |
| [c-abi.md](c-abi.md) | Header layout, types, handle and ownership rules, the function families, the interface definition format |
| [cli.md](cli.md) | `bdk init`, `bdk doctor`, `bdk patch`, the build integration, exit codes, the application project layout |
| [production-browser.md](production-browser.md) | What a shippable browser needs, which part BDK supplies, which part the application writes, and a launch checklist |

Read `configuration.md` and `swift-api.md` first. `production-browser.md` is the map from "a feature a browser must have" to the API that provides it.

## 2. Status labels

Each API group, table row or key carries one of these.

| Label | Meaning |
|---|---|
| **Plan** | Required by the spec or by `architecture.md`. Only the shape shown here is new. |
| **Added** | Not in the plan before this document. Proposed because a production browser cannot ship without it, or because an existing requirement cannot be met without it. Each one is listed in section 6 and needs confirmation. |
| **Conditional** | Depends on an open decision in [../risks.md](../risks.md) or on a milestone M0 result. The dependency is named. |
| **Later** | After public v1. Listed so the v1 API leaves room for it. |

A milestone (M2 to M9) and task identifiers from [../tasks.md](../tasks.md) are given where they exist. An **Added** item has no task yet.

## 3. The surfaces

| Surface | Package | Who uses it | Authority |
|---|---|---|---|
| `bdk.toml` | Read by the CLI | Every application | Build-time and start-up configuration. Never holds secrets. |
| Swift SDK | `packages/swift`, product `BDK` | The application's native code | Full. The application is in the browser process. |
| UI requests | Part of the Swift SDK | Applications that replace a browser-owned surface | Presentation only. Security checks stay in BDK. |
| `@bdk/internal/*` | `packages/js/internal` | Packaged internal pages | The capabilities granted to that logical page in `bdk.toml` |
| `@bdk/automation`, `@bdk/commands`, `@bdk/events` | `packages/js/automation` | Application-authored scripts in the automation host, and external tools the application admits | The capability set of the session the application created |
| C ABI | `packages/core` | Language bindings. Application developers do not call it. | Same as Swift |
| `bdk` CLI | `packages/cli` | Developers and the Xcode build | None at runtime |

Swift, internal pages and automation all reach the same service commands through one broker. A setting written from a settings page is the value Swift reads. That rule is from `architecture.md` section 9 and every document here assumes it.

## 4. Conventions used by every surface

| Topic | Rule |
|---|---|
| Core abstraction | WebContents. BDK has no tab, space, group or workspace type in any surface. A page is "attached to a window" or "attached to none". |
| Threads | Every Swift call and callback is on the main actor. Every C ABI call and callback is on the UI thread unless the header marks it otherwise. |
| Asynchrony | Swift `async` functions and `AsyncSequence` events; JavaScript promises and async iterators; C callbacks with a context pointer and a cancellation handle. |
| Cancellation | Every asynchronous command can be cancelled. Cancelling a Swift task or aborting a JavaScript `AbortSignal` cancels the native work. |
| Errors | One structure everywhere: domain, code, message, and optional details. See `swift-api.md` section 16 for the domains. No exceptions cross the C ABI. |
| Identifiers | Profiles, pages, downloads, bookmarks, extensions and requests have opaque string or integer identifiers that are stable for the life of the object. Only profile, bookmark and extension identifiers are stable across launches. |
| URLs | The application and JavaScript callers receive the branded form, `<scheme>://settings`, for any page named logically. No surface returns a `chrome://` URL for such a page (task T-WUI-06). |
| Profile context | A Swift caller names the profile. An internal page always operates on its own profile. An automation session operates on the profiles the application put in its scope. |
| Names | Commands and events are `namespace.verb` in lower camel case after the dot: `history.search`, `bookmarks.createFolder`. Capabilities are `namespace.level`: `history.read`. Application-registered names start with `app.`. `bdk.toml` keys are `snake_case`. |
| Stability | The Swift and TypeScript APIs are experimental until v1 and may break in any `0.y` release. The C ABI is checked by version and hash at start. `bdk.toml` follows the BDK version; inside one major version every earlier valid file still validates. |

## 5. How the pieces fit in one application

```text
bdk.toml ──► bdk build ──► bdk.resolved.json (in the signed bundle) ──┐
Pages/   ──► bdk build ──► bundled pages     (in the signed bundle) ──┤
Automation/ ─► bdk build ─► bundled scripts  (in the signed bundle) ──┤
                                                                      ▼
AppDelegate.applicationWillFinishLaunching
    var config = try BDKConfiguration.load()        reads the resolved file
    config.pages.register(…)                        Swift page registrations
    config.commands.register(…)                     app.* commands, events, settings
    config.services.translation.register(…)         providers and keys
    config.ui.permissions.setHandler(…)             handlers for surfaces in custom mode
    let runtime = try BDKRuntime.start(config)      ABI check, scheme, Chromium; tables freeze
                                                                      │
    let profile = try await runtime.profiles.open("Personal")         │
    let page    = try profile.webContents.create()                    ▼
    window.contentView = page.view                  the application's own layout
    try await page.navigate(to: url)
```

What must be registered before `start`, because it fixes a trust boundary or a table that Chromium reads early:

| Registered before start | Why |
|---|---|
| Page registrations and `native` page handlers | The route table freezes at start (`architecture.md` 8.2) |
| Application commands, events and settings | Their names are part of the capability table that pages are checked against |
| Providers and provider keys | The Google key override happens once, at start |
| Handlers for surfaces in `custom` mode | A surface declared custom with no handler is a start error, not a silent fallback |

Everything else can change while the runtime runs: handlers for surfaces in `default` mode, event subscriptions, profiles, pages, automation sessions.

## 6. Additions that need confirmation

These are the **Added** items. None is in `tasks.md`. Each is small in API terms; several are not small to build. The right-hand column says what happens if the answer is no.

| # | Addition | Where | Why it is needed | If declined |
|---|---|---|---|---|
| S1 | Address-bar services: input classification, autocomplete suggestions, search-engine management. No default search engine is compiled in; the application names one in `bdk.toml`. | `services.md` 5.13; `configuration.md` `[search]` | An application-owned address bar has nothing to complete from without Chromium's autocomplete and search-engine services. | Applications query history and bookmarks themselves and write their own ranking and search-suggest client. |
| S2 | Stable storage identifiers: `[storage] directory` and `[storage] keychain_item`, separate from the display name | `configuration.md` `[storage]` | `architecture.md` 8.3 derives the data directory and the Keychain item from the display name. Renaming the browser would then orphan every profile and stored password. | Renaming a shipped browser needs a migration the application writes. |
| S3 | Additional browser-owned surfaces: device choosers, screen-share picker, WebAuthn, client certificates, external-protocol prompts, unresponsive-page dialog, blocked-popup notice, autofill suggestion popup, keyboard and pointer lock notice | `ui-requests.md` section 4, rows marked Added | Chromium asks for each of these through an interface that `BrowserView` normally answers. With no presenter the feature fails or the request hangs. | Each becomes "denied by default" and is listed as a compatibility gap. WebAuthn is built ahead of the rest (2026-11-22, T-UIR-09; [../amendments.md](../amendments.md) 2.19): Chromium's own dialog ended the process on a BDK page, so it could not wait. Its `[ui.surfaces]` key still waits for S3 with the others. |
| S4 | Crash reports kept locally or handed to the application; never uploaded by BDK | `configuration.md` `[diagnostics]`; `swift-api.md` 15 | The spec forbids upload to Google and says nothing about where reports go. A shipped browser needs them. | Crashes are not collected. |
| S5 | User-agent brand in client hints | `configuration.md` `[browser.user_agent]` | The runtime must not claim to be Google Chrome in `Sec-CH-UA`, and applications want their own brand there. | The brand list is "Chromium" only. |
| S6 | Extensions shipped inside the application bundle | `configuration.md` `[extensions.bundled]` | First-party extensions are a common way to ship features. They need an install path that is covered by the application's signature. | Applications install their own extensions at first run from a CRX. |
| S7 | Product defaults for browser settings | `configuration.md` `[settings.defaults]` | A privacy-focused product needs different defaults from Chromium's without patching. | Applications write the settings at first run, which races with first navigation. |
| S8 | A fourth CLI command, `bdk build`, that the Xcode build phase calls | `cli.md` section 5 | Task T-CLI-04 needs an entry point for "resolve configuration, build pages, generate declarations, sign". The spec proposes three commands and asks for the CLI to stay lean. | The build steps become shell scripts in the template with no stable interface. |
| S9 | Chromium feature overrides in prebuilt mode, with a deny list | `configuration.md` `[chromium.features]` | The spec promises "documented runtime customization" for prebuilt runtimes. Feature switches are the cheapest form of it. | Feature changes need source mode. |
| S10 | A scope on automation sessions: which profiles and which pages a session may touch | `swift-api.md` 13; `services.md` 3 | The capability set says what a session may do, not to what. Without a scope a session that may read page content may read every page in every profile. | Applications filter inside their own wrapper. |
| S11 | Window registration with anchor rectangles | `swift-api.md` 8 | Default presenters need to know where the application's address bar and toolbar are, or every prompt appears at the window's top centre. | Fixed anchoring. |
| S12 | Favicon and most-visited services | `services.md` 5.14 | History, bookmark and new-tab pages cannot be built without them. | Applications fetch and cache favicons themselves. |
| S13 | Branding beyond the name: a product logo substituted into built-in pages and default UI, defaulting to the application icon; helper display names and the alerts-helper icon written at signing | `configuration.md` 3.1.1 | The prebuilt runtime carries the Chromium logo and neutral helper names. Without this a branded browser shows another product's logo on its own settings and about pages. | Built-in pages show the Chromium logo; products that mind replace those pages. |
| S14 | A credential provider: `[services.credentials] provider = "app"` puts the application's own store behind Chromium's password manager | `configuration.md` 3.8; `swift-api.md` 11.2 | A product with its own vault or sync otherwise has to switch Chromium's password manager off and rebuild form detection and filling with scripts. | Applications mirror their store into Chromium's with `passwords.add`, or disable the surface and fill by script. |
| S15 | Page discarding surfaced to the application: a discarded state and event, `page.discard()`, a per-page exemption, and Memory Saver settings | `swift-api.md` 6.1, 6.2; `services.md` section 6 | Chromium's discarding runs in every BDK runtime because every page is a real tab. It discards hidden pages under critical memory pressure. Without this the application is not told and cannot exempt a page. | Automatic discarding is disabled in the runtime, and the application saves memory by closing and restoring pages itself. |
| S16 | A leak-check provider: `[services.leak_check] provider = "app"` lets the application answer whether a login appeared in a breach, after a sign-in and for the saved logins. `"none"` by default; no `"google"` value. | `configuration.md` 3.8; `services.md` 5.9; `swift-api.md` 11.2; `ui-requests.md` 5.8 | Chromium's leak detection is a Google service and is removed. Without a slot an application can only check saved passwords by revealing each one, with operating-system authentication per call, and cannot check at sign-in at all. | No compromised-password check. Products disclose it as a difference from Chrome. |

Scope statements this document proposes, also for confirmation:

| Topic | Proposal |
|---|---|
| Installable web apps (PWAs), `chrome.tabGroups`, reading list, side-search, Chrome's own new-tab page | Not in v1. The APIs report themselves unavailable. BDK ships a minimal default new-tab page of its own. |
| Importing bookmarks, history and passwords from other browsers | Later. HTML bookmark import and export are in v1 because they are small. |
| Enterprise policy | Not planned. `[settings.defaults]` is not a policy mechanism; users can change the values. |
| Page capabilities for driving other pages (`contents.*`) | Never grantable to an internal page. Only Swift and automation sessions can hold them. A page that needs "open this URL" calls an `app.` command. |
| Mac App Store distribution | Not supported. The browser process is not expected to run under the App Sandbox. Unverified. |

## 7. Open implementation choices

These do not need the product owner. They are recorded so the implementation agent settles each once, in the named task.

| Choice | Recommendation | Settled by |
|---|---|---|
| Payload encoding for broker commands across the C ABI | UTF-8 JSON for arguments and results, plus a separate buffer type for binary results such as screenshots and PDFs. Generated typed wrappers hide it in Swift and TypeScript. | T-ABI-03 |
| How Swift-registered commands become TypeScript declarations at build time | Commands are Swift types that conform to a protocol with `Codable` input and output. The build compiles a small declaration extractor from the application's command module and writes one JSON file that the generator reads. | T-SVC-05, T-JS-02. Settled in T-SVC-05: a command type states its input and output shape next to its `Codable` types (Swift cannot reflect fields); `BDKDeclarations.write(to:)` writes `bdk.commands.json`; the extractor is an executable target of the application that builds its `config.commands` and calls it. |
| How editors find `@bdk/*` types when generated output lives in derived data | The build writes the packages to derived data and maintains a git-ignored `node_modules/@bdk` link beside `Pages/`. | T-JS-02, T-CLI-04 |
| Swift observation | `AsyncSequence` for events on every supported OS. `@Observable` state objects only if the minimum macOS is 14; the runtime manifest example in the tooling document says 13.0. | T-SWF-02 |
| How the build phase finds the CLI | From the resolved Swift package, as a prebuilt executable in an artifact bundle, so the CLI version always equals the SDK version. A globally installed `bdk` is used only for `bdk init` and `bdk doctor`. | T-CLI-05, T-CLI-04 |
| Whether a bundled internal page may `fetch` remote `https:` data | No, by default. A page that needs remote data calls an `app.` command, so the network access is in native code. Stricter than Chromium's own pages; can be relaxed later. | T-WUI-03 |
| Where the default translation models are downloaded from | Not decided. It has a hosting cost and a privacy consequence, so it may become a product decision. | T-PRV-02 |
| Trusted Types in bundled pages | Chromium's WebUI policy sends `require-trusted-types-for 'script'` with an empty `trusted-types` list, which blocks string assignment to `innerHTML`. Several frameworks build templates that way. Keep enforcement and let the data source allow a short, fixed list of named policies that the Vite preset installs per framework. Dropping the directive is the fallback. | T-WUI-03 |
| CLI implementation | No longer open. Decided by the product owner on 2026-11-16: TypeScript, packaged with Node's own `node --build-sea` on a pinned Node 26 ([cli.md](cli.md) section 2). What remains is the verification in the task; Bun's `bun build --compile` is the fallback. | T-CLI-05 |

## 8. Unverified assumptions introduced here

In addition to the list in [../risks.md](../risks.md) section 4. Each line is something these documents assume and nobody has run.

| Assumption | Used in |
|---|---|
| Chromium's autocomplete controller and search-engine service run without a `BrowserView` and without Google endpoints once the default engine is replaced | S1 |
| A user gesture can be attested from an `NSEvent` well enough to stop accidental grants | `ui-requests.md` 3 |
| The standard AppKit edit actions reach the page through the responder chain when the page view is first responder in a non-Views window | `swift-api.md` 9 |
| A private profile maps onto a Chromium off-the-record profile, and several independent ones can exist under one parent | `swift-api.md` 5 |
| Chromium's desktop media picker, device choosers and WebAuthn dialogs can be driven from their controllers without their Views (WebAuthn: holds, 2026-11-22, through a factory patch and Chromium's sheet models; T-UIR-09) | S3 |
| Crashpad can run with upload disabled and expose completed reports to the embedding application | S4 |
| A DevTools front end is an ordinary WebContents whose view the application can place anywhere | `swift-api.md` 14 |
| The spell checker uses the operating system's on macOS and downloads no dictionary from Google | `production-browser.md` |
| The resource-access entitlements the main application needs match upstream's `chrome/app/app-entitlements.plist` | `configuration.md` 6 |
| The product logo can be substituted at start through the resource bundle's delegate, with no rebuild, and helper names and the alerts-helper icon can be rewritten before signing | S13 |
| An application store can stand behind Chromium's password manager as a `PasswordStoreBackend` without the built-in login database | S14 |
| Chromium's `LeakDetectionCheckFactory` can be replaced with one that asks the application's provider, for the sign-in check and the bulk check, with no signed-in account; and the flags it stores work without Google's check-up service | S16 |
| A BDK page handle survives a discard, whether Chromium keeps the WebContents or replaces it, and discard eligibility behaves sensibly for pages in the parking context | S15 |
| Each offered framework's Vite output runs under the internal-page content security policy, Trusted Types included | `cli.md` 4.3 |
| A Node single executable works from a Swift package artifact bundle and an Xcode run-script phase, starts fast enough to run three times per build, reads its embedded templates, and can load a project's Vite with its embedded runtime | `cli.md` 2.3 |
