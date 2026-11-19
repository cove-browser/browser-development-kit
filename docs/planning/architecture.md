# BDK architecture

Status: plan, 2026-11-14. Based on the BDK Design Specification v0.7, the planning brief, and the evidence in [feasibility.md](feasibility.md). Nothing here has been built. Class and file names from Chromium are real and refer to tag `158.0.8127.37`. Names for BDK's own types are placeholders; the implementation agent chooses the final ones.

## 1. Decisions

| # | Decision | Evidence status |
|---|---|---|
| A1 | Every hosted page lives in a real Chromium `Browser` and `TabStripModel`. BDK replaces only the `BrowserWindow` with its own implementation, modelled on upstream's `WebUIBrowserWindow`. | Partially verified |
| A2 | The public API stays WebContents-first. The `Browser` objects are internal "host contexts" that BDK creates and maintains from where the application attaches views. | Design |
| A3 | The application owns `NSApplication`, the run loop, windows and views. Chromium's browser process runs inside it. | Partially verified; Arc ships this |
| A4 | The branded internal scheme is a runtime alias over `chrome://`, not a second privileged origin. | Alias partially verified; true origin disproven as a small patch |
| A5 | One native broker holds the service commands. Swift, internal pages and the automation host are three callers with different authority. | Binding points partially verified |
| A6 | Browser-owned UI is delivered as typed requests. BDK ships a default presenter for each; the application may handle, defer to the default, or reject. | Design |
| A7 | Translation, reputation and geolocation sit behind provider contracts with non-Google defaults. The reputation default is weaker than Chrome's and is described as such. | Partially verified |
| A8 | The runtime is one prebuilt framework with neutral internal names, usable under any application identity, with a versioned C ABI. | Partially verified |
| A9 | For now `main` tracks the newest Chromium stable release through one pin, bumped by an automated roll pull request. There are no maintenance branches. Decision D8 confirmed this on 2026-11-16; Extended Stable is not planned. | Decided by the product owner |

Three findings shape everything else:

- **A WebContents outside a tab model loses almost every Chrome service.** About 177 tab-scoped features, including sessions, autofill, passwords, translate, permissions and the extensions tab helper, are created when Chromium's `TabModel` is constructed, and that code requires a browser window. History refuses to record a page that is not in a browser's tab collection. This rules out a "bare WebContents plus services" design.
- **Upstream already runs `//chrome` with a window that is not `BrowserView`.** `WebUIBrowserWindow` is selected in the same factory function as `BrowserView`, and `BrowserWindowFeatures` already branches between the two. BDK adds a third.
- **Chromium has one active tab per window.** Some services and all extensions observe that. It is the main constraint on Arc-style layouts and is handled in section 4.3.

## 2. Layers

```text
 Application (Swift, SwiftUI, AppKit; later C#/.NET, Linux host)
   │  owns: NSApplication, windows, layout, whatever page-organization model it wants, commands, sync, agent policy
   │
   ├── BDK Swift SDK                 packages/swift        idiomatic API, @MainActor
   │        │
   │   BDK C ABI                     packages/core         versioned, opaque handles, platform-neutral
   │        │
   ├────────┴───────────────────────────────────────────────────────────────────────────┐
   │ BDK engine module  (//bdk)      chromium/bdk          built by GN inside Chromium  │
   │                                                                                    │
   │  runtime & lifecycle      host contexts & window adapter      UI request router    │
   │  service broker           internal-page registry & alias      provider adapters    │
   │  extension UI adapters    automation host endpoint            platform adapters    │
   └────────┬───────────────────────────────────────────────────────────────────────────┘
            │  narrow patches                chromium/patches
   Chromium //chrome/browser   services, Browser, TabStripModel, extensions
   Chromium //components, //extensions
   Chromium //content, Blink, V8, network     sandboxed renderer, GPU, utility processes
```

Trusted JavaScript reaches the broker by two separate doors:

```text
 Internal page (packaged, chrome://bdk-<page>)        Automation host (separate process)
   @bdk/internal/*  ── Mojo, bound per page ──┐   ┌── authenticated local IPC ──  @bdk/automation
                                              ▼   ▼
                                     BDK service broker  ── same commands ──  Swift API
                                              │
                                  Chromium services, app-registered commands
```

## 3. Components

