# chromium/bdk

The `//bdk` overlay: BDK's engine, built by GN inside the Chromium checkout and linked into the Mac
framework. Chromium style (`.clang-format`, `BasedOnStyle: Chromium`).

`chromium:build` copies this directory to `<checkout>/src/bdk` under the checkout lock, writing only
files whose content changed (so only they rebuild), deleting files removed here, and excluding `bdk/`
from `git status` in `src`. It also copies, under `//bdk/core`, the runtime core
(`packages/core/runtime`), the C ABI headers (`packages/core/include/bdk`) and the generated ABI hash.
A copy rather than a symlink: the managed checkout is shared by every worktree, and a link out of the
source root would follow whichever worktree last changed it (and dangle when that worktree is removed).
The overlay therefore reflects the worktree that is building.

## Targets

| Target | What |
|---|---|
| `//bdk:core` | The runtime core shared with the stub: every C ABI function's checks, handles, calls, subscriptions, broker and UI requests. Built as BDK's portable C++ (`no_chromium_code`, no Chromium clang plugins); its own lint is in `packages/core`. |
| `//bdk:bdk` | The engine: the Chromium backend of the core, start-up inside the application, and the marker. |

## The engine (`engine/`)

| File | Role |
|---|---|
| `chromium_main.h`, `chromium_main_mac.mm` | Starts the `//chrome` browser process inside an application that already owns `NSApplication` (architecture.md section 6.1). `ChromeMainDelegate` is subclassed so the browser is initialised and handed back instead of running its loop; the loop then runs nested inside the application's own, from a run-loop block, until the runtime shuts down. Adds `--bdk-hosted`, `--no-startup-window`, `--no-first-run`, `--no-default-browser-check`. |
| `application_mac.mm` | `BDKApplicationBase`, the class the Swift SDK's `BDKApplication` subclasses (declared for applications in `packages/core/include/bdk_mac`). It subclasses Chromium's `BrowserCrApplication`, so events, accessibility activation and the message pump behave as in Chrome; `terminate:` is `NSApplication`'s. |
| `engine_backend.cc` | `bdkcore::Backend` over Chromium: start and stop, the profile registry (`BDK Profiles.json` in the user data directory; profile `profile-N` lives in directory `profile-N`), persistent profiles through `ProfileManager` (kept loaded from their first open until the runtime stops: an unload runs in the background, and reopening meanwhile would put two instances on one profile's databases), private profiles as separate off-the-record profiles of the base profile, and every page operation. |
| `engine_contents.{h,cc}` | One page: its `WebContents` (a tab of a host context, owned by the tab model), its observer (events) and its delegate (new-window, fullscreen and context-menu UI requests, keyboard routing, `window.close()`). BDK's delegate serves the page in every context: a `Browser` takes the delegate when it takes a tab, so the engine sets it again after each insertion, move and replacement. Forwarding Chrome's own delegate (`BrowserWebContentsDelegate`) for a service is Phase 2 work, call by call. |
| `host_context.{h,cc}` | Host contexts (architecture.md section 4.1): `HostContext` wraps one `Browser` and its tab model, `HostContexts` owns them. A profile's **parking context** has no native window; a registered window has one context per profile whose pages it shows (a `Browser` belongs to one profile). Pages enter a context as background tabs and move only through the tab model (`DetachTabAtForInsertion`, `InsertDetachedTabAt`), so the tab, its session identifier (the extension tab id) and its tab helpers survive. A context is created on first use, never shown, omitted from session restore, and destroyed with its window (its pages go to parking first), its profile or the runtime. |
| `bdk_browser_window.{h,cc}` | `BdkBrowserWindow`, the `BrowserWindow` of a context, modelled on `WebUIBrowserWindow`: `AsBrowserView()` is null, most calls do nothing, a call a service may need logs once (`NOTIMPLEMENTED_LOG_ONCE`). Identity, bounds, activation and fullscreen state come from the registered native window; `Show`, `Hide`, `Close` and `SetBounds` do nothing, because the application owns its windows and a context outlives its tabs. Real objects, not nulls, where Chromium dereferences without a check: a location bar (`Browser::OnTabDeactivated` saves state into it), a find bar, an exclusive-access context and a modal-dialog host, each a stub for now. |
| `host_window.h`, `host_window_mac.mm` | The platform half of a registered window: the `NSWindow` as `gfx::NativeWindow`, its frame in screen coordinates, key/visible/minimised/zoomed/fullscreen state and key-window notifications (which drive `BrowserActiveStateManager`). The only window code with AppKit types. |
| `host_view.h`, `host_view_mac.mm` | The native view the application places. It holds the page's view, so the page can be replaced (restore, discard) without the application noticing; reports when it enters or leaves a window (`viewDidMoveToWindow`), which moves the page's tab to the registered window's context (later, from a task, never inside the application's own call); the default context menu and fullscreen presenters. Engine files with AppKit types: this, `host_window_mac.mm` and start-up. |
| `session_state.{h,cc}` | Page history for the application to store: a BDK envelope (`BDKC`, version 1) around Chromium's session-restore records. |
| `identity.{h,cc}` | The application's identity from `bdk.resolved.json`: data directory, Keychain item, product name (T-PKG-02). |
| `devtools_frontend.{h,cc}` | Undocked DevTools in a BDK page: Chrome's bundled front end and `DevToolsUIBindings`, attached to the inspected page. Opened from the context menu's Inspect item (absent when `[ui.surfaces] devtools = "disabled"`); arrives as a new-window request of kind `devTools`. |
| `content_browser_client.{h,cc}` | `BdkContentBrowserClient`, Chrome's browser client plus BDK's additions (the delegate supplies it through a patch). Today: the scripted-navigation interceptor. |
| `bdk_testing.h`, `testing.{h,cc}` | Test hooks, exported but not ABI: scripted navigations served without the network, UI requests for the application's own handler, script with a user gesture, synthetic clicks, a run-loop slice that lets Chromium work from inside a task, a pending-work count, the count of Chrome windows that are not host contexts and of host contexts, and where a page's tab is (its context, the tab's session identifier, whether it is active). Used by the engine conformance app (`packages/core`, `nx run core:test-engine`). None gives a page or a session anything. |

Test and diagnostic switches, read from the environment of the application: `BDK_ENGINE_MOCK_KEYCHAIN=1`
(Chromium's mock Keychain, so tests never prompt), `BDK_ENGINE_SWITCHES` (more Chromium switches, space
separated, no dashes), and `BDK_ENGINE_KEEP_CHROMIUM=1` (a runtime shutdown leaves Chromium running, so
one process can start several runtimes; Chromium itself starts once per process).

## Host contexts

Every page is a tab of a `Browser`, so Chrome's tab helpers and tab-scoped services attach (T-ADP-01 to
T-ADP-03; the C ABI did not change).

