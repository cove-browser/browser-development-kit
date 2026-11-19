# BDK feasibility record

Date: 2026-11-14. This file records what was inspected, what was run, and what each result does and does not prove. The architecture that follows from it is in [architecture.md](architecture.md).

## How to read the status labels

| Status | Meaning |
|---|---|
| **Verified** | A test was executed in this session and the observed result is recorded here. |
| **Partially verified** | Upstream source at the pinned revision, a shipping precedent, or a local binary was inspected, and the conclusion follows from it. Nothing was built or run against Chromium. |
| **Unverified** | Not inspected closely enough, or the evidence is secondary. |
| **Disproven** | The evidence contradicts the assumption. |

No Chromium build was made for sections 1 to 8. Every statement about Chromium behaviour in them comes from reading source, not from running it.

Update 2026-11-17: milestone M1 synced, patched, built, packaged and ran the pinned Chromium on the host below, and rehearsed a roll to the next tag. The results are in section 9, under "M1 on a real Chromium checkout".

## Summary

| # | Investigation | Status | One-line result |
|---|---|---|---|
| 1 | Service retention without Chrome's visible UI | Partially verified | Upstream already has a second production window implementation that is not `BrowserView`; a BDK one fits the same seam. A WebContents outside a tab model loses almost every Chrome service. |
| 2 | Native hosting | **Verified** (2026-11-19, M2) | A Swift application owns `NSApplication` and hosts `//chrome` pages in its own views, with three small patches; renderer crash, focus, keys, visibility and accessibility work. Reparenting with a popup open and GPU restart are untested. |
| 3 | Extension compatibility | Partially verified | The tabs and windows APIs run on window and tab-list interfaces that a real `Browser` satisfies. Side panel, popup and toolbar hosting need BDK implementations of existing interfaces. |
| 4 | Branded privileged WebUI scheme | Disproven as a true origin; partially verified as an alias | A runtime `<scheme>://` origin with WebUI privileges is not a small patch. A runtime `<scheme>://` alias over `chrome://` is feasible in a prebuilt runtime. |
| 5 | Hybrid JavaScript broker | Partially verified | Chromium has the binding points needed for per-page capability checks. A proxy prototype was written and not executed. |
| 6 | Service providers | Partially verified | Workable non-Google defaults exist for translation and geolocation. Reputation has a real protection gap that needs a product decision. |
| 7 | Prebuilt packaging | Partially verified | One framework can serve many app identities with a short list of patches; the identity (data directory, Keychain item, product name) is taken at runtime (M2). Nested-helper signing in Xcode and notarization are unconfirmed. |
| 8 | Nx multi-language | **Verified** | C++, Swift and TypeScript targets build, cache and invalidate correctly from root scripts. |
| 9 | Worktrees and cache correctness | **Verified** (dummy runtimes in M0; a real Chromium checkout in M1) | Shared immutable runtime, isolated outputs, exclusive locking and stale-cache detection all behave as designed, and held on a real build. |
| 10 | Arc-style reference scenario | Partially verified | Scoped. Two coupling problems are identified: one active tab per window, and upstream split view stops at two panes. |

## Environment and limits

| Item | Value |
|---|---|
| Host | Apple M2 Max, 12 cores, 32 GB, macOS 27.2, APFS, about 106 GB free |
| Toolchain | Xcode 27.2, Apple clang 21, Swift 6.4.2, Node 26.9, pnpm 11.1.2, Nx 24.0.6, ccache 4.14, CMake 3.31, Ninja 1.12.1 |
| Not installed | depot_tools, GN, sccache, SwiftLint, Taplo, actionlint, shfmt, ShellCheck, Lefthook |
| Chromium pin used for evidence | Tag `158.0.8127.37`, commit `b8c2be851f62f6a394ec57955d7f31335d7cb6d4` (stable channel release of 2026-11-12) |
| Extended Stable at the time | `156.0.8078.100` |
| Chromium build | Not attempted. Disk is too tight and the brief rules out a full build for this assignment. |

### Source access

A sparse, blobless checkout of the pinned tag was made for inspection: 1.2 GB covering `docs`, `chrome/browser`, `chrome/common`, `chrome/app`, `extensions`, `content`, selected `components`, `url`, `sandbox` and `build/config`. The first attempt against `chromium.googlesource.com` stalled for about twenty minutes; the GitHub mirror completed in under a minute. Files outside the sparse set were fetched from `raw.githubusercontent.com/chromium/chromium/158.0.8127.37/<path>`.

Line numbers below refer to the pinned tag. Paths for CEF and Electron refer to their default branches on the day of inspection. CEF's default branch targeted Chromium `158.0.8127.0`, so it is a close match.

### Grep by Vercel MCP

| Step | Result |
|---|---|
| Was it connected? | No. |
| Configuration | `claude mcp add --transport http grep https://mcp.grep.app` succeeded and `claude mcp list` reported the server connected. |
| Tool access in this session | The tools load only into new sessions, so the server's `searchGitHub` tool was called directly over JSON-RPC with `curl`. `tools/list` confirmed one tool, `searchGitHub`. |
| Coverage of `chromium/chromium` | **None.** Queries scoped to `chromium/chromium` return no results, including for a symbol that certainly exists (`BrowserWindowInterface`). |
| What it does index | `chromiumembedded/cef`, `electron/electron`, `Alex313031/thorium` (partial copies of upstream files), `browseros-ai/BrowserOS`, `ungoogled-software/ungoogled-chromium`, `sparkle-project/Sparkle`. Not `brave/brave-core`. |
| How it was used | Discovery of call sites and patches in the downstream forks, which are the relevant precedents. All upstream claims were then checked in the pinned checkout. |

The brief's instruction to discover upstream Chromium locations through Grep could not be followed as written, because the index does not contain the upstream repository. That step is recorded as **not performed for upstream Chromium**. Upstream discovery used `rg` in the pinned checkout instead.

Queries that produced evidence used below: `cef::BrowserDelegate`, `GetBrowserViewForBrowser`, `BrowserWindowInterface` under `patch/patches`, and `TabInterface` under `libcef` (all CEF); `kStandardSchemes`, `LockSchemeRegistries`, `ShouldLockSchemeRegistry`, `redispatchKeyEvent`, `OverrideChildProcessPath` (Electron); `OnRegisterCustomSchemes`, `HandleKeyboardEvent`, `external_message_pump`, `OnScheduleMessagePumpWork` (CEF); `binaryTarget` in Sparkle's `Package.swift`.

### How the work was divided

Five research subagents were used, three on Claude Opus and two on Claude Sonnet, with separate scopes: services and tab/window coupling; extensions, WebUI and schemes; macOS hosting and packaging; providers and licensing; build, cache and CI mechanics. Two were interrupted by a usage limit and resumed. The lead ran the local experiments (8 and 9), spot-checked ten of the load-bearing source citations against the pinned checkout, and reconciled the findings. All ten matched.

---

## 1. Service retention without Chrome's visible UI

**Status: partially verified.**

### What the source shows