| Component | Location | Responsibility |
|---|---|---|
| C ABI and interface definitions | `packages/core` | Public headers, command and event definitions, ABI version and hash, stub runtime for tests |
| Engine module | `chromium/bdk` | Everything that links against Chromium; implements the C ABI |
| Patch series | `chromium/patches` | Hooks in upstream files; each patch states its purpose |
| Swift SDK | `packages/swift` | Swift types over the C ABI, view hosting helpers, default-presenter glue, `NSApplication` base class |
| Protocol | `packages/js/protocol` | Typed commands, events and transport shared by both JavaScript SDKs |
| Internal SDK | `packages/js/internal` | `@bdk/internal/*` for packaged pages |
| Automation SDK | `packages/js/automation` | `@bdk/automation`, `@bdk/commands`, `@bdk/events` |
| Code generators | `packages/js/codegen` | TypeScript declarations from interface definitions and application-registered commands |
| CLI | `packages/cli` | `bdk init`, `bdk patch`, `bdk doctor`; configuration validation and resolution |

## 4. Chromium adapters

### 4.1 Host contexts

A host context is BDK's internal wrapper around one Chromium `Browser`. The application never sees it by default.

| Application action | What BDK does internally |
|---|---|
| Creates a WebContents in a profile | Creates the tab in that profile's parking context: a `Browser` with no native window, excluded from session restore |
| Attaches the page's view into a view in window W | Moves the tab into W's host context with `DetachTabAtForInsertion` and `InsertDetachedTabAt`, creating the context on first use |
| Moves the view to another window | Same move between contexts; the tab and its services survive |
| Removes the view from all windows | The tab stays in its last context as a background tab, or moves to the parking context if that window closes |
| Focuses a page | Activates that tab in its context and marks the context most recently active |
| Destroys the WebContents | Closes the tab |

Moves always go through the tab model. BDK never reparents a WebContents behind Chromium's back, because that is what keeps extension tab identifiers, `activeTab` grants and tab services intact.

Windows that Chromium creates itself, such as undocked DevTools, popups from `window.open` and picture-in-picture, arrive through the same factory. BDK turns each into a "new window requested" UI request (section 7) carrying the type and the WebContents, so the application decides where it appears.

BDK has no space, workspace, group or tab type, in the public API or in configuration. How an application organizes pages is its own data model, and BDK sees only "this page's view is attached to that window, or to none".

**Provisionally decided (D1, option (b)), to be confirmed by the prototype:** what extensions observe for pages that are not on screen. With the table above, a page whose view is detached stays a background tab of the window it was last in, so `tabs.query` lists it there. Some applications will want off-screen pages to look like they belong to a different window instead. BDK adds one optional, generic call, unless the prototype shows no extension can tell the difference: "keep this page in host context C", where C is an opaque handle the application creates and BDK attaches no meaning to. An application that never calls it gets the per-window default. See decision D1 in [risks.md](risks.md).

### 4.2 The BDK window

`BdkBrowserWindow` implements Chromium's `BrowserWindow` interface, which has 72 pure virtual methods. Most become no-ops. The ones that carry services are:

| `BrowserWindow` responsibility | BDK implementation |
|---|---|
| Window identity, bounds, activation (`ui::BaseWindow`) | Platform adapter over the application's native window |
| Modal-dialog host for the window and for a tab | Positions dialogs over the page's host view; needed for print preview and tab-modal dialogs |
| Find bar | Emits find UI requests |
| Exclusive-access context | Emits fullscreen and pointer-lock requests; reports the application's response |
| Location bar | A stub that supplies anchor rectangles, as `WebUIStubLocationBar` does upstream |
| Autofill bubble handler, translate bubble, download UI entry points | Emit UI requests |
| `AsBrowserView()` | Returns null |

Patches needed for this, all small (as built; see [amendments.md](amendments.md) section 2.13):

- A settable factory that `BrowserWindow::CreateBrowserWindow` asks first. `BrowserWindow::FromBrowser` needs nothing.
- No `BrowserWindowFeatures` arm: it already tolerates a window with no `BrowserView`. A window must call `TearDownPreBrowserWindowDestruction()` before it goes, and supply a location bar, find bar, exclusive-access context and modal-dialog host.
- A guard where upstream accepts a `Browser` without a side panel UI for Webium only (read-anything), and an exception for BDK's private profiles in `OTRProfileID::AllowsBrowserWindows`. More guards are expected as the services of M3 and M5 reach `BrowserView` casts.
- A context belongs to one profile, so a window showing pages of two profiles has two contexts.