| Application action | What the engine does |
|---|---|
| Creates a page | Inserts its `WebContents` as a background tab of the profile's parking context |
| Adds the page's view to a view in registered window W | After the view reports it, moves the tab to W's context for the page's profile, creating the context on first use. A view in a window that was never registered changes nothing |
| Moves the view to another registered window | The same move; tab id, history and renderer survive |
| Removes the view from every window | Nothing: the page stays a background tab of its last context |
| Unregisters W (`bdk_window_unregister`) | Moves W's pages to parking, then closes W's contexts |
| Focuses a page (`bdk_contents_focus`, or the page taking focus) | Makes its tab the active tab of its context; a window's context becomes the most recently active one |
| Destroys the page | Closes its tab (`DetachAndDeleteWebContentsAt`) |

Restoring or discarding a page replaces its `WebContents` inside the same tab (`DiscardWebContents`), so the
context is kept; the tab's session identifier changes then, as in Chrome.

`chromium_main_mac.mm` keeps the `APP_CONTROLLER` keep-alive, so closing the last context never starts a
shutdown. `bdk_testing_chrome_window_count()` ignores host contexts; anything it counts is a window Chromium
opened itself.

What Chromium requires of a window that is not a `BrowserView` (T-ADP-02) is in
[amendments.md](../../docs/planning/amendments.md) section 2.13.

## The patches it needs

`chromium/patches` links `//bdk` into the framework, exports the C ABI, `BDKApplicationBase` and the test hooks
(`chromium:build` fails when the framework does not export every function the headers declare), lets the
engine keep the content runner after start-up, leaves `NSApplication`'s delegate and menu to the
application, lets the application name its Keychain item, lets the engine supply its own
`ChromeContentBrowserClient`, lets it supply the `BrowserWindow` of a `Browser` (a settable factory that
`BrowserWindow::CreateBrowserWindow` asks first), accepts a `Browser` with no side panel UI outside tests,
and allows a `Browser` in a BDK private profile. Each patch's message says why.

`bdk_overlay_marker()` stays as the cheapest proof that `//bdk` is linked: `chromium:build` runs
`nm -gU` on the built framework and fails if the symbol is absent.