| Finding | Evidence |
|---|---|
| `Browser` is now a thin class that implements `BrowserWindowInterface` and owns a `TabStripModel`, a `BrowserWindow` and `BrowserWindowFeatures`. | `chrome/browser/ui/browser.h:102-104` and the member list |
| Upstream calls `Browser` a legacy object and tells feature code to depend on `BrowserWindowInterface` and `TabInterface`. | `docs/chrome_browser_design_principles.md:45-55, 320-327` |
| One factory chooses the window implementation, and it already has two production choices: `BrowserView` and `WebUIBrowserWindow`. | `chrome/browser/ui/views/frame/browser_window_factory.cc:31-39` |
| `WebUIBrowserWindow` is a `BrowserWindow` that is not a `BrowserView`. It returns null from `AsBrowserView()`, stubs 54 methods, and supplies a real modal-dialog host, find bar, exclusive-access context and a stub location bar. A macOS file exists. | `chrome/browser/ui/webui_browser/webui_browser_window.h:42-46`; `.cc:974-982, 1102-1115`; `webui_browser_window_mac.mm` |
| `BrowserWindowFeatures` already branches on which window type is attached. | `chrome/browser/ui/browser_window/internal/browser_window_features.cc:665-700` |
| Upstream provides an embedder hook for window-scoped features. | `chrome/browser/ui/browser_window/public/embedder_browser_window_features.h:12-46` |
| A caller-supplied `BrowserWindow` is accepted by `BrowserWindowCreateParams`, but only in tests. | `create_browser_window.h:147-149`; `browser.cc:307-309` (`CHECK_IS_TEST()`) |
| `BrowserWindow` has 72 pure virtual methods. | `chrome/browser/ui/browser_window.h` (counted) |
| Every tab-scoped service is created when a `TabModel` is constructed, and that code requires a browser window. | `chrome/browser/ui/tabs/tab_model.cc:52-71`; `tab_features.cc:427` (`CHECK(tab.GetBrowserWindowInterface())`); 177 owned members in `tab_features.h` |
| History does not record a WebContents that is not in a browser's tab collection. | `chrome/browser/history/history_tab_helper.cc:703-705` |
| Tabs move between windows without being rebuilt. | `chrome/browser/ui/tabs/tab_strip_model.h:296-348` (`DetachTabAtForInsertion`, `InsertDetachedTabAt`) |
| `BrowserList` and `browser_finder.h` no longer exist. Lookups go through `GlobalBrowserCollection`. | Files absent at the tag; `browser_window/public/global_browser_collection.h:28` |
| Code outside the Views directory still casts to `BrowserView`: 162 call sites in 118 non-test files, 44 of those files outside `chrome/browser/ui/views`. | `rg` count at the tag |
| The only non-`Browser` implementation of the window interface is Android's reduced one. | `browser_window/internal/android/android_browser_window.h:19` |

### Per-service coupling

W = works with a bare WebContents and profile. T = needs a tab object. B = needs a `Browser` and its tab model. V = needs a `BrowserWindow` method or Views UI.

| Service | Class | Evidence | Consequence for BDK |
|---|---|---|---|
| Session service and restore | B | `session_service.h:144` | Windows restore as `Browser` objects; BDK maps them to app containers. |
| History | T/B | `history_tab_helper.cc:703` | A bare WebContents is not recorded. |
| Bookmarks model | W | No `Browser` references in the model | Works. The bubble is UI. |
| Downloads core | W | `download_core_service_impl.cc` | Works. |
| Download UI | B+V | `download_bubble_ui_controller.cc:94-98` | BDK presents downloads over the existing model. |
| Password manager and autofill logic | T | `tab_features.cc:440-445` | Needs the tab object. |
| Autofill and password bubbles | T+V | `chrome_autofill_client.cc:787-789`; `browser_window.h:259` | BDK implements or stubs the bubble handler. |
| Permission prompts | T+V | `permission_prompt_factory.cc:44-103` | Anchoring needs a location-bar object; Webium uses a stub. |
| Printing and print preview | T+V | `print_preview_dialog_controller.cc:136-140, 266-275` | Needs a modal-dialog host from the BDK window. |
| Translate | T+B+V | `chrome_translate_client.cc:608-631` | UI is shown only for the active tab of the last-active window. |
| Safe Browsing interstitials | W | `chrome_safe_browsing_blocking_page_factory.cc` | Works at content level. |
| DevTools | B, docked V | `devtools_window.cc:2139-2141` | Undocked DevTools creates a window through the same factory. |
| JavaScript dialogs | T+B | `javascript_tab_modal_dialog_manager_delegate_desktop.cc:66-99` | Foreground means the active tab and its split peers. |
| Find in page | T, bar B+V | `find_bar_controller.cc:48-65` | BDK implements the find bar; it follows the active tab. |
| Fullscreen | B+V | `browser_window.h:455` | BDK supplies an exclusive-access context. |
| Popups and new windows | B | `browser_web_contents_delegate.cc:700-788`; `browser_navigator.cc:404-470` | Every disposition becomes a tab insertion or a new `Browser`. |

### Precedents

- **CEF, Chrome style**: a real `Browser` and `TabStripModel` with a CEF-owned `BrowserView` subclass passed through the test-only window field, after patching out the check (`libcef/browser/chrome/chrome_browser_host_impl.cc:412-449`; `patch/patches/chrome_browser_browser.patch`, 19 files; `chrome_runtime_views.patch`, 23 files). CEF documents that a Chrome-style window hosts at most one Chrome-style view (`include/internal/cef_types_runtime.h`).
- **CEF, Alloy style**: no `Browser`. Permission prompts default to deny, downloads are cancelled by default, and CEF carries a patch that splits tab-feature initialisation to cope (`patch/patch.cfg:253`).
- **Arc 1.171.0** (installed, inspected read-only): a Swift application whose `NSPrincipalClass` is a Swift class conforming to Chromium's application protocol, linking a Chromium framework (`ArcCore`, version `156.0.8078.74`) that exports 147 Objective-C API classes. This is closed source, so it shows the approach ships, not how.

### Conclusion

Keep a real `Browser` and `TabStripModel` for each application window, and replace only the `BrowserWindow` with a BDK implementation modelled on `WebUIBrowserWindow`. The alternatives are worse: implementing the window interface without `Browser` means rebuilding most of `Browser`, and hiding a `BrowserView` CEF-style cannot show several pages in one window.

Estimated upstream patch footprint for this strategy: five to ten files, plus new BDK files. That estimate is unverified.

### Not verified

- Whether a BDK window with stubbed methods survives startup, session restore, DevTools, print preview and permission prompts without a null dereference in the 44 non-Views files that cast to `BrowserView`. (2026-11-19, M3: it survives Browser creation, tab insertion, moves between contexts, activation, discard, restore, popups, DevTools and closing, as exercised by the engine scenarios; print preview, permission prompts, session restore of windows, translate and find are not yet run. The crashes found and their handling are in [amendments.md](amendments.md) section 2.13.)
- Whether the Webium window is complete enough on macOS to copy from.
- Whether every caller honours a hidden tab strip.

### Smallest follow-up

At the pinned tag on macOS, copy `WebUIBrowserWindow` into a BDK window behind a flag in `browser_window_factory.cc`. Run the `browser_tests` filters for session restore, permissions, print preview, translate, find, DevTools and the tabs API. Log every null `BrowserView` and every `NOTIMPLEMENTED`. Then move a tab between two such windows while a JavaScript dialog is open and while a permission prompt is pending.

---

## 2. Native hosting