BDK's window-scoped glue goes in upstream's `EmbedderBrowserWindowFeatures` hook, which exists for embedders.

### 4.3 Active, focused and visible

Chromium distinguishes three states per tab, and BDK maps application behaviour onto them:

| Chromium state | BDK source |
|---|---|
| Visible | Follows the native view automatically when it is shown, hidden or moved |
| Active (one per context) | The page that last had focus in that window |
| Last-active context | The window that last had focus |

This is correct for one or two visible pages. A two-page split is represented as an upstream split tab, so Chromium treats both as foreground.

**Architectural issue for Arc-style applications, including Cove.** With three or more visible pages in one window, the pages that are neither active nor split peers are background tabs to several services. Translate UI is suppressed for them, the find bar does not follow them, and JavaScript dialogs from them are not treated as foremost. Extensions see one active tab per window regardless.

Handling, in order:

1. Ship the one- and two-pane mapping first, because it needs no extra patch.
2. Add a BDK predicate, "this tab is presented", and use it at the call sites that currently compare against the active tab. Known sites: translate bubble gating, the find bar controller, and JavaScript dialog foregrounding. The full list is unverified.
3. Fallback: back each visible pane with its own context. Every pane is then active. Extensions see more windows, which is a compatibility trade-off that must be documented if taken.

Extension semantics do not change under option 2: `tabs.query({active: true, currentWindow: true})` returns the focused pane. That is the behaviour to document.

### 4.4 Sessions

Chromium's session service records windows and tabs as `Browser` objects are created. The application owns session restoration, so BDK:

- Stops Chromium from restoring windows on its own at startup.
- Exposes each page's serialised navigation state for the application to store and restore.
- Keeps Chromium's per-tab session identifiers, because the extension tab identifier is derived from them.

## 5. Shared core and C ABI

The ABI is the only boundary the Swift SDK, and later the .NET and Linux bindings, depend on.

| Rule | Reason |
|---|---|
| Plain C. No Objective-C, C++ or platform types in signatures | Swift, C# and other bindings consume it unchanged |
| Opaque handles that are generation-checked identifiers, not pointers | A stale handle fails cleanly |
| Every struct begins with its size | Structs can grow by appending fields |
| Explicit ownership: each returned object has one documented release call | No guessing across languages |
| Asynchronous calls take a callback, a context pointer and return a cancellation handle | Maps to Swift `async`, C# `Task` |
| All calls and callbacks happen on the UI thread unless marked otherwise | Matches Chromium's UI sequence; Swift marks them `@MainActor` |
| Errors are a structure: domain, code, message | No exceptions across the boundary |
| Strings are UTF-8 with explicit length | No ambiguity about termination or encoding |
| Native views cross as a tagged platform handle | The one place platform types appear, and only as an opaque value |
| `bdk_abi_version()` and `bdk_abi_hash()` are checked at start | A mismatch fails before Chromium initialises |

CEF's C API is the precedent for the size-prefixed structures and the version-and-hash check.

Two layers sit on the ABI:

- **Host functions**: runtime start and stop, profiles, WebContents lifecycle and navigation, view attachment, UI request delegates, event subscription.
- **Broker commands**: typed service operations such as history search, bookmark edits, settings reads and writes, extension management, translation control, and application-registered commands. These are defined once in `packages/core/interface` and invoked through one entry point.

The Swift SDK wraps both. Putting services behind broker commands is what lets Swift, internal pages and automation share one implementation.

The stub runtime in `packages/core` implements the ABI in memory with no Chromium. SDK unit tests and hosted CI use it. A conformance suite runs against both the stub and the real runtime so they cannot drift.

## 6. macOS host

### 6.1 Process and event loop

The application's executable links the runtime framework directly. Its `NSApplication` principal class is, or subclasses, a small Objective-C class shipped by BDK that conforms to Chromium's application protocols and forwards `sendEvent:` through Chromium's handling. SwiftUI applications set it through `NSPrincipalClass`.

Start-up sequence:

1. The application starts normally and owns the run loop.
2. From `applicationWillFinishLaunching`, it calls the runtime start function with the resolved configuration.
3. BDK checks the ABI, registers the alias scheme, initialises Chromium's main runner and holds a keep-alive.
4. Chromium's message pump detects the running application and nests instead of taking over.

