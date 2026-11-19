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
| `engine_backend.cc` | `bdkcore::Backend` over Chromium: start and stop, the profile registry (`BDK Profiles.json` in the user data directory; profile `profile-N` lives in directory `profile-N`), persistent profiles through `ProfileManager`, private profiles as separate off-the-record profiles of the base profile, and every page operation. |
| `engine_contents.{h,cc}` | One page: its `WebContents`, its observer (events) and its delegate (new-window, fullscreen and context-menu UI requests, keyboard routing, `window.close()`). |
| `host_view.h`, `host_view_mac.mm` | The native view the application places. It holds the page's view, so the page can be replaced (restore, discard) without the application noticing; the default context menu and fullscreen presenters. The only engine file with AppKit types besides start-up. |
| `session_state.{h,cc}` | Page history for the application to store: a BDK envelope (`BDKC`, version 1) around Chromium's session-restore records. |
| `identity.{h,cc}` | The application's identity from `bdk.resolved.json`: data directory, Keychain item, product name (T-PKG-02). |
| `content_browser_client.{h,cc}` | `BdkContentBrowserClient`, Chrome's browser client plus BDK's additions (the delegate supplies it through a patch). Today: the scripted-navigation interceptor. |
| `bdk_testing.h`, `testing.{h,cc}` | Test hooks, exported but not ABI: scripted navigations served without the network, UI requests for the application's own handler, script with a user gesture, synthetic clicks, a run-loop slice that lets Chromium work from inside a task, and a pending-work count. Used by the engine conformance app (`packages/core`, `nx run core:test-engine`). None gives a page or a session anything. |

Test and diagnostic switches, read from the environment of the application: `BDK_ENGINE_MOCK_KEYCHAIN=1`
(Chromium's mock Keychain, so tests never prompt), `BDK_ENGINE_SWITCHES` (more Chromium switches, space
separated, no dashes), and `BDK_ENGINE_KEEP_CHROMIUM=1` (a runtime shutdown leaves Chromium running, so
one process can start several runtimes; Chromium itself starts once per process).

## The patches it needs

`chromium/patches` links `//bdk` into the framework, exports the C ABI, `BDKApplicationBase` and the test hooks
(`chromium:build` fails when the framework does not export every function the headers declare), lets the
engine keep the content runner after start-up, leaves `NSApplication`'s delegate and menu to the
application, lets the application name its Keychain item, and lets the engine supply its own
`ChromeContentBrowserClient`. Each patch's message says why.

`bdk_overlay_marker()` stays as the cheapest proof that `//bdk` is linked: `chromium:build` runs
`nm -gU` on the built framework and fails if the symbol is absent.