**Status: verified on 2026-11-19 (see "Verified on 2026-11-19" below); the rest of this section is the 2026-11-14 source reading.**

| Question | Finding | Evidence |
|---|---|---|
| Can the WebContents view go into any `NSView`? | Yes. It is a plain `NSView`. content_shell and CEF both add it as a subview, and Chrome's own host does the same with a thin wrapper. | `content/browser/web_contents/web_contents_view_mac.mm:138-140`; `content/shell/browser/shell_platform_delegate_mac.mm:226-230`; `ui/views/controls/native/native_view_host_mac.mm:123-165` |
| Does visibility follow the view hierarchy? | Yes. The view recomputes hidden, occluded or visible when it moves between windows or is hidden. | `content/app_shim_remote_cocoa/web_contents_view_cocoa.mm:531-543, 597-610` |
| Is occlusion per view? | No, per window. A page covered by a sibling view still counts as visible. | Same file, `:570-594` |
| Who handles keyboard shortcuts? | The page sees key equivalents first. The host must route the unhandled ones. | `render_widget_host_view_cocoa.mm:1380-1431`; `ui/base/cocoa/command_dispatcher.h:124-152` |
| Popups, drag and drop | Owned by `//content`; no host work. | `web_contents_view_mac.mm:388-400`; `web_contents_view_cocoa.mm:209-407` |
| Context menus, fullscreen | Need an embedder delegate. | `web_contents_view_mac.mm:379-385` |
| Accessibility | The host must set the accessibility parent explicitly. | `render_widget_host_view_mac.mm:422-436` |
| Can a Swift app own the event loop? | The message pump only needs the application object to conform to a small protocol, and it nests its own loop if the application is already running. | `base/message_loop/message_pump_apple.mm:786-815, 904-919` |
| What blocks that in `//chrome`? | `//chrome` checks that the application object is its own subclass, checks that there is no application delegate, then installs its own delegate and main menu. | `chrome/browser/chrome_browser_application_mac.mm:39-46, 165-184`; `chrome/browser/chrome_browser_main_mac.mm:157-166` |
| Precedents for an app-owned application object | CEF, Electron, Arc. | CEF `include/cef_application_mac.h:60-74`; Electron `shell/browser/mac/electron_application.h:14-16`; Arc binary inspection |
| Renderer crash, GPU restart | API presence only. | `content/public/browser/web_contents_observer.h:282` |

### Required patches, from the source reading

1. Accept any application object that conforms to the control protocol, instead of requiring `BrowserCrApplication`.
2. Skip Chrome's application delegate, main menu and the nil-delegate check.
3. Route termination through a BDK callback.
4. Suppress the startup window and hold a keep-alive owned by BDK.
5. Provide an entry point that initialises and runs the content main runner from inside an application that is already running.

### Not verified

- Whether the nested run loop works for `//chrome` when started from a running `NSApplicationMain`. CEF and Arc suggest it does.
- Reparenting while a popup, a `<select>` menu or fullscreen is open.
- Input-method and VoiceOver behaviour in a non-Views host.
- GPU process restart.

### Verified on 2026-11-19 (M2, Chromium 158.0.8127.37, macOS 27.2, Apple silicon)

Built and run, not read: the engine in `chromium/bdk` with the patch series, against the release build.

| Question | Result | How |
|---|---|---|
| Does `//chrome` run nested inside an application's `NSApplication` run loop? | Yes. `ChromeMainDelegate` is subclassed so `BrowserMainRunner::Initialize` returns to the application; `Run` is then called from a one-shot run-loop timer and nests (`MessagePumpNSApplication` sees the application running). A run-loop *block* starves every block queued with it, and a main-queue block blocks the main queue: both were tried and failed. | Engine start-up; example and conformance app |
| Which checks fire? | `RegisterBrowserCrApp` (NSApp must be a `BrowserCrApplication`: met by subclassing it), the nil-delegate check and the main menu (patched to skip under `--bdk-hosted`), `RunContentProcess` shutting the runner down after `Run` (patched: `keep_runner_after_run`), `BrowserCrApplication +initialize` reading the command line (the base class initialises it in `+load`), and a feature-flag read in `-finishLaunching` before Chromium starts (skipped when there is no feature list yet). | Spike and engine |
| Clean termination | Yes, with one constraint: `NSTerminateLater` deadlocks, because AppKit's reply loop nests inside Chromium's loop, which cannot quit until it returns. Cancel and terminate again after the runtime stops. | Example quits with status 0 |
| Input methods, VoiceOver | Key events reach the page and unhandled ones the application; VoiceOver's activation exposes the page's accessibility tree. The view needs no explicit accessibility parent: in-process hosting keeps AppKit's own hierarchy. Composition by an input method needs a key window and was not exercised from a terminal-launched harness. | Engine scenarios |
| Renderer crash | `PrimaryMainFrameRenderProcessGone` reaches the application; a reload recovers; other pages are untouched. | Example end-to-end test |
| Reparenting with a popup or `<select>` open, GPU process restart | Not tested. | — |

### Proxy experiment, not executed

A fixture using Electron as a stand-in for "a prebuilt Chromium runtime hosted by an application" was written. It checks two WebContents in one window, detaching and reparenting one into another window with page state and renderer process preserved, and recovery from a renderer crash. It is in [experiments/hybrid-broker-proxy/](experiments/hybrid-broker-proxy/). The run was not carried out in this session, so it contributes no evidence. Electron does not use `//chrome`, so even a passing run would only show content-layer behaviour.

### Smallest follow-ups

1. A content_shell-based spike: a Swift application whose principal class conforms to the control protocol calls the content main runner from `applicationWillFinishLaunching`. Check page load, input methods and command keys.
2. The same against `//chrome` with the five patches, recording which checks still fire.
3. Two WebContents in one window, then one moved to a second window with a `<select>` open. Log visibility, focus and the accessibility tree.

---

## 3. Extension compatibility

**Status: partially verified.** A fixture exists and was not run.

| Finding | Evidence |
|---|---|
| `tabs.query` iterates browser windows and their tab lists. A WebContents outside those is invisible to it. | `chrome/browser/extensions/api/tabs/tabs_api.cc:560-657` |
| "Current window" is the calling context's window controller, or the most recently activated window. | `chrome/browser/extensions/chrome_extension_function_details.cc:38-81` |
| Tab events are driven by tab-list observation, and on desktop also by a `TabStripModel` tracker. | `tabs_event_router.cc:237-481`; `tabs_event_router_platform_delegate_non_android.cc:40-62` |
| Tab identifiers survive moves between windows. | `extension_tab_util.cc:346-353, 618-660` |
| `activeTab` is attached per WebContents with no `Browser` involved, and is revoked on navigation. | `chrome/browser/extensions/tab_helper.cc:82-99`; `extensions/browser/permissions/active_tab_permission_granter.cc:135, 255` |
| Clicking an action is window-agnostic and returns what the UI should do. | `extension_action_runner.cc:95-150` |
| A popup host needs a window interface, and its view is pluggable. | `extension_view_host_factory.cc:260`; `extension_view_host.h:38-79`; `extension_view.h:33-58` |
| Upstream has a non-Views toolbar container for extensions. | `chrome/browser/ui/webui/webui_toolbar/webui_toolbar_extensions_container.h:37` |
| The side panel is created only when a `BrowserView` exists. | `browser_window_features.cc:819-823, 886-894` |
| Context-menu items are a model; executing one grants `activeTab`. | `render_view_context_menu.cc:1122, 1213`; `menu_manager.cc:690-747` |
| `--load-extension` is ignored only in Google-branded builds. | `chrome/browser/extensions/extension_service.cc:409-437` |
| Off-store CRX installation is denied by default. | `chrome/browser/extensions/extension_management.cc:388-402` |
| Store and update URLs are overridable by switch and by client hooks. | `extensions/common/extension_urls.cc:63-67`; `chrome_extensions_client.cc:80-95`; `extensions_client.h:129-142` |
| `webstorePrivate` is exposed to the Chrome Web Store origin in any Chromium build. | `extensions/common/api/_api_features.json:1009-1020` |
| API availability is compiled-in metadata with a delegated-check option. | `_api_features.json:1018` |