Patches in `//chrome` for this: accept an application object that conforms to the protocol and is not Chrome's subclass; skip Chrome's application delegate and main menu; route termination through BDK; create no startup window.

### 6.2 View hosting

Each WebContents exposes a native view. On macOS that is an `NSView` that BDK wraps in a host view the application places anywhere, including inside `NSViewRepresentable`.

| Concern | Who handles it |
|---|---|
| Visibility when hidden, shown or moved | Chromium, automatically |
| Unhandled keyboard events | BDK reports them; the application decides. BDK also offers Chromium's redispatch behaviour for windows that adopt it |
| Accessibility parent | BDK sets it on attach |
| Popups, `<select>` menus, drag and drop | Chromium |
| Context menus, fullscreen | UI requests |
| Reparenting with a popup or fullscreen active | BDK closes transient UI first |

### 6.3 Subprocesses

Renderer, GPU and utility processes are Chromium's own helper applications inside the framework, sandboxed as upstream. BDK does not weaken the sandbox, site isolation or process locks.

## 7. Browser-owned UI

Every browser-owned surface follows one pattern:

1. Chromium asks for UI through an interface BDK implements.
2. BDK builds a typed request and offers it to the application's handler for that surface, if one is registered.
3. The handler returns one of three results:

| Result | Effect |
|---|---|
| Handled | The application presents it and completes the request through the handle it was given |
| Use default | BDK's default presenter shows it |
| Reject | The request is denied or cancelled, where that is safe |

With no handler, the default presenter is used. Configuration can set a surface to `default`, `custom` or `disabled`.

Security rules that override configuration:

- A security interstitial cannot be disabled or auto-dismissed.
- A permission, extension-install or credential prompt cannot be resolved as "granted" without a user gesture delivered through the request handle.
- A custom presenter receives the same origin, risk and permission text the default would show.
- `disabled` means the feature is unavailable and requests are denied. It never means silently allowed.

Surfaces: permission prompts, JavaScript dialogs, HTTP authentication, file pickers, context menus, new-window requests, downloads, autofill and password prompts, printing and print preview, translation, find in page, fullscreen, notifications, certificate and Safe Browsing interstitials, DevTools, and the extension surfaces in section 10.

### Default presenters

Preferred implementation: reuse Chromium's existing dialog and bubble views, anchored to rectangles the BDK window supplies. Upstream's Webium work is making those views run without `BrowserView`, and they are cross-platform, so the same defaults would serve Windows and Linux.

This is unverified on a native window that is not a Views widget. If it does not hold, the defaults become small platform-native presenters written per platform. The request model above does not change either way. Milestone M0 tests it.

## 8. Internal pages

### 8.1 Logical routes

`bdk.toml` names pages logically. It never contains `chrome://`. Each logical page resolves to one mode:

| Mode | Meaning |
|---|---|
| `chromium` | The built-in page |
| Bundled path | A packaged HTML and TypeScript page from the application bundle |
| `native` | A request delivered to a Swift handler |
| `disabled` | Navigation is blocked with a standard error page; no redirect |

Settings and extension management follow the same rules as every other page. Overriding or disabling one leaves no alternate route to the built-in version.

### 8.2 Resolution rules

Precedence, highest first: Swift registration, explicit TOML override, auto-discovered page under `Pages/`, built-in default.

| Rule | Detail |
|---|---|
| Name normalisation | Directory names are split on case boundaries and lower-cased with hyphens: `NewTab` becomes `new-tab`. Valid names match `[a-z][a-z0-9-]*`. |
| Collisions | Two directories that normalise to the same name are a build error. So is an alias with the same name as a page. |
| Aliases | An alias points at a page, never at another alias. It shares the page's identity and capabilities. |
| Reserved names | A fixed list of names Chromium needs internally cannot be used by applications. |
| Freezing | The route table is fixed when the runtime starts. Swift registrations must be made before start. Nothing can change a page's mode, and so its trust level, afterwards. |
| Unknown route | Blocked with an error page. |

The build writes the resolved table, with the capability grants, into the application bundle as `bdk.resolved.json`. It is covered by the application's code signature. The engine reads that file and nothing else from disk. Swift registrations are applied on top of it, in memory, before the table freezes.

#### What a Swift registration looks like

A Swift registration sets one route's mode from application code, before the runtime starts. Type and method names are placeholders.

```swift
// In applicationWillFinishLaunching, before the runtime starts.
var config = try BDKConfiguration.load()        // reads bdk.resolved.json from the bundle

// Serve Chromium's built-in history page, even though Pages/History exists.
config.pages.register("history", .chromium)

// Deliver <scheme>://downloads to Swift. No page is loaded.
config.pages.register("downloads", .native { request in
    DownloadsWindow.shared.show(path: request.path)
    return .handled
})

if !license.allowsExtensions {
    config.pages.register("extensions", .disabled)
}

try BDKRuntime.start(config)                    // the route table freezes here
```

Registering a route as `.chromium` is the Swift form of `history = "chromium"` under `[pages.overrides]`. It passes no code. It only tells the resolver which implementation serves the route, and it outranks a TOML override and an auto-discovered directory. Use TOML when the choice is fixed. Use Swift for two cases TOML cannot express: a choice known only at launch (licence, feature flag, debug build), and `native` mode, which needs a handler.

Limits:

- A registration after start is an error, never a silent substitution.
- A Swift registration can point a route at a bundled page only if that page is in the signed bundle and already has an entry in the resolved file. Capabilities for bundled pages still come from `bdk.toml`.
- A Chromium page registered this way keeps exactly the interfaces Chromium gives it, as in section 9.2.

### 8.3 Branded scheme

Nothing about the browser's identity is compiled into the runtime. The name and scheme are read from each application's `bdk.toml` when the runtime starts:

| Value | Source | Used for |
|---|---|---|
| Display name | `[browser] name` | Product strings in default UI, the Keychain item name, the user-data directory |
| Internal scheme | `[browser] internal_scheme` if set; otherwise derived from the name by lower-casing it and dropping characters a URL scheme cannot contain | Branded internal URLs |

In this document `<scheme>` stands for that configured value. An application whose `bdk.toml` says `name = "Cove"` gets `cove://settings`; one that says `name = "Tide"` gets `tide://settings` from the same prebuilt runtime. An explicit `internal_scheme` is recommended, because a derived scheme changes if the display name does, and existing links would break. A scheme that is empty after derivation, reserved, or already registered fails configuration validation.

The runtime handles the scheme as an alias:

- At start, the name is validated against reserved and existing schemes and registered in every process as a standard, secure scheme with no WebUI, CORS or CSP privileges.
- A navigation to `<scheme>://settings` is rewritten by a URL handler to the internal URL: `chrome://settings` for a built-in page, `chrome://bdk-settings` for a bundled one.
- BDK reports the branded form to the application for display, history and session data.
- A page from the web cannot navigate to either form, exactly as with `chrome://` today.

Consequences the application developer will notice: inside a bundled page, `location.origin` is `chrome://bdk-<page>`, and DevTools shows the same. Making the page commit as `<scheme>://…` would need changes across Chromium's WebUI security checks and compiled-in metadata, and is not planned. This departs from the literal reading of the spec and was accepted as decision D3 in [risks.md](risks.md).

### 8.4 Bundled pages

Each bundled page is served by a BDK data source from the signed application bundle, under Chromium's standard WebUI content security policy: no remote scripts, no inline script, no `eval`. The Vite preset supplied by BDK produces compliant output. A page that needs no privileged access gets none; it is still a WebUI page, isolated from the web.

## 9. Service broker and hybrid JavaScript

### 9.1 One broker, three callers

| Caller | How it connects | Authority |
|---|---|---|
| Swift application | C ABI, in process | Full |
| Internal page | Mojo interface bound to that page | The capabilities granted to its logical page in `bdk.toml` |
| Automation host | Authenticated local channel | The capability set the application gave that session |

Commands are defined once, each with a capability name such as `settings.read` or `extensions.manage`. Application-registered commands use an `app.` prefix. The broker checks the caller's capability on every call and supports cancellation and progress.

### 9.2 Internal pages

Authority comes from page identity, checked natively:

1. Each privileged page has its own controller type and internal host.
2. The broker interface is bound only through Chromium's per-controller binder, which rejects subframes and other controller types and reports the renderer.
3. At bind time BDK also checks the committed origin, that the frame is the primary main frame, that the process has WebUI bindings and the expected process lock, and the profile.
4. Every call re-checks the committed origin. Bindings are dropped on navigation and teardown.
5. Capabilities come only from the resolved configuration, never from the page.

Importing `@bdk/internal/*`, knowing a route, or creating a directory grants nothing. An alias carries its target's capabilities. Remote content never does.