How far the interface layer goes: the tabs API, the tab utilities and the event router core already work against `BrowserWindowInterface` and `TabListInterface`. Three desktop paths still need a real `TabStripModel`, and the side panel, toolbar and popups are created only with a `BrowserView`. In `chrome/browser/extensions`, 20 non-test files use `TabListInterface` and 13 still mention `TabStripModel`.

Precedent for giving up `Browser`: CEF carries a patch to add "minimal support for tabs API" to Alloy browsers, and Electron documents partial tab support with no CRX installation.

### Surface table

| Surface | Depends on | BDK work | Risk for split view and moves |
|---|---|---|---|
| `tabs.query`, `get`, `getCurrent` | Window interface, tab list | None if every container is backed by a `Browser` | **High**: tabs in hidden spaces appear in results; one active tab per window |
| `tabs.move`, `windows.*`, attach and detach events | Tab list, `TabStripModel` | Native moves must go through the tab model | **High**: which event fires depends on how spaces are modelled |
| `tabs.onActivated` | Tab-list observer | Map pane focus to tab activation | Medium |
| Split view | `TabListInterface::CreateSplit`, `tabs.createSplit` | Map application splits to Chrome splits where they fit | Medium: upstream UI supports two panes |
| `activeTab` | Per-WebContents helper | Toolbar, shortcuts and menus target the focused pane | Low |
| Action popup | Popup host, pluggable view | Native `ExtensionView` and container | Low to medium |
| `action.openPopup` | Extensions container on the window | Native container | Low |
| `chrome.sidePanel` | Views-gated side panel | Patch `BrowserWindowFeatures`, native side-panel implementation | Medium |
| Context menus | Menu model | Render the model as a native menu | Low |
| Options page | Window controller | None | Low |
| DevTools extensions | DevTools window | Undocked first | Medium, unverified |
| Service workers, messaging | `extensions/browser` only | None | Low |
| Unpacked and command-line loading | Extension service | Do not set Google branding | Low |
| CRX and store installs | Installer, management policy, Google endpoints | Own the off-store policy; decide on store use | Medium, legal |

### Fixture

[experiments/mv3-probe/](experiments/mv3-probe/) is a small Manifest V3 extension with a service worker, content script, action popup, side panel, options page and context-menu item. The worker records tab and window events and answers probes for query results, injection, messaging, moves, popup and side-panel opening. **It has not been executed, not even in stock Chromium.** Treat the first run as part of milestone M0.

Assertions for the first real run are listed in [tasks.md](tasks.md) under T-EXT-01.

---

## 4. Branded privileged WebUI schemes

**Status: disproven as a true runtime origin with a small patch; partially verified as a runtime alias.**

`<scheme>` below is whatever the application's `bdk.toml` configures, read at start-up. It is not a fixed name. The spec's example is `cove`, for an application named Cove.

| Finding | Evidence |
|---|---|
| WebUI scheme recognition in `//content` is a fixed list of three schemes. | `content/public/common/url_utils.cc:31-41` |
| The embedder hook for extra WebUI schemes is documented as aliasing and its result is cached for the life of the process. | `content/public/browser/content_browser_client.h:606-611`; `content/browser/webui/url_data_manager_backend.cc:63-83` |
| The WebUI config map refuses any scheme except `chrome` and `chrome-untrusted`. | `content/public/browser/webui_config_map.cc:73, 79` |
| Schemes are registered once per process and then locked, in every process. | `content/common/url_schemes.cc:54-138` |
| The renderer hard-codes WebUI properties to `chrome`. | `content/renderer/render_thread_impl.cc:886-892` |
| Process locking keys off the scheme list. | `content/browser/security/cpsp/child_process_security_policy_impl.cc:1991-2004` |
| Extension API availability for internal pages matches literal `chrome://` URLs in compiled-in metadata. | `chrome/common/extensions/api/_api_features.json` (54 lines) |
| Volume of hard-coding: 346 `"chrome://` literals in 116 non-test files under `chrome/`; 1,826 resource files reference `chrome://resources`. | `rg` counts at the tag |
| URL rewriting is a pluggable handler chain. | `chrome/browser/chrome_content_browser_client.cc:5228-5257` |
| Brave keeps `chrome://` internally and treats `brave://` as a rewritten alias with a compile-time name. | brave-core `common/brave_content_client.cc:87-92`; `chromium_src/chrome/browser/browser_about_handler.cc` |
| Electron registers privileged schemes at runtime in a prebuilt binary and passes them to child processes on the command line. | Electron `shell/browser/api/electron_api_protocol.cc:115-211`; `shell/app/electron_content_client.cc:309-330` |
| CEF requires identical scheme registration in every process. | CEF `include/cef_app.h:222-230` |

### Options

| Option | Per-app at runtime | Verdict |
|---|---|---|
| A. `<scheme>://settings` commits as its own privileged origin | The name could come from a switch, but the feature metadata and resource references are compile-time | **Disproven as a small patch.** Two privileged origins and inconsistent checks make it a security risk as well. Not recommended. |
| B. `<scheme>://` is an alias: registered as a standard, secure, non-WebUI scheme in every process from a startup switch, rewritten to `chrome://` on navigation, and displayed as `<scheme>://` by the application UI | Yes | **Partially verified, recommended.** Brave proves the rewrite, Electron proves runtime registration, `//content` provides the per-process hook. The combination has not been built. |
| C. Alias with a compile-time name | No | Feasible, but unacceptable for prebuilt mode. |

What option B means in practice: the address the user sees and types is `<scheme>://settings`. The origin the page commits as is `chrome://` with a BDK-namespaced host, for example `chrome://bdk-settings`. Code inside the page sees the `chrome://` origin. So does DevTools. This is a product-visible difference from the spec's wording and is raised in [risks.md](risks.md).

### Not verified

- A link to `<scheme>://…` on an ordinary web page. It should be blocked the way `chrome://` links are today.
- Session restore and history display of aliased URLs.
- That nothing leaks the internal name to web content.

### Smallest follow-up

On a real build: add the scheme switch, the handler pair and one `chrome://bdk-test` page. Launch with two different scheme names without rebuilding. Check that both load, that the page's process has WebUI bindings and the expected process lock, and that an `https` page cannot navigate to either form.

---

## 5. Hybrid JavaScript broker

**Status: partially verified. The prototype was written and not executed.**