Built-in Chromium pages keep the interfaces Chromium gives them. BDK capabilities are additional and separate.

### 9.3 Automation host

A dedicated host runs application-authored scripts, independent of any visible page. It exposes WebContents operations, DOM and script execution in permitted worlds, DevTools protocol access, browser-service commands, registered commands and events.

Recommended shape, to be confirmed by a spike in milestone M5:

- A sandboxed utility process in the runtime runs bundled scripts on V8 and reaches the broker over Mojo.
- For external tools, BDK can open a local socket that is off by default, restricted to the user, and authenticated with a token the application issues. The DevTools protocol is offered over the same authenticated channel, never on a network port.

What BDK does not provide in v1: agent identity, consent UI, per-agent policy, a CLI, or MCP or ACP gateways. What it does provide for applications that build those: sessions with an explicit capability set chosen by the application, enforced by the same broker check used for pages; session revocation; and events for auditing.

The internal-page path and the automation path cannot call each other. They meet only at the broker.

## 10. Extensions

Extensions run in Chromium's extension system unchanged: manifest handling, service workers, content scripts, messaging, storage and permissions need no BDK work. Because every page is in a real tab model, the tabs and windows APIs see real tabs and windows.

BDK supplies the UI side by implementing interfaces Chromium already defines:

| Surface | Chromium interface | BDK work |
|---|---|---|
| Action state and click | `ExtensionActionRunner` is window-agnostic | Expose state and activation; target the focused page |
| Action popup | `ExtensionViewHost` with a pluggable `ExtensionView` | A native view implementation; a container implementing `ExtensionsContainer` so `action.openPopup` works |
| Side panel | Created only with `BrowserView` today | A patch in `BrowserWindowFeatures` and a BDK side-panel implementation |
| Context menus | Menu model | Deliver the model's items as a UI request |
| Options page | Opens through the window controller | A new-window or new-page request |
| DevTools extensions | DevTools window | Undocked first |

Each follows the request pattern in section 7. The default presenter for an application with no toolbar is an extensions menu attached to the host window, from which every action is reachable.

Installation:

| Source | Default | Notes |
|---|---|---|
| Unpacked directory | On | Works in any build without Google branding |
| Local CRX | On | Chromium denies off-store installs by default; BDK owns this policy and keeps the install prompt |
| Self-hosted update URL | On | Assured |
| Chrome Web Store | A named source, off unless the application enables it (decision D4). When enabled, the store's install button is relabelled "Add to <browser name>". | Works technically; not assured by Google's terms |

Compatibility is published as a versioned matrix generated from the test suite. Before installation BDK inspects the manifest and rejects known incompatibilities with a reason. Disabled surfaces make the corresponding API report itself as unavailable. Unsupported calls fail explicitly.

## 11. Providers

Each service has a contract that the default UI, the Swift API and `@bdk/internal/*` all use. Changing provider changes none of them.

| Service | Contract covers | Default | Optional, off by default |
|---|---|---|---|
| Translation | Language detection, supported pairs, batched text translation with cancellation, progress, errors, model download, a privacy descriptor | Local Bergamot engine with Mozilla models; Chromium's open language detector | Apple Translation, DeepL, Microsoft, Google |
| Reputation | URL check, download check, list updates, verdict types that map to Chromium's interstitials | Signed static block lists built from open feeds and served as files by BDK (decision D2); "reduced protection" | Google Web Risk with the application's own key |
| Geolocation | Position requests, permission, accuracy, errors | Operating-system provider | beaconDB, commercial services, Google |

Translation needs a BDK renderer agent, because Chromium's own contract is "inject Google's script". The agent extracts text, calls the provider through the browser process, and applies results. Chromium's translate state, preferences and language detection are reused.

Google-backed providers require the application's own key and acceptance of Google's terms. BDK ships no Google key, because the terms forbid sharing one across legal entities.

### 11.1 Choosing a provider, and Google keys

An application picks a provider per service in one of three ways. All three sit behind the same contract, so the default UI, Swift API and `@bdk/internal/*` do not change.

| Way | Where | What it does |
|---|---|---|
| Named provider with the application's key | `bdk.toml`, key supplied at start | Uses a provider BDK ships: `google`, `deepl`, `microsoft`, `web-risk` and so on |
| Endpoint remap | `bdk.toml` | Keeps the provider's protocol and sends it to a host the application names, such as its own proxy. The key can then stay on the server. Brave does this for Safe Browsing. |
| Application provider | Swift, `provider = "app"` | The application implements the contract and calls any API it likes |

```toml
[services.geolocation]
provider = "google"                         # Chromium's own network provider, with the app's key

[services.reputation]
provider = "google"
endpoint = "https://sb.example.com"         # optional: same protocol, the application's proxy

[services.translation]
provider = "app"                            # Swift implements the translation contract
```

```swift
config.services.google.apiKey = Secrets.googleAPIKey      // not written in bdk.toml
config.services.translation.register(MyTranslationProvider())
```

How the key reaches Chromium: `google_apis` has a GN argument, `support_external_google_api_key`, that compiles in `InitializeAndOverrideAPIKey()`. Upstream turns it on for iOS and Fuchsia. BDK turns it on for its runtime and calls it at start when the application supplied a key. One prebuilt runtime therefore serves applications with and without a key. Read at the pinned tag in `google_apis/google_api_keys.h` and `google_apis/BUILD.gn`; not yet built on macOS.

A key alone is not enough for every service, which is why the mapping layer exists:

| Service | With the application's Google key | Mapping needed |
|---|---|---|
| Geolocation | Chromium's network provider works as in Chrome. Google bills the key's owner. | None |
| Reputation | Chromium's Safe Browsing client works, but Google's Safe Browsing API terms allow non-commercial use only | A commercial application maps the contract to Web Risk, which is a different API |
| Translation | Chromium's built-in path calls Google's translate element endpoint, which Google does not offer to third-party keys as far as this plan has established | Map the contract to Cloud Translation, or to another vendor |

Rules:

- The key belongs to the application's legal entity. BDK never ships, proxies or defaults one.
- A key shipped inside an application can be extracted, as Chrome's can. Restrict it by API and quota in Google's console, or use the endpoint remap with a proxy.
- The key is supplied from Swift or from a build setting. `bdk.toml` holds the provider choice, not the secret.
- Invariant 9 in section 16 is unchanged: requests go to Google only for the services the application configured that way. The network audit runs once with no Google provider and once per configured one.

**Reputation is not equivalent to Chrome.** Open threat feeds list known threats. They do not give real-time checks, download reputation or machine-learned phishing detection. BDK's documentation and any default UI text must say "reduced protection" for the default provider. TLS validation, certificate errors, interstitials, site isolation and the sandbox are unaffected.

Security data that Chromium receives through its component updater must keep arriving: revocation lists, the Certificate Transparency log list and root-store updates. The update URL is overridable and components verify against pinned keys, so a BDK or application mirror can serve Google-signed packages unmodified. Whether the signed update-check protocol allows that is unverified and is task T-PRV-06.

## 12. Privacy

Excluded from the runtime: Google account sign-in, Chrome Sync, any built-in sync, metrics and crash upload to Google, and variations fetches from Google.

How this is enforced:

- Build-time: features removed or disabled in GN arguments and patches, tracked in an audit list.
- Run-time: no Google API key; Google endpoints for utility services unreachable unless the application configured that provider.
- Test: a network audit runs a scripted session with Chromium's network log enabled and fails on any request to a Google-owned host that the script did not visit. It is a release gate.

Applications can build their own sync or telemetry on BDK's state APIs. That is outside the runtime.

## 13. Runtime packaging and versions

```text
App.app/Contents/
  MacOS/App
  Resources/bdk.resolved.json, Pages/…
  Frameworks/BDKRuntime.framework/Versions/A/
      BDKRuntime            Chromium plus the BDK engine module
      Resources/            paks, ICU, locales, manifest.json
      Libraries/
      Helpers/              BDK Helper.app, (Renderer), (GPU), (Alerts), crash handler
```

- Framework and helper names are neutral and fixed at build time. The engine version is in the manifest, not the path.
- Identity comes from the application: bundle identifier, and so Mach and sandbox names; Keychain item name; data directory.
- A BDK signing script, run as a build phase, rewrites helper bundle identifiers and re-signs inside-out with the application's identity: libraries, helpers, framework, then the application. Renderer and GPU helpers get the JIT entitlement only.
- Distribution is a SwiftPM binary target pointing at a release asset, with a checksum.
- No updater ships. BDK never updates itself. The application chooses a BDK version and ships its own updates.
- Before the first launch of a newer engine milestone, BDK snapshots the profile. Starting an older engine on a newer profile without a snapshot is refused or uses a fresh profile, according to application policy.