| Finding | Evidence |
|---|---|
| A Mojo interface can be bound only for a specific WebUI controller type in a main frame; anything else is rejected and the renderer is reported. | `content/public/browser/web_ui_controller_interface_binder.h:91-115` |
| Each WebUI controller type has its own interface registry; an unregistered request terminates the renderer. | `content/public/browser/web_ui_browser_interface_broker_registry.h:97-122`; `content/browser/per_web_ui_browser_interface_broker.cc:19-41` |
| Identity signals available at bind time: last committed origin, primary-main-frame status, WebUI bindings on the process, the process lock, and the profile. | Same files, plus `child_process_security_policy_impl.cc:1871` |

These are the binding points the per-page capability broker needs. The design that follows is in [architecture.md](architecture.md).

### Proxy prototype

[experiments/hybrid-broker-proxy/](experiments/hybrid-broker-proxy/) implements the shape of the design on Electron: a scheme name read from a configuration file at startup; one broker and one settings service; an internal-page path whose identity is derived natively from the committed frame; and a separate automation process that authenticates over a local socket with a token and reaches the same broker. It asserts that the settings page can read and write, the new-tab page is denied through the same bridge, an alias shares the target's identity, an ordinary origin and a subframe are denied, privilege ends on navigation, a wrong token is rejected, and a write through automation is visible to the page.

**It was not run in this session.** It is also not the BDK mechanism: it uses Electron's process bridge, not Chromium WebUI and Mojo. If run, it would support the authorisation logic and the runtime-scheme claim at the content layer only.

### Smallest follow-up

On a real build: one `chrome://bdk-settings` page with a capability broker bound through the controller-type binder, one page without it, and an automation client on a local socket. Assert the same list as the proxy fixture, plus that a compromised renderer asking for the interface from the wrong page is terminated.

---

## 6. Service providers

**Status: partially verified.** Source items were read at the pinned tag. Terms and prices were read from vendor pages on 2026-11-14 where stated; the rest is unverified and marked so.

### Translation

| Finding | Evidence | Status |
|---|---|---|
| Chromium's page translation injects Google's `element.js` and sends page text to Google with an API key. | `components/translate/core/browser/translate_script.cc:56`; `translate_url_util.cc:25` | Read |
| Translation is disabled when no Google API key is configured. | `translate_manager.cc:214, 790, 971` | Read |
| The script URL is overridable by switch. | `translate_script.cc:154-175` | Read |
| The renderer contract is "run this script": `TranslateAgent.TranslateFrame(translate_script, source, target)`. | `components/translate/content/common/translate.mojom:84-130` | Read |
| Chromium's on-device translation is a Google-signed component download; the library and models are not in the source tree. | `components/on_device_translation/constants.cc`, `component_manager.cc` | Read |
| Mozilla's translation models are MPL-2.0: 116 language pairs across 60 language codes in the Firefox model collection, median file 17 MB, largest 59.5 MB. The count includes some alpha entries. | `github.com/mozilla/translations`; Firefox Remote Settings `translations-models` | Read |
| Apple's `TranslationSession` can be created without SwiftUI for installed language pairs on macOS 15 and later. | Apple documentation JSON | Read |
| Download flow for pairs that are not installed | | Unverified |

Recommended default: a local engine using the Bergamot translator and Mozilla models, driven by a BDK renderer agent that replaces the script-injection step. Optional providers, each needing the application's own key: Apple on macOS, DeepL, Microsoft, Google.

### Security and reputation

| Finding | Evidence | Status |
|---|---|---|
| Safe Browsing endpoint prefixes are constants with a test-only override. | `components/safe_browsing/core/browser/db/sb_protocol_manager_util.cc:40-53` | Read |
| The Google Safe Browsing API is for non-commercial use only. | `developers.google.com/safe-browsing/v4/usage-limits` | Read |
| Google Web Risk is the commercial route: first 100,000 lookups a month free, then $0.50 per 1,000; the list-diff API is free. | Web Risk pricing page, via search result | Read; other terms unverified |
| Google API keys may not be shared outside the legal entity that accepted the terms. | `chromium.org/developers/how-tos/api-keys` | Read |
| Brave routes Safe Browsing through its own proxy hosts. | brave-core `components/constants/network_constants.h:9-39` | Hosts read; agreement unverified |
| Open feeds: URLhaus, PhishTank, OpenPhish and others | | Licences unverified |

There is no open equivalent to Google's protection. Open feeds list known threats. They do not provide real-time checks, download reputation, machine-learned phishing detection or leak detection. A BDK default built on them is weaker than Chrome, and the plan must say so wherever protection is described.

### Security data delivered by the component updater

| Finding | Evidence | Status |
|---|---|---|
| The default update server is Google's and is overridable by switch and by configurator. | `components/component_updater/component_updater_url_constants.cc:17-20`; `configurator_impl.cc:78-87` | Read |
| Each component is verified against a pinned public-key hash. | `components/update_client/unpacker.cc:72-97` | Read |
| So a fork can serve Google-signed component packages unmodified without holding a signing key. | Follows from the above | Inference |
| Whether a mirror can answer the signed update-check protocol without Google's key | | **Unverified** |

Revocation lists, the Certificate Transparency log list and root-store updates arrive this way. BDK has to keep them flowing.

### Geolocation

| Finding | Evidence | Status |
|---|---|---|
| A CoreLocation-based provider exists and the provider manager is enabled by default on macOS and Windows. | `services/device/geolocation/core_location_provider.h:20, 52`; `services/device/public/cpp/device_features.cc:116-121` | Read |
| The macOS default mode is hybrid; Windows is platform-only; other platforms are network-only. | `device_features.cc:158-173` | Read |
| The network provider posts to a Google endpoint with no switch to change it. | `network_location_request.cc:49` | Read |
| Whether hybrid mode on macOS ever calls the network provider | | Unverified |

Recommended default: the operating-system provider, with the mode forced to platform-only on macOS.

### Extension stores

| Route | Works technically | Assured by terms |
|---|---|---|
| Chrome Web Store | Yes, in any Chromium build | No. The terms describe distribution "to users of Google Chrome" and have no clause for other browsers. |
| Self-hosted update manifest | Yes | Yes |
| Policy installation | Yes | Yes |
| Microsoft Edge Add-ons, Opera add-ons | Unverified | Unverified |

### Release cadence

Chrome moved to a two-week stable cycle with Chrome 153 on 2026-09-08. Extended Stable keeps eight weeks and is public. Whether every stable security fix is backported to Extended Stable was not measured.

### Unverified and needing legal review

Widevine licensing, patent licensing for H.264, AAC and HEVC, trademark rules, export notices, feed licences, Web Risk terms beyond price, and the terms for redistributing Google-signed component data.

---

## 7. Prebuilt packaging

**Status: partially verified.** Source reading plus read-only inspection of the installed Chrome 157 and Arc 1.171 bundles.

| Finding | Evidence |
|---|---|
| Chrome's executable is a loader with the product name and version compiled into the framework path. | `chrome/app/chrome_exe_main_mac.cc:179-219` |
| Framework, helper and product names are compile-time constants. | `chrome/common/chrome_constants.cc:14-86`; `chrome/app/theme/chromium/BRANDING` |
| The base bundle identifier is taken from the outer application at runtime, and Mach and sandbox names follow it. | `chrome/app/chrome_main_mac.mm:105-124`; `base/apple/mach_port_rendezvous_mac.cc:46`; `content/browser/sandbox_parameters_mac.mm:130-138` |
| The embedder can choose the helper path at runtime. | `content/browser/child_process_host_impl.cc:58-130` |
| Electron derives helper names from the application name at runtime. | Electron `shell/app/electron_main_delegate_mac.mm` |
| The Keychain item name is a mutable static; the user-data directory comes from an `Info.plist` key. | `components/os_crypt/common/keychain_password_mac.mm:33-84`; `chrome/common/chrome_paths_mac.mm:25-69` |
| A profile used by a newer milestone is "unsupported" on an older one unless a snapshot exists. | `chrome/browser/downgrade/downgrade_manager.cc:158-337` |
| Signing order and options per helper are defined in the installer scripts. | `chrome/installer/mac/signing/parts.py:60-295` |
| Renderer and GPU helpers carry only the JIT entitlement. | `chrome/app/helper-renderer-entitlements.plist`; confirmed with `codesign` on the installed Chrome |
| A SwiftPM binary target can carry a framework with nested applications. | Sparkle's `Package.swift`; nested `Updater.app` inside Arc is signed with Arc's team identifier |

Measured on installed bundles:

| Bundle | Framework directory | Notes |
|---|---|---|
| Chrome 157.0.8103.40 | About 730 MB per version, universal | Two version directories side by side; helpers include four "Aperitif" variants |
| Arc 1.171.0 | 592 MB | `Versions/A`, neutral helper names ("Browser Helper"), no updater in the framework, Keychain name formatted at runtime |

Conclusion: one prebuilt framework can serve many application identities if helper and framework names are fixed to neutral values at build time and the framework path is resolved from the loaded image. Bundle identifiers, the Keychain name and the data directory are already runtime values or `Info.plist` rewrites.

### Not verified

- Whether Xcode's "Embed & Sign" re-signs nested helper applications. The plan assumes it does not and supplies a signing script.
- What the Aperitif helpers are for and whether BDK needs them. (The signing step handles them like their plain counterparts; in the run below the headless browser used the Aperitif Renderer and Aperitif helpers.)
- Notarization of an application that embeds the re-signed runtime, and Gatekeeper acceptance. No credentials were available.
- Signing with a real Developer ID identity: the hardened runtime options and library validation with a real team identifier.
- Downloading a SwiftPM binary target from a URL. SwiftPM refuses non-https URLs for binary targets (`valid schemes are: 'https'`), so this can only be tested against a published release asset.

### Verified on 2026-11-18

Re-signing under another identity (T-PKG-03). Runtime: the packaged Chromium 158.0.8127.37 at `~/.bdk/runtimes/a3c82d8de4cecdf1/Chromium.app` (697 MB), cloned with `ditto` into scratch space and made a second application (`CFBundleIdentifier` `com.second.Browser`); `bdk build sign --identity -` from `packages/cli` (see [sdk/cli.md](sdk/cli.md) section 5.1 for the plan and the full observations).

| Finding | Evidence |
|---|---|
| A packaged runtime can be re-signed under a different bundle identifier and still launch. | `codesign --verify --deep --strict` passed. `--headless=new --dump-dom data:text/html,<p>hi</p>` printed the rendered page. The process list showed renderer, utility and GPU-process children from re-signed helpers (the GPU process ran from the plain Aperitif helper, not from a `(GPU)` helper). |
| Helper identifiers can be rewritten with `plutil -replace CFBundleIdentifier` before signing. | The signed helpers report `com.second.Browser.helper`, `.helper.renderer` and `.framework.AlertNotificationService`; the framework reports `com.second.Browser.framework`. |
| Renderer and GPU helpers need only `com.apple.security.cs.allow-jit`. | Signed with that entitlement alone; the renderer helper ran. The `(GPU)` helper was not launched in the run, and with an ad hoc identity the hardened runtime is off, so this is not a test of the entitlement's necessity. |
| Chromium's `--options` (`restrict`, `library`, `runtime`) cannot be used with an ad hoc identity. | With them the application fails to start: `different Team IDs` (library validation, implied by the hardened runtime). Without `library` and `runtime` but with `restrict`: `security policy does not allow @ path expansion`. With no options it starts. |
| Gatekeeper rejects an ad hoc re-signed application. | `spctl --assess --type execute -vv` printed `rejected`, exit 3. Expected; not a failure. |
| A SwiftPM `.binaryTarget(path:)` over a zipped xcframework of the 713 MB "Chromium Framework.framework" keeps all 5 symlinks and an identical file tree after extraction. | Reported by the coordinator, not run in this session: archive made with `zip -ry`, 235 MB; `swift build` resolved it in about 11 s. |

---

### 7.1 System codecs (T-CHR-12, decision D7), 2026-11-19

Measured on the packaged runtime: pages are offered VP9, AV1 and Opus, and no H.264, AAC or HEVC, in media elements and in Media Source (an engine scenario pins this). The build has `proprietary_codecs = false` (the default for a non-Chrome-branded build, `build/config/features.gni:31`) and `ffmpeg_branding = "Chromium"`, so no proprietary decoder is bundled.

From the source at the pin:

| Piece | State |
|---|---|
| MP4 demuxing and the H.264, AAC and HEVC codec strings | Behind `proprietary_codecs`. Turning it on with `ffmpeg_branding = "Chromium"` keeps FFmpeg's proprietary decoders out. |
| H.264 decode through the system | VideoToolbox (`media/gpu/mac/video_toolbox_h264_accelerator.cc`, the decompression session manager) in the GPU process. |
| HEVC | `enable_platform_hevc` follows `proprietary_codecs` (`media/media_options.gni:159`); decode is VideoToolbox's. |
| AAC through the system | `media/filters/mac/audio_toolbox_audio_decoder.cc` decodes only xHE-AAC, AC-3 and E-AC-3; AAC-LC would need a patch to accept it (the AudioConverter set-up already handles AAC magic cookies). Its size is not yet known. |
| H.264 in WebRTC | Not examined. |

Not done: the build with `proprietary_codecs = true` that would show H.264 and HEVC playing through VideoToolbox and measure the AAC patch. It changes a GN argument that rebuilds most of the tree (hours), so it belongs in an experiment checkout. D7 stays as decided; T-CHR-12 is not an M2 exit criterion.

## 8. Nx multi-language feasibility

**Status: verified.** Fixture: [experiments/nx-multilang/](experiments/nx-multilang/).

Graph: `core` (C++ compiled with clang into a static library behind a C header), `swift` (a SwiftPM package that links the library through a module map), `js-protocol` (TypeScript, with a `codegen` target that generates declarations from an interface file in `core`).

Commands, run in the fixture directory:

```sh
pnpm install
pnpm exec nx graph --file=graph.json
pnpm build && pnpm test
pnpm exec nx run-many -t build test --output-style=static   # repeated after each change
pnpm exec nx show projects --affected --base=HEAD
```