Compatibility contract between an application and a runtime: ABI version and hash, manifest digest for the BDK release, and the build attestation once runtimes are built in CI. Source-built and prebuilt runtimes for the same inputs must report the same ABI hash.

## 14. Windows and Linux boundaries

Shared, platform-neutral: the C ABI, the broker, host contexts, the page registry, provider contracts, the UI request model and the extension adapters.

Per platform, behind adapter interfaces:

| Adapter | macOS now | Windows, Linux later |
|---|---|---|
| Application and event-loop integration | `NSApplication` protocols | Message loop, GLib |
| Native view host | `NSView` | `HWND`; X11 or Wayland surface |
| Window adapter for `BdkBrowserWindow` | `NSWindow` | Platform window |
| Default presenters, if Views reuse fails | AppKit | WinUI or Win32; GTK or Qt |
| Credential store | Keychain | DPAPI; Secret Service |
| Default-browser registration, file pickers, notifications, accessibility | AppKit, Launch Services | Platform APIs |
| Packaging and signing | Framework, helpers, notarization | Installer and Authenticode; packages |

Guardrails from the first milestone:

- No file in the shared part of `chromium/bdk` includes a platform header. Platform code is in suffixed files.
- The ABI headers compile as C on Linux in CI, and the conformance suite runs against the stub runtime there.
- Each ABI change is reviewed against a short Windows and Linux checklist.

## 15. Lifecycle and failure

| Event | Behaviour |
|---|---|
| Runtime start | Fails with a structured error on ABI mismatch, invalid configuration, missing or tampered runtime. No partial start. |
| Renderer crash | An event on the WebContents; the application reloads or replaces it. Other pages are unaffected. |
| GPU process crash | Chromium restarts it; BDK reports it. |
| Automation host crash | Sessions end with an error; the application may restart the host. |
| Window closed with attached pages | Pages move to the parking context unless the application destroys them. |
| Application termination | The application asks BDK to shut down; BDK runs unload handlers, closes contexts, stops Chromium, then replies. |
| Profile deletion | Refused while WebContents exist in it. |

## 16. Security invariants

1. Chromium's sandbox, site isolation and process locks are unchanged.
2. Ordinary websites receive no BDK interface.
3. Internal-page privilege is bound to native page identity and re-checked per call.
4. The branded alias scheme has no privilege of its own.
5. Extension contexts never receive `@bdk/internal/*`.
6. Security prompts and interstitials cannot be bypassed by configuration or by a custom presenter.
7. Automation endpoints are local, authenticated and off by default.
8. The resolved configuration and bundled pages are covered by the application's signature.
9. No Google key ships; Google-directed requests happen only for providers the application configured.
10. The runtime never updates itself.

## 17. Differences from the spec and brief

| Topic | Spec or brief | This plan | Why |
|---|---|---|---|
| Internal scheme | Pages are reached at `<scheme>://settings` | Reached and displayed at `<scheme>://settings`; committed origin is `chrome://…` | A second privileged origin is not a small or safe patch (feasibility 4) |
| WebContents-first API | "WebContents should be the core content abstraction, not a BDK Tab"; no prescribed tab, space, window or workspace model | Kept. Internally every page is a Chromium tab. At most, one optional and generic host-context call is added for off-screen pages (section 4.1) | Services and extensions require the tab model (feasibility 1 and 3) |
| Reputation default | `provider = "default"` retains protection with Google off | A default exists but is weaker than Chrome, and its form needs a decision | No open equivalent exists (feasibility 6) |
| Extension stores | `compatible_stores = true` by default | Unpacked, CRX and self-hosted on by default; Chrome Web Store is a named source awaiting a decision | Store terms do not cover other browsers |
| Off-store CRX | Supported by default | BDK must replace Chromium's default-deny policy for off-store installs | `extension_management.cc:388-402` |
| Build tool | GN and Ninja | GN and `autoninja`, which runs Siso by default | Upstream changed |
| Translation | Reuse Chromium's translation machinery | State, preferences and detection are reused; the page-translation step is replaced | Upstream's step is Google's script |
| Configuration parsing | `bdk.toml` read at build and run time | Parsed once by the CLI; the engine reads a resolved file | One parser and validator |
| Split view | Arbitrary layouts | Arbitrary layout for hosting; service foregrounding beyond two panes needs a patch or a per-pane fallback | Feasibility 10 |