| Case | Change | Observed |
|---|---|---|
| a | Cold run after `nx reset` | All six tasks executed; both tests passed |
| b | No change | All tasks served from the local cache |
| c | Output directories deleted | Outputs restored from cache: static library, Swift binary, compiled TypeScript |
| d | C++ source edited | `core:build` and `swift:build` and `swift:test` re-ran; TypeScript tasks stayed cached |
| e | Swift source edited | Only `swift:build` and `swift:test` re-ran |
| f | Native interface definition edited | Only `js-protocol` codegen, build and test re-ran |
| g | Toolchain identity changed through an environment variable, no file changed | Native tasks re-ran; TypeScript stayed cached. Restoring the identity gave cache hits again |
| h | `implicitDependencies` removed | The graph showed no edges, and `swift:build` ran before the library existed and failed at link |
| i | `nx affected` | A C++ change marked all three projects; a Swift change marked only `swift` |

Other observations:

- pnpm 11 skipped Nx's install script until `nx` was listed under `allowBuilds` in `pnpm-workspace.yaml`.
- TypeScript 7.0.2 compiled the fixture with `tsc` unchanged.
- Nx 24 stores its local cache in `~/.nx/<workspace-id>/cache`, outside the workspace.

Limits: the fixture is tiny. Xcode project builds, `xcodebuild` caching and large Swift packages were not exercised.

---

## 9. Worktree and cache correctness

**Status: verified, with dummy runtime artifacts and a simulated source build.** Fixture: [experiments/worktree-cache/](experiments/worktree-cache/).

Setup: one Git repository with two extra worktrees (`wt-a`, `wt-b`); a store directory standing in for `BDK_HOME`; a resolver that computes an identity key from the pin, patches, overlay, GN arguments, platform and toolchain; a simulated source build that runs under `lockf` and publishes an immutable runtime.

| Case | Action | Observed |
|---|---|---|
| 1 | Resolve with no runtime published | Failed with a message naming the key and the two remedies |
| 2 | Publish a runtime once | Store entry created with a manifest; files read-only |
| 3 | Resolve in both worktrees | Both symlinks point to the same store directory; same inode |
| 4 | Write into the runtime from a worktree | Refused: permission denied |
| 5 | Re-run in `wt-a` | Resolve ran again; the dependent build was a cache hit |
| 6 | Edit a patch in `wt-b` | Key changed; `wt-b` failed to resolve; `wt-a` unaffected |
| 7 | Two source builds on the same managed checkout | The second was refused immediately with "already locked"; the owner record named the first worktree and process |
| 8 | Experiment checkout for `wt-b` while the managed build ran | Both completed; outputs under separate checkout and key directories; two runtimes in the store |
| 9 | Resolve again | Each worktree resolved its own runtime |
| 10–12 | Tamper with, then remove, the store entry | A resolve task cached on tracked files alone reported success from cache in both cases. A task with a store probe as a runtime input, and an uncached task, both failed with an integrity or missing-runtime error |
| 13 | Change toolchain identity only | Cache miss |
| 14 | Change the pinned revision, then revert | Miss, then hit |
| 15 | APFS clone of a 406 MB, 4,001-file tree | 0.59 s and about 2 MiB extra, against 1.31 s and 406 MiB for a copy; original unchanged after editing the clone; modification times preserved |
| 16 | ccache across two checkout paths | No hits by default or with a common base directory; hits with a per-checkout base directory and directory hashing off; objects byte-identical |

Two results changed the design:

- **Nx shares its local cache across all worktrees of a repository.** `wt-b` got a cache hit for a task only `wt-a` had run. Every input that distinguishes worktrees must therefore be declared, and the resolve task must not be cached. See case 10–12.
- **The prototype key included the local toolchain.** That is wrong for consumers of a prebuilt runtime. The design now separates source identity from build identity; see [repository-and-tooling.md](repository-and-tooling.md) section 5.3.

Limits of the fixture: no real Chromium checkout was involved. The M1 runs below cover that. `lockf` is macOS and BSD; Linux needs `flock`, which is still untested.

### M1 on a real Chromium checkout

**Status: verified on one machine.** Host as in the environment table, with depot_tools, Lefthook, gitleaks and the other tools installed in M1. `BDK_HOME` was `~/.bdk` on the internal disk. Commands are the `chromium:*` and `tooling/src/chromium/*` targets in the repository.

| Case | Task | Observed |
|---|---|---|
| Managed checkout at the pin | T-CHR-01 | `src` at `158.0.8127.37` (`b8c2be851f62`), synced with `--no-history --shallow`: 40 GB, of which `out/release` is 8.6 GB. depot_tools is 866 MB |
| Second `chromium:sync` of the managed checkout | T-CHR-01 | First measured at 130 s, after which the next build recompiled 44,819 files (all ccache hits, 12 to 13 minutes): gclient `--force` runs `git reset --hard HEAD` in every dependency, which recreated 466,465 files, and the sync reset `src` and re-applied the series. With both fixed (gclient gets `--force` only from `chromium:sync --force`; a checkout already at the pin plus the series is left as it is): 22 s, 9 files rewritten (hook outputs only), and the next build took 17 s, Ninja 6 s. The first sync was made by a one-off gclient script, so `chromium:sync` into an empty `BDK_HOME` is still untested |
| Patch roll in place, `158.0.8127.37` to `.65` | T-CHR-10 | 28 patch releases. 504 files changed in `src`, and V8, Skia, ICU, ANGLE, PDFium, Dawn and LevelDB moved, so widely included headers changed. The build was incremental with the same compiler: 12,868 compiles, 12,785 of them ccache misses, 83 minutes. Rolling back to `.37` with the fixed sync: 41 s and 693 files rewritten, then 12,867 compiles, all ccache hits, 8 minutes |
| `pnpm run setup` | T-REP-01 | Passed in 2.5 s: 13 tool checks, Lefthook hooks installed; runtime resolution skipped because `pin.runtime` is unset. The hooks then rejected a commit message without a type, caught a planted token with gitleaks, auto-formatted a staged file with Biome, and ran the affected lint on pre-push in 17 s |
| Cold release build of `chrome` with the series and `//bdk` | T-CHR-02, T-CHR-04, T-CHR-05 | 10,669 s wall (2 h 58 min), 57,188 actions. ccache saw 47,180 compiles, all cacheable; the cache grew to about 4 GB. The first attempt failed at `verify_chrome_framework_order` because the overlay marker was not in `chrome/app/framework.order`; the patch now adds it, and the resumed build took 45 s |
| Incremental rebuild, no change | T-CHR-05 | 17.5 s: patch check 3.2 s, overlay 0.0 s, `gn gen` 4.9 s, Ninja 6.3 s. `_bdk_overlay_marker` is exported by the framework |
| Package | T-CHR-08 | Source key `a3c82d8de4cecdf1` in 7.6 s; runtime 717 MB; a headless run rendered a test page and reported Chromium 158.0.8127.37 |
| Reproducibility check | T-CHR-09 | Re-packaging the same build gave the same manifest digest, in 27 s |
| Resolve in two worktrees | T-CHR-09 | Same store directory, same inode |
| Publish to a file backend, resolve into an empty `BDK_HOME` | T-REP-05 | Archive 198 MB (`tar.zst`), published in 10.5 s; downloaded, verified and unpacked in 2.8 s; the runtime ran. A tampered archive was refused before anything was unpacked |
| Experiment checkout by APFS clone, `out/` excluded | T-CHR-07 | 262 s and about 1 GB of extra disk. The first `git status` in the clone took 123 s while Git rehashed its index |
| ccache across the two checkouts | T-CHR-06 | Building `base` in the clone after the managed build: 2,059 of 2,059 compiles were hits. Settings: `CCACHE_NOHASHDIR=1`, no `CCACHE_BASEDIR`, and the relative paths Chromium's GN already emits. Ninja took 38 s |
| Managed and experiment builds at once | T-CHR-07 | The managed `chrome` build ran from 01:01:07 to 01:01:26 inside the experiment build's 01:00:47 to 01:03:35. Both succeeded; each wrote only its own `out/` |
| Siso state after cloning | T-CHR-07 | Not carried over: the clone excludes `out/`, where Siso keeps its state, so the first experiment build starts cold for Siso and warm for ccache |
| Roll rehearsal to `159.0.8154.6` in the experiment | T-CHR-10 | **Passed.** The series applied without conflicts; sync took 552 s once gclient was given the pinned SHA. The release build of `chrome` succeeded and `_bdk_overlay_marker` is exported by the 159 framework (source key `f754baf256329e69`). Ninja ran 18,926 s wall, of which the Mac slept about 2 h 16 min, so about 3 h of building: the same as the cold 158 build. ccache: 47,282 compiles, 11,919 hits (25%), all from an earlier attempt at 159 that was stopped part-way; nothing was reused from 158, because 159 ships a newer clang (`llvmorg-24-init-17044` against `-14453`) and `CCACHE_COMPILERCHECK=content` treats that as a new compiler. Peak memory of the build processes, sampled every 2 s: 11.1 GB, of which Siso held about 2.7 GB; most of the time it was 5 to 6 GB |

What changed because of these runs:

- **gclient fetches everything when given a tag.** It skips its own fetch only when `--revision` is a SHA it already has. With a tag it ran a full `git fetch origin` of every branch, which took over 20 minutes and twice ended in an HTTP 502, once after 82 minutes. `chromium:sync` now fetches the pinned tag at depth 1 (under 4 minutes) and passes the SHA.
- **Chrome's export list must name the overlay marker.** `verify_chrome_framework_order` rejects any exported symbol missing from `framework.order`.
- **A packaged runtime must come from a verified build.** The build writes `out/<config>/bdk-build-identity.json` only after a successful, verified build, and packaging refuses an output directory without a matching stamp.
- **A roll to a new milestone is a cold build; a roll within one is not.** ccache cannot help across a compiler change, so on this machine 158 to 159 cost about three hours of build time on top of the sync. Even with the same compiler most of the build would recompile: 24,484 of 143,491 C and C++ files changed between the two tags, 8,164 of them headers, 197 of those in `base/`, abseil or mojo. Within a release branch the compiler is fixed: `tools/clang/scripts/update.py` names the same `CLANG_REVISION` at `158.0.8127.0`, `.20`, `.37` and `.52`, and at `157.0.8103.0` and `.40`. So patch rolls are incremental, though not necessarily small: `.37` to `.65` recompiled 12,785 files in 83 minutes because several dependencies moved. A milestone roll could be made incremental on the day by building the new branch's beta tag ahead of time; that is not automated. Risk R3's roll effort should assume one cold build per milestone.
- **APFS clones stop sharing once the tree moves.** After the roll to 159, the clone's checkout had diverged and free disk fell from 52 to 30 GiB. An experiment that changes the tag costs nearly a full checkout.

Still not verified: a second machine resolving the published runtime, the integration workflow against it (`pin.runtime` is still unset, because nothing was published to GitHub), the GitHub-hosted checks, depot_tools' cache locking (the Git cache is not used with shallow syncs), and Linux.

---

## 10. Arc-style reference scenario

**Status: partially verified. Scoped, not prototyped.**

The scenario: an application-owned sidebar, two spaces, a split view, and moving a WebContents between spaces and windows, with services and extensions behaving correctly.

What the evidence supports:

| Requirement | Assessment | Basis |
|---|---|---|
| Custom native shell with no Chrome UI | Supported by the recommended strategy | Investigations 1 and 2; Arc precedent |
| Several pages visible at once | Hosting works. Service semantics are the problem; see below | Investigation 2 |
| Move, hide, reparent, restore | Supported if moves go through the tab model | `tab_strip_model.h:296-348`; `tab_interface.h:185-213` |
| Extension context | Works for real `Browser` objects | Investigation 3 |
| Session restoration controlled by the application | Needs BDK to keep Chrome's session service from restoring windows on its own | `session_service.h:144`; not designed in detail yet |

### Coupling that limits Arc-style applications such as Cove

**1. One active tab per window.** Chromium treats one tab per `Browser` as active. Translate UI, the find bar and JavaScript dialog foregrounding gate on it (`chrome_translate_client.cc:621-631`; `find_bar_controller.cc:48-65`; `javascript_tab_modal_dialog_manager_delegate_desktop.cc:66-99`). In a three-pane layout, two panes are background tabs as far as those services are concerned. Extensions see the same thing through `tabs.query({active: true})`.

**2. Upstream split view is two panes.** The data model has split tabs and treats the split's members as foreground (`tab_strip_model.cc:1215-1224`). The Views implementation shows at most two (`multi_contents_view.h:49`). Whether the data model itself accepts more is unverified.

**3. Spaces have no upstream equivalent.** Tabs in a hidden space are either background tabs in the same `Browser`, in which case extensions list them, or tabs in another `Browser`, in which case moving between spaces fires window attach and detach events.

Proposed handling, in order of preference:

1. Map the focused pane to the active tab. Represent a two-pane split as an upstream split so both are foreground.
2. For more panes, patch the foreground predicate so that any tab BDK reports as presented counts, at the handful of call sites that gate on the active tab. The number of sites is unverified.
3. If that proves fragile, back each visible pane with its own `Browser`. Every pane is then active, at the cost of extensions seeing more windows.

This is a property of the reference application, not of BDK, which has no space type. What BDK has to decide is generic: whether an off-screen page is always a background tab of its last window, or whether the application may optionally keep pages in a separate `Browser`. That is left to the prototype, because it changes what extensions observe, and is decision D1 in [risks.md](risks.md).

### Smallest follow-up

Milestone M0's last step: on the first working build, a throwaway Swift window with a sidebar, two spaces and a two-pane split. Run the MV3 probe and record exactly what `tabs.query`, `tabs.onActivated`, `windows.getAll` and the attach and detach events report under each of the two mappings for off-screen pages.

---

## Open items carried into the plan

| Item | Where it is handled |
|---|---|
| First real build: sizes, times, ccache behaviour | Done in M1; see section 9 |
| BDK window surviving `//chrome` without `BrowserView` | M0, T-ADP-01 to T-ADP-04 |
| App-owned `NSApplication` with `//chrome` | M0, T-MAC-01 to T-MAC-03 |
| Alias scheme end to end | M0, T-WUI-01 |
| MV3 probe under native hosting | M0, T-EXT-01 |
| Component update mirror and the signed update check | T-PRV-06 |
| Nested-helper signing and notarization | T-PKG-03 |
| Active-tab gating with more than two panes | T-ADP-06, risk R2 |
| Reputation provider and its cost | Decided: static block lists, decision D2 in [risks.md](risks.md) |
