# BDK roadmap

Status: plan, 2026-11-14. Milestones are ordered by what could invalidate the architecture, not by developer convenience. Task identifiers refer to [tasks.md](tasks.md). Sizes there are relative effort, not delivery dates.

## 1. Order and dependencies

```text
M0  Chromium integration prototype ──► gate G0 (go / change strategy)
 │
 ├─► M1  Repository foundation and runtime pipeline
 │     │
 │     └─► M2  Minimal macOS runtime and Swift SDK
 │             │
 │             ├─► M3  Browser services and default UI
 │             │      │
 │             │      ├─► M4  Internal pages and privileged SDK
 │             │      ├─► M5  Extensions                         (public v1 gate)
 │             │      └─► M6  Providers and privacy              (public v1 gate)
 │             │
 │             └─► M7  Automation host and DevTools  (needs the broker from M3)
 │
 M4, M5 ─► M8  Arc-style reference application                  (public v1 gate)
 M3–M8  ─► M9  Release hardening ──► public v1
 M9     ─► M10 Windows feasibility
```

M4, M5, M6 and M7 can proceed in parallel once M3's broker and UI-request router exist. M1 may start while M0 is running, but M1 work must not delay M0.

| Milestone | Proves | Depends on |
|---|---|---|
| M0 | The architecture's five riskiest assumptions, on a real build | Nothing |
| M1 | The repository, quality gates and runtime pipeline work as designed on real Chromium | M0 build baseline |
| M2 | A Swift application hosts pages through the C ABI using a prebuilt runtime | M0, M1 |
| M3 | Chrome's services work behind the broker, and every browser-owned surface has a default | M2 |
| M4 | Branded routes, bundled pages and per-page capabilities | M3 |
| M5 | Extensions run with default and custom UI | M3 |
| M6 | Translation, reputation and geolocation without Google; no Google-directed traffic | M3 |
| M7 | Trusted automation independent of pages | M3 |
| M8 | A Cove-like application works without compromising services or extensions | M4, M5 |
| M9 | The runtime can be shipped, upgraded and patched | M3 to M8 |
| M10 | The shared core holds for a second platform | M9 |

## 2. Milestones

### M0 — Chromium integration prototype

> **Status: skipped (decided 2026-11-18, by the product owner).** M0 was not run. Gate G0 is therefore not passed, and the assumptions below are untested. M2 starts without G0. Consequences: the MV3 probe, the BDK window replacing `BrowserView`, the app-owned `NSApplication`, the alias scheme and the two-window and off-screen decision (D1, still provisional) are first exercised during M2 and M3 instead of before M2. If one of them fails, the change in the table under G0 applies at that point, and M2 work that depended on it may need rework.

Throwaway code is acceptable. The output is evidence and a decision, not a product. Use the newest stable tag; the evidence pin is `158.0.8127.37`.

| Step | Work | Tasks | Acceptance |
|---|---|---|---|
| M0.1 | Managed checkout and an unmodified Chromium build at the pin | T-CHR-01, T-CHR-02, T-CHR-05 | Chromium runs. Checkout size, output size, full and incremental build times and packaged size are recorded in `feasibility.md`. |
| M0.2 | A Swift application owns `NSApplication` and hosts one page | T-MAC-01, T-MAC-02 | A SwiftUI window shows a page from the `//chrome` browser process. No Chrome window, menu or delegate appears. Typing, input methods and command keys work. |
| M0.3 | The BDK window replaces `BrowserView` | T-ADP-01, T-ADP-02, T-ADP-03 | Chromium's browser tests for session restore, permissions, print preview, translate, find, DevTools and the tabs API run against the BDK window. Every crash, null cast and unimplemented call is listed. |
| M0.4 | Services work | T-ADP-04, T-MAC-04 | With no Chrome UI: a visit is recorded in history, a bookmark is saved, a download completes, a permission prompt can be answered, print preview opens, a JavaScript dialog shows. Whether Chromium's dialog views can be used on the native window is recorded. |
| M0.5 | Extensions work | T-EXT-01 | The MV3 probe loads unpacked. The service worker runs, messaging works in both directions, the popup opens in a BDK host, `activeTab` is granted on click and revoked on navigation. |
| M0.6 | Branded route and privileged call | T-WUI-01 | One binary launched with two different scheme names serves a bundled page under each without rebuilding. A page with a granted capability calls a native command; a page without it, a subframe and a web page are refused. |
| M0.7 | Two windows, split, spaces | T-ADP-08, T-ARC-01 | A page moves between two windows and keeps its state, tab identifier and renderer. The probe's reports under both host-context mappings for off-screen pages are recorded for decision D1. |
| M0.8 | Embedding under another identity | T-PKG-01 | The built runtime runs inside a second application with a different bundle identifier and signing identity. |

**Gate G0.** Proceed if M0.2 to M0.6 pass. D3 is decided. Before M2, confirm or change the provisional decision D1 from [risks.md](risks.md), using the M0.7 record.

If a step fails:

| Failure | Change |
|---|---|
| The BDK window cannot be made stable (M0.3, M0.4) | Evaluate a hidden `BrowserView` per window with BDK-managed content views. This affects split view and must be raised as an architectural issue before continuing. |
| An application-owned `NSApplication` is not workable (M0.2) | Try an external message pump. If that fails, BDK must own the application object and expose hooks, which changes the Swift integration story. |
| The alias scheme is not workable (M0.6) | Branded routes become display-only: the application maps names to `chrome://` URLs. |
| Extensions fail under native hosting (M0.5) | Stop. Extension support is a v1 requirement. |

### M1 — Repository foundation and runtime pipeline

| Work | Tasks | Acceptance |
|---|---|---|
| Root workspace, project graph, quality gates | T-REP-01, T-REP-02 | `pnpm install`, `pnpm lint`, `pnpm test`, `pnpm build` run from a clean clone with no Chromium checkout. Lefthook installs. |
| Chromium project targets | T-CHR-03, T-CHR-04, T-CHR-07 | `pnpm chromium:sync`, `chromium:build` and `chromium:package` work under the checkout lock. The patch series applies with no build in under a minute. An experiment checkout builds concurrently with the managed one. |
| Runtime store and manifest | T-CHR-08, T-CHR-09 | Two worktrees resolve one immutable runtime. A changed patch changes the key. A tampered or missing runtime fails resolution. |
| Compiler cache | T-CHR-06 | Hit rate across two checkouts measured and recorded. |
| ABI skeleton and stub runtime | T-ABI-01, T-ABI-02, T-ABI-05 | The conformance suite passes against the stub on macOS and Linux. |
| CI | T-REP-03, T-REP-05 | Pull-request checks run on hosted runners. A runtime built on a developer's Mac is published with a manifest and resolves in CI. No Chromium build runs in CI. |
| Rebase rehearsal | T-CHR-10 | The patch series is moved to the next upstream tag once, and the effort is recorded. |
| Chromium roll bot | T-CHR-11 | A newer upstream stable produces one roll pull request that states whether the patch series applies. |
| Versioning | T-REP-07 | Pull-request titles follow Conventional Commits. |

### M2 — Minimal macOS runtime and Swift SDK

| Work | Tasks | Acceptance |
|---|---|---|
| Host functions in the engine | T-ABI-03, T-ABI-04, T-ADP-05, T-ADP-07 | Runtime start and stop, profiles including ephemeral ones, WebContents lifecycle, navigation, events, new-window requests. |
| Swift SDK | T-SWF-01 to T-SWF-03 | A Swift application creates profiles and pages, attaches them to its own views, and observes navigation. No BDK tab or window type is required. |
| Host view behaviour | T-MAC-03, T-MAC-05 | Attach, hide, reparent, focus, keyboard, accessibility parent, fullscreen and context-menu requests. Verified with VoiceOver and an input method. |
| Configuration and the CLI executable | T-CLI-05, T-CFG-01, T-CFG-02 | `bdk.toml` validates against the schema; invalid fixtures fail with specific messages; the resolved file is produced by a `bdk` executable that runs with no Node installed. |
| System codecs | T-CHR-12 | The spike for decision D7 is recorded: whether H.264, AAC and HEVC play through the system decoders with no bundled proprietary decoder. Not an exit criterion for M2. |
| Prebuilt runtime | T-PKG-02, T-PKG-03, T-PKG-04 | The runtime is published with a manifest and a recorded digest, resolved through SwiftPM, and signed inside an application with another team identifier. Notarization is part of the release procedure (T-PKG-06, M9), not M2 (product owner, 2026-11-19). |
| Example | T-SWF-05 | `pnpm dev:example` launches the minimal browser with two pages visible. |

Exit criteria: the multi-page sample survives a renderer crash and an application relaunch. Private profiles keep cookies, cache and permissions separate.

**Status, 2026-11-19: the exit criteria are met on Chromium 158; publishing and the codec build remain.** M0 was skipped at the product owner's request (2026-11-19): the engine was built straight onto Chromium's `WebContents`, without the `Browser`-based host contexts of M0 (T-ADP-01 to T-ADP-03). [amendments.md](amendments.md) section 2.12 records that departure and what it leaves for M3. Everything below was run against the packaged runtime (`pnpm chromium:package`, `chromium:runtime`); nothing in CI builds Chromium.

| Task | Status | Where it is recorded |
|---|---|---|
| T-ABI-03 | Done. Contents events gained `RENDERER_GONE` and `VISIBILITY_CHANGED`. | `packages/core/docs/conventions.md`; [sdk/c-abi.md](sdk/c-abi.md) section 7 |
| T-ABI-04 | Done: the engine (`chromium/bdk`) is the Chromium backend of the runtime core the stub also uses. The conformance suite passes against it (121 tests) together with 15 engine scenarios: 136 of 136 (`nx run core:test-engine`). | `chromium/bdk/README.md`; `packages/core/docs/conventions.md` |
| T-MAC-01, T-MAC-02 (M0, done in M2's form) | Done: `//chrome` starts inside an application-owned `NSApplication` (`BDKApplicationBase` subclasses `BrowserCrApplication`), no Chrome window, menu or delegate, clean termination. Patches 2 to 4 of the series. | `chromium/bdk/README.md`; [feasibility.md](feasibility.md) section 2 |
| T-ADP-05 | Done: `window.open` (kind `popup`), new-page dispositions and "open link in new page" (`newPage`), and undocked DevTools from the context menu's Inspect (`devTools`) each raise one new-window request carrying the new page. A rejected or unhandled one is destroyed: the default presenter is T-UIR-03 (M3). Picture-in-picture is not routed. | engine scenarios in `packages/core/tests/tests_engine.c` |
| T-ADP-07 | Done: Chromium restores nothing at start (`--no-startup-window`); a page's state is Chromium's own navigation entries in a BDK envelope, restored into a new page with its history. | engine scenario; minimal-browser relaunch test |
| T-MAC-03 | Done: the host view keeps the page through restore and discard; visibility events on hide and show; focus events and state; unhandled shortcuts reach the application with the `NSEvent`, reserved keys never reach the page; VoiceOver's activation (`AXEnhancedUserInterface`) exposes the page's `AXWebArea`, heading and text. The input-method composition step needs a key window, which a terminal-launched harness cannot get, so it is skipped there with a message; typing into a field is checked. Not verified by a person with VoiceOver or an input method. | engine scenarios |
| T-MAC-05 | Done: HTML fullscreen enters and exits through fullscreen requests (default presenter: the window goes fullscreen); a denial leaves the page not fullscreen (content has no asynchronous refusal); context-menu requests carry the context and items, `perform` runs one, the default presenter is an `NSMenu`. | engine scenarios |
| T-SWF-01 to T-SWF-03 | Done against the stub and the real runtime. `BDKApplication` subclasses the runtime's `BDKApplicationBase`; `terminate(replyingTo:)` cancels and re-terminates instead of `.terminateLater`. The Swift UI-request API is T-SWF-04 (M3). | `packages/swift/README.md` |
| T-CFG-01, T-CFG-02, T-CLI-05 | Done (unchanged). | as before |
| T-PKG-02 | Done: data directory, Keychain item and product name come from `bdk.resolved.json` (a new patch makes the Keychain name settable). A real run created the item "Minimal Browser Safe Storage" and left "Chromium Safe Storage" alone. | `chromium/bdk/README.md` |
| T-PKG-03 | Done for M2: signing, ad hoc; `bdk build sign` signs the example with its embedded runtime. A real Developer ID identity and notarization moved to the release procedure (T-PKG-06), at the product owner's decision. | [sdk/cli.md](sdk/cli.md) section 5.1 |
| T-PKG-04 | Partly, unchanged: the https download path needs a published runtime. Publishing (`chromium:publish`) is an outward action left to the product owner. | [feasibility.md](feasibility.md) section 7 |
| T-SWF-05 | Done on the real runtime: `pnpm dev:example` shows two real pages. | `apps/examples/minimal-browser/README.md` |
| T-CHR-12 | Spike recorded from measurement and source; the build that would test system decoders has not run. | [feasibility.md](feasibility.md) section 7.1 |
| Exit criterion: renderer crash | Met: `chrome://crash` kills a page's renderer, the application gets `rendererGone`, reloads it, the other page is untouched. | minimal-browser end-to-end test |
| Exit criterion: relaunch | Met: a relaunch restores both pages with Chromium's own history. | minimal-browser end-to-end test |
| Exit criterion: private profiles | Met for cookies, local storage and the HTTP cache (with the shared profile as control). Permissions are kept apart by construction (each private profile is its own off-the-record profile with its own content settings); no test grants one, because BDK has no permission prompt before M3 (T-UIR-02). | minimal-browser end-to-end tests |

### M3 — Browser services and default UI

| Work | Tasks | Acceptance |
|---|---|---|
| Broker | T-SVC-01, T-SVC-05, T-SVC-08 | Commands are defined once, checked per caller, cancellable. Application commands register from Swift. |
| Services | T-SVC-02, T-SVC-03, T-SVC-04, T-SVC-06, T-SVC-07 | History, bookmarks with folders and moves, downloads, site settings, cookies, permissions, passwords and autofill with Keychain storage, print to PDF, screenshots, default-browser registration. |
| UI requests and defaults | T-UIR-01 to T-UIR-06, T-SWF-04, T-SWF-07 | Every surface works with no handler registered. Each can be replaced from Swift. Handled, use-default and reject behave as specified. |
| Security flows | T-UIR-05 | Certificate and reputation interstitials cannot be disabled or bypassed. A permission cannot be granted without a user gesture. |

Exit criteria: a browser built with no custom UI code is usable for everyday browsing, including passwords, autofill, downloads, printing and permission prompts.

**Status, 2026-11-20: the exit criteria are met on Chromium 158, by the engine scenarios; nobody has browsed with the minimal browser by hand.** Every page is now a tab of a `Browser` host context ([amendments.md](amendments.md) section 2.13), which M2 had left out (section 2.12). The engine suite was 279 of 279 in one process on an unlocked screen (`nx run core:test-engine`). After T-SVC-07, T-SVC-08 and T-SWF-07 joined M3 it is 300 tests, run with the screen locked: 296 pass, and the 4 that fail need an unlocked screen and a key window (HTML fullscreen twice, the accessibility tree, page capture), as before. The stub suites, Swift (133 tests) and codegen pass. The patch series is 14 patches; each message says why.

| Task | Status | Where it is recorded |
|---|---|---|
| T-ADP-01 to T-ADP-03 | Done: a BDK `BrowserWindow` (no `BrowserView`), a parking context per profile and a context per window and profile; attach, move, detach, window close, focus and destroy go through the tab model, and a move keeps the tab identifier, history and renderer. The `browser_tests` filters of T-ADP-02 were not run (no test build): T-ADP-09, M9. | `chromium/bdk/README.md`; amendments 2.13 |
| T-ADP-04 | Done as engine scenarios rather than a one-off table: a visit is recorded in history, a bookmark saved, a download completes, a permission prompt is answered, a JavaScript dialog shows, find works. Print preview is not hosted (it needs a parent widget); printing uses the system print panel: T-UIR-08, M4. | `packages/core/tests/tests_engine*.c`; [feasibility.md](feasibility.md) section 11 |
| T-SVC-01, T-SVC-05 | Done: 86 commands and 26 events defined once in `packages/core/interface/`, checked in the order of services.md section 2, cancellable, with progress and a 4 MiB limit; application commands and events from Swift. Page and session callers exist inside the core for M4 and M7. | `packages/core/docs/conventions.md` (the broker) |
| T-SVC-02, T-SVC-03, T-SVC-06 | Done on Chromium's services, with events: history, bookmarks (folders, moves, import and export as HTML), downloads, settings (16 preferences and the content defaults), site settings, cookies (with `cookies.changed`), certificate exceptions, clearing data, profiles, default-browser status (`bookmarks.import`/`export`, the certificate-exception commands and `cookies.changed` came with T-SVC-07). Screenshots and PDFs work in the engine but have no command until `contents.*`: T-SVC-09, M7. | [sdk/services.md](sdk/services.md) section 8A |
| T-SVC-04 | Done: passwords and autofill commands and their events (`passwords.changed`, `passwords.issuesChanged`, `autofill.changed`, T-SVC-07); reveal and password changes ask for Touch ID or the login password; stored passwords survive a relaunch under the application's Keychain item. No scenario fires `passwords.issuesChanged`: nothing flags a login as insecure before the leak-check provider (S16, M6). `autofill.changed` carries no entry id, because Chromium's data managers do not say which entry changed. | services.md 5.9, 5.10 |
| T-UIR-01, T-SWF-04 | Done: per-surface `default`, `custom` and `disabled` from the resolved configuration; a custom surface with no handler fails the freeze; requests on one surface and page reach the handler one at a time; typed Swift requests for 24 surfaces. | conventions.md (the UI request router); `packages/swift/README.md` |
| T-UIR-02 | Done: permissions (persisted per profile, private profiles apart), JavaScript dialogs, file pickers. HTTP authentication is built but has no end-to-end test: scripted navigations cannot answer 401 (T-UIR-07, M4). | [sdk/ui-requests.md](sdk/ui-requests.md) 8.1 |
| T-UIR-03 | Done: where to save and dangerous downloads; context menus and new windows from M2. | ui-requests.md |
| T-UIR-04 | Done: the password save and update prompt (asked when a page finishes a navigation with a question pending), autofill save prompts and the suggestion popup, `window.print()`. The leaked-password request waits for the leak-check provider (S16, M6). The card autofill prompt has no scenario (T-UIR-07, M4). | ui-requests.md 5.7 to 5.9 |
| T-UIR-05 | Done: certificate and Safe Browsing interstitials only go back or proceed; disabling fails at start; reject, drop and release go back; proceed needs a gesture and an overridable error. Tested with a real TLS server with an untrusted certificate. The non-overridable case (HSTS, pinning) is not tested end to end (T-UIR-07, M4). | ui-requests.md; `tests_engine_prompts.c` |
| T-UIR-06 | Done: find (started by the application too: `page.find` and `page.print()`, T-SWF-07, done in M3), fullscreen, notifications (in-window banner: the system notification centre needs entitlements). | ui-requests.md 8.1 |
| T-MAC-04 (M0, settled here) | Default presenters are AppKit, not Chromium's views, which need a Views widget to anchor to. Decided from the source and from what the engine needed, not from a spike. | feasibility.md section 11 |
| Exit criterion | Met by the scenarios with no handler registered for passwords, autofill, downloads, printing and permission prompts. Not checked by hand in `apps/examples/minimal-browser` (T-UIR-07, M4). | engine suite |

Typed application settings (`BDKSettingKey`, `config.settings.register`, T-SVC-08) and `page.find` and `page.print()` (T-SWF-07) are done in M3. Swift API from swift-api.md that M3 did not build, each now a task: page commands, setting zoom and mute, leaving picture-in-picture, all left over from M2 (T-SWF-06, M4); address-bar services, `profile.search` (S1, T-SVC-10, M4); `page.capture` (T-SVC-09, M7); public window registration and anchors (S11, T-SWF-08, M5).

Also fixed in M3: a persistent profile stayed loaded in Chromium after the application closed it, keeping about 87 file descriptors; it is now unloaded, and reopening waits for Chromium to finish destroying it.

### M4 — Internal pages and privileged SDK

| Work | Tasks | Acceptance |
|---|---|---|
| Route registry and alias | T-WUI-02, T-WUI-05, T-WUI-06 | Logical routes resolve by the documented precedence. Collisions fail the build. Disabled pages show an error. Settings and extension management behave like any other page. Branded URLs display correctly in history and session data. |
| Bundled pages | T-WUI-03, T-CLI-04 | `Pages/Settings` is discovered, built with the Vite preset and served under the standard content security policy. |
| Capability broker | T-WUI-04 | A granted page reads and writes settings through `@bdk/internal`; Swift sees the same values. An ungranted page, a subframe, an extension and a web page are refused. A wrong-page interface request terminates the renderer. An alias carries its target's grants. |
| SDK and generation | T-JS-01, T-JS-02, T-JS-03 | Declarations are generated locally from interface definitions and registered commands. Nothing is published to npm. |
| Left over from M2 and M3 | T-SVC-10, T-SWF-06, T-UIR-07, T-UIR-08 | Page commands, zoom, mute and address-bar services work from Swift. The M3 scenarios left open pass, print preview is hosted or the system panel is recorded as the default, and the M3 exit criterion is checked by hand. (T-SVC-07, T-SVC-08 and T-SWF-07 were planned here and done in M3.) |

**Status, 2026-11-21: built and tested on Chromium 158; the engine suite is 342 of 343 on an unlocked screen.** The engine was built incrementally on the managed checkout (release, 16 patches). With the screen locked it was 338 of 343: the 4 M3 scenarios that need an unlocked screen and a key window (HTML fullscreen twice, the accessibility tree, page capture) failed, and pass unlocked. The one failure left is `engine.non_overridable_certificate_error`: on `hsts.localhost` Chromium raises no interstitial at all, so the non-overridable case is still not tested end to end (T-UIR-07 stays open for it). The engine build needed four fixes: a `std::string_view` conversion; an unknown route's `ERR_INVALID_URL` is a `CANCEL` (`BLOCK_REQUEST` takes only a blocked error); internal pages cannot be saved or viewed as source, as in the stub; and a new patch, because `chrome://policy` reads the initial browser command line that only `ChromeMain()` recorded. A branded URL is now reported in its canonical form (`conf://flags/`), as Chromium reports it. What runs without Chromium passes: the stub conformance suite (219 of 219, 30 new), Swift (148, 14 new), `@bdk/protocol` (10), `@bdk/internal` (82), `js-codegen` (156), `@bdk/vite` (21), the CLI (304, 2 opt-in tests skipped), tooling (135), lint and format, `codegen-check` and `chromium:patch-check` (16 patches). The decisions are in [amendments.md](amendments.md) section 2.14.

| Task | Status | Where it is recorded |
|---|---|---|
| T-WUI-02, T-WUI-05 | Done in the core against the stub: the route table from `bdk.resolved.json`, Swift registrations before the freeze (`config.noBuiltin`, `routeUnknown`, `routeInvalid`, `routeMode`, `handlerMissing`), disabled and unknown routes show the error page, an overridden or disabled built-in has no internal way in, native routes reach the Swift handler. Engine (URL handler, navigation throttle): built, and the page scenarios pass on it. T-WUI-01 (M0) is covered here: the alias scheme comes from the configuration at start and is registered in every process by a new patch. | `packages/core/runtime/routes.h`; conventions.md decision 15 |
| T-WUI-06 | Done in the core and the stub; the engine maps every URL it reports: built and passing. Internal pages are not recorded in history, as in Chrome; an alias is shown as its target route. | amendments 2.14 |
| T-WUI-03 | `@bdk/vite` and `bdk build pages` done and tested (Vite 8.3.8 run in process from the project's `node_modules` by the packaged `bdk` with no Node on the path). The data source in the engine: built; the content security policy scenario passes. | `packages/js/vite/README.md`; `packages/cli/README.md` |
| T-WUI-04 | Page callers, grants (an alias carries its target's), revocation on navigation and page subscriptions done in the core and tested on the stub. The Mojo interface, its binder and per-call checks: built; the scenarios in `tests_engine_pages.c` pass on the engine, including the renderer of a wrong page being terminated. | `chromium/bdk/README.md` (`pages/`) |
| T-JS-01 to T-JS-03 | Done: `@bdk/protocol`, `@bdk/internal` (one generated module per namespace with page callers, a mock transport), the TypeScript generators and the per-application declarations. Nothing is published. | `packages/js/*/README.md` |
| T-CLI-04 | Done except Xcode itself: `bdk build codegen`, `bdk build pages`, the default `bdk build`, Xcode file lists and the template's build phases. Not run from Xcode; `--watch` and the declaration extractor: T-CLI-06, M5. | `packages/cli/README.md`; `apps/templates/swift-browser/README.md` |
| T-SVC-10 | Commands defined, done in the stub; the engine service on `TemplateURLService` and the omnibox: built and passing. First-run engines from `[search]` and suggest off by default: T-PRV-10, M6. | services.md 5.13 |
| T-SWF-06 | Done against the stub and on the engine. | swift-api.md 6.7 and 9 |
| T-UIR-07 | HTTP authentication against a loopback 401 server and the card save prompt pass on the engine. The non-overridable error does not: Chromium raises no interstitial for `hsts.localhost` (it ignores HSTS headers for `*.localhost`, and the scenario's direct `AddHSTS` gives a fatal error with no interstitial), so it needs a host that is not `localhost` mapped to loopback. The M3 exit criterion has not been checked by hand. | `tests_engine_pages.c` |
| T-UIR-08 | Decided: the system print panel is the permanent default; print preview is not hosted. | amendments 2.14 |
| Exit criteria | Met on the engine by the scenarios, except the hand check of the M3 exit criterion and the non-overridable certificate case (T-UIR-07). The minimal example has a bundled `minimal://settings` page that reads and writes `privacy.doNotTrack`. | `apps/examples/minimal-browser/README.md` |

Not done, each a task: the non-overridable certificate scenario and the hand check (T-UIR-07); `bdk build pages --watch`, the Swift declaration extractor and Xcode verification (T-CLI-06, M5); a settings key catalogue for typed keys and event filters for pages (T-JS-05, M5); `navigation.handledNatively` for native routes (T-SWF-09, M5); `favicons`, `topSites` and their `@bdk/internal` modules (S12, T-SVC-11, M5); the Google-service audit of the built-in pages (T-WUI-07, M6); search defaults with no Google (T-PRV-10, M6). `@bdk/internal/extensions` and `translation` arrive with their namespaces (T-EXT-02, M5; T-PRV-03, M6).

### M5 — Extensions (public v1 gate)

| Work | Tasks | Acceptance |
|---|---|---|
| Action, popup, context menu, options | T-EXT-02, T-EXT-04 | With no custom code, a popup opens from BDK's default extensions menu. With a Swift handler, the same popup appears in the application's layout with focus and close behaviour intact. |
| Side panel | T-EXT-03 | `chrome.sidePanel` opens in the default host and in an application-supplied container. |
| Installation | T-EXT-05, T-EXT-09 | Unpacked, CRX and self-hosted update sources work with Chromium's install prompt. Sources are configurable. The store source follows decision D4. |
| Compatibility checks | T-EXT-06 | A manifest requiring a disabled surface is rejected with a reason, at install and at update. A disabled API is absent, not silently accepted. |
| Suite and matrix | T-EXT-07, T-EXT-08 | The probe suite and a set of real extensions run in CI. The matrix is generated from results and lists known gaps. |
| Window registration | T-SWF-08 | Extensions see the window kind the application registered; default presenters attach to the application's anchors. |
| Probe (owed by M0) | T-EXT-01 | The assertions for T-EXT-01 pass on the engine. |
| Left open by this milestone | T-EXT-10, T-EXT-11, T-EXT-12, T-EXT-13 | The matrix's remaining scenarios and the real extensions run; `extensions.inspectView`; new pages opened by extension UI with no handler; the store's update check when the store is off. |
| Left over from M4 | T-CLI-06, T-JS-05, T-SWF-09, T-SVC-11 | `bdk build pages --watch` and the declaration extractor work in an Xcode build; setting keys are typed and page subscriptions filtered; a native route answers navigation; favicons and top sites once S12 is confirmed. |

Exit criteria: representative real extensions, including a content blocker, a password manager and a developer tool, install and work. Every known difference from Chrome is documented.

**Status, 2026-11-21: built on Chromium 158; the engine suite runs, and the extension scenarios do not all pass yet, so no exit criterion is met.** The main session built M5 incrementally on the managed checkout (release, 17 patches). The build needed compile fixes in 11 files and 11 functions added to the C ABI export patch; the engine suite needed the core to unregister every window at shutdown (the window registry and host contexts pointed at freed windows, which raw_ptr's dangling check aborts on) and test fixes (a fixtures path with `..`, buffers too small for an icon data URL). Results, on an unlocked screen, the existing suite in one launch and each of the 59 new scenarios in its own (a failed REQUIRE, a crash or a hang ends a launch): the existing suite 330 of 331 (`ext_activation_needs_a_gesture_and_an_extension`: the engine's error is not `extension.notFound`); of the new scenarios 40 pass, 16 fail and 3 hang. Install 15 of 19: a self-hosted update to 2.0 is never applied and `extensions.updated` never fires; the permission-increase update raises no request; both store-button scenarios crash Chromium (`google_url_loader_throttle.cc`: a response served by `extensions.serve` has no parsed headers). UI 12 of 17: activation grants on the wrong page, a handled popup does not grow, navigation does not close a popup; the disabled popup and context-menu surface scenarios hang. Panels 8 of 15: the parking context is visible to `windows.getAll`, `windows.update` does not minimize, `windows.remove` and a page panel's second panel misbehave, the disabled side-panel surface scenario hangs, and two scenarios crash on a freed `BDKSidePanelContainerView` (a use after free, found by Chromium's Objective-C zombie check). Probe 5 of 8 (`onActivated`, the parking context, the action's `activeTab`). With the screen locked, in groups, it was 326 of 331 and 30 of 59 with 18 not run. Separately, a race in Chromium's heavy-ad opt-out store (its database loads at best-effort priority and fails after the test has deleted the profile) is a fatal DCHECK in the existing suite with M5's slower profile start; the runs above kept that store in memory (`HeavyAdPrivacyMitigations:OptOutStoreDisabled`), and the fix for applications is open. Before the engine build: the stub conformance suite (216 tests, 27 of them new for extensions and windows), Swift (163 tests, 29 new), codegen, the CLI tests, the extension suite's own tests and `chromium:patch-check` (17 patches, 3 new). Decisions taken where the documents were silent are in [amendments.md](amendments.md) section 2.17 and `packages/core/docs/conventions.md` ("Extensions (M5)").

| Task | Status | Where it is recorded |
|---|---|---|
| T-EXT-02 | Written, engine build and test pending: action state (`extensions.actions`, `extensions.actionChanged`), activation and declared commands from the host with the confirming event (`bdk_extensions.h`), the window's `ExtensionsContainer` (Chromium's cross-platform toolbar model) so `action.openPopup` works, popups as open `extension_popups` requests with a secure view and size updates, the default menu (`bdk_extension_menu_present`, and a title-bar accessory button when actions are `default` and the window has no anchor provider). Stub and Swift: done. | `chromium/bdk/README.md`; extensions.md 4.1, 4.2, 4.7 |
| T-EXT-03 | Written, engine build and test pending, with no patch: `chrome.sidePanel` reaches a BDK `SidePanelUI` for every host context; the panel is an open `extension_side_panels` request, BDK's frame at the trailing edge by default, and `getLayout()` reports the side the application gives. | `chromium/bdk/README.md`; extensions.md 4.3 |
| T-EXT-04 | Written, engine build and test pending: extension items in the context-menu request (`source`, `extensionId`, one submenu per extension), performing one grants `activeTab`; options pages as `newPage` requests naming the extension (one patch: Chromium opens them as a tab of the hidden host context). | extensions.md 4.4, 4.5 |
| T-EXT-05 | Written, engine build and test pending: unpacked, local CRX (BDK owns the off-store policy) and self-hosted update URLs, each refused by source with `extension.sourceNotAllowed` naming the key; every install, re-enable and permission request is an `extension_installs` request (one patch: Chromium's install and uninstall dialogs are Views). | extensions.md 4.6, 5 |
| T-EXT-06 | Done in the shared core (`runtime/extension_policy`), tested on the stub; the engine's use of it at install and before an update is written, engine build and test pending. With `allow_all`, the APIs of disabled surfaces are removed from the granted permissions so they are absent. | extensions.md 6 |
| T-EXT-07 | Done without Chromium: `tests/extensions` (the fixed probe, fixtures for a content blocker, a password manager and a developer tool, both incognito modes), `matrix.json` and the generated [compatibility matrix](../extensions/compatibility-matrix.md), committed in its "not run" state; `extensions-suite:test-extensions` runs the engine's scenarios and regenerates it. The real extensions are listed with no version yet and have not been fetched (T-EXT-10). | `tests/extensions/README.md` |
| T-EXT-08 | Written, engine build and test pending: Chrome's bindings add `devtools_page` panels to BDK's undocked front end; `[extensions.ui] devtools = "disabled"` blocks them. A replaced front end loads none (the matrix says so). | extensions.md 4.8 |
| T-EXT-09 | Written, engine build and test pending: `chrome-web-store` installs only when listed in `stores`; when listed, a script on the store's pages relabels "Add to Chrome" as "Add to <browser name>" and leaves the page alone if it finds no such button. The store's own update check may still be sent when the store is off (T-EXT-13). | extensions.md 5 |
| T-SWF-08 | Done against the stub (Swift `runtime.windows.register`, `BDKWindow`, anchors, delegate, `page.window`); the engine's window kinds, attachments, anchors and window requests are written, engine build and test pending. The parking context is hidden from the extension windows API by a patch. | swift-api.md 8.1; `packages/swift/README.md` |
| T-EXT-01 | Written, engine build and test pending: assertions 1 to 5, 7 and 10, and decision D1 (a), in `tests_engine_ext_probe.c`; 6, 8 and 9 are the popup, context-menu and side-panel scenarios. Settled from the source while writing it: Chromium lists the parking context in `windows.getAll` and fires `windows.onCreated` for it, so it is hidden. | [feasibility.md](feasibility.md) section 3 |

Not done, each a task: the 25 matrix scenarios with no engine test yet and the run of the three real extensions (T-EXT-10, M5); `extensions.inspectView` (T-EXT-11, M5); a new page opened from extension UI (an options page, "Manage Extensions", a link in a popup) is dropped when the application has no new-window handler (T-EXT-12, M5); the store update check when the store is off (T-EXT-13, M6, where the network audit is). `developer_mode`, `blocklist` and `[extensions.bundled]` (S6) wait for the product owner's acceptance of those additions and are not read (T-EXT-14, M9).

### M6 — Providers and privacy (public v1 gate)

| Work | Tasks | Acceptance |
|---|---|---|
| Contracts | T-PRV-01 | One contract per service, used by default UI, Swift and `@bdk/internal`. |
| Provider selection | T-PRV-09 | An application switches a service between the default, a named provider with its own key, a remapped endpoint and a Swift provider without changing UI code. |
| Translation | T-PRV-02, T-PRV-03 | A real page is translated through the default presenter with no Google credential. A Swift UI drives the same service. Switching provider changes no application code. |
| Geolocation | T-PRV-04 | A page gets a position through the operating-system provider. No request goes to Google. |
| Reputation | T-PRV-05 | The chosen default blocks a listed test URL with an interstitial. Documentation and default UI state the protection level. |
| Security data | T-PRV-06 | Revocation and transparency data update through the chosen route, or the fallback and its staleness are documented. |
| Audit | T-PRV-07 | The network audit passes: no Google-owned host is contacted except sites the test visits. No sign-in, sync, metrics or crash upload code path is reachable. |
| Left over from M4 | T-WUI-07, T-PRV-10 | The built-in pages contact no Google service; a new profile has only the configured search engines and sends no typed text. |
| Left over from M5 | T-EXT-13 | With the store source off, no extension update check reaches the store. |
| Left open by M6 | T-PRV-11, T-PRV-12, T-PRV-13 | Signed reputation lists are published and downloaded; the Google contact the audit list leaves open is closed or recorded; the leak-check provider (S16) once the product owner accepts it. |

**Status, 2026-11-21: not done; built and tested on Chromium 158, the engine suite is 338 of 339 on an unlocked screen, and the network audit passes.** The engine was built incrementally on the managed checkout (release). With the screen locked it was 334 of 339: the 4 M3 scenarios that need an unlocked screen and a key window failed, and pass unlocked. The one failure left is `engine_a_page_gets_its_position_from_the_app_provider` (T-PRV-04, below). The network audit (`engine_privacy_a_scripted_session_contacts_no_google_host`, run with `BDK_TEST_NETLOG` and `--log-net-log`) first found three Google contacts, now turned off at start: the omnibox's AI Mode eligibility check (`www.google.com/async/folae`), the search-engine preconnect (`www.google.com`) and the translate ranker's model (`www.gstatic.com`); a scripted session now reaches only the hosts it visits. The engine build also needed: four compile fixes, the five `bdk_provider_*` functions added to the C ABI export patch, the translate observer leaving the driver's list, Chromium's never-translate list written block-first, and Safe Browsing given a known threat source. Before the engine build: Stub conformance 215 of 215 (26 new), Swift 150 of 150 (16 new), codegen 107 of 107, core lint (clang-format, clang-tidy) and `chromium:patch-check` pass; the new provider tests are also clean under AddressSanitizer and UBSan. The patch series is still 14 patches (the C ABI export patch gains five symbols); the one GN change is `support_external_google_api_key = true` in both presets.

| Task | Status | Where it is recorded |
|---|---|---|
| T-PRV-01 | Done without Chromium: `bdk_provider.h` vtable for translation, reputation and geolocation, answered through call contexts; the core checks every answer against the contract; a fake provider for each passes the contract tests on the stub (`tests_providers.c`, which also runs on the engine). | `packages/core/docs/conventions.md` ("Providers") |
| T-PRV-09 | Core and Swift done: `[services]` read at start, the start errors (`providerUnavailable`, `endpointUnsupported`, `keyMissing`, `keyUnused`, `providerMissing`, `providerNotApp`, `descriptorInvalid`), the Google key in `bdk_start_config_t`, `config.services` in Swift. Engine built and its scenario passes (key before Chromium starts, GN argument, key only for geolocation = "google"). The endpoint remap works only for the default reputation lists, whose download is T-PRV-11; Safe Browsing's and the network location provider's endpoints need patches (T-PRV-14, after v1). | conventions.md; `chromium/bdk/engine/providers/README.md` |
| T-PRV-02 | Open. The renderer agent is replaced: two scripts in the translate isolated world read and write text nodes, and the core's job sends them to the provider in batches (built; a real page translates through an application provider on the engine). The Bergamot engine, its model source and the measurements are not done, so the default provider answers `provider.unavailable` and a real page translates only through an application provider. Chromium's language detection model is a Google download, so without it detection rests on the page's declared language or the provider's `detect` (feasibility.md 6). | providers/README.md; feasibility.md 6 |
| T-PRV-03 | Done without Chromium: the ten `translation.*` commands and three events, preferences, offers, always and never, the request and Swift's `TranslationRequest` and `page.translation`. Engine built on Chromium's translate preferences, with a default sheet; its scenarios pass. Chromium never empties its never-translate list (services.md 5.12). | services.md 5.12; ui-requests.md 5.10 |
| T-PRV-04 | Built: the operating system's provider only (`PlatformOnly`) by default, the application's provider as a `device::LocationProvider`. The application's provider is not reached on the engine: a page's request never completes. Chromium gates geolocation on macOS's location permission (the device service, and Chrome's permission layer through `system_permission_settings`) before any provider starts, and the test application has none; an application provider needs that gate lifted for it. Open. Whether CoreLocation itself contacts Apple is the operating system's. | providers/README.md |
| T-PRV-05 | Core and stub done: the list format, Safe Browsing style URL expressions, the warning with "reduced protection". Engine built (BDK's Safe Browsing database manager under Chromium's interstitial); the default lists and an application provider raise the interstitial on the engine. Publishing, signing and downloading lists, and download checks: T-PRV-11. | conventions.md; providers/README.md |
| T-PRV-06 | Decided (D5): security data ships with each runtime release and the component updater is off; a mirror cannot answer Chromium's signed update checks without a patch. Built; the start-up settings are checked on the engine. Revocation between releases is absent (Chromium's built-in CRLSet is empty). | feasibility.md 6; risks.md D5, R11 |
| T-PRV-07 | Audit list written and its settings applied at start; the network audit test reads Chromium's net log (`tests_engine_privacy.c`, needs `BDK_TEST_NETLOG`). Run on the engine: it passes after three more Google contacts were turned off (above). What the list leaves open: T-PRV-12. | providers/README.md |
| S16 (leak check) | Not built: S16 is still a proposal for the product owner. `passwords.check` answers `provider.unavailable`, and `[services.leak_check] provider = "app"` is a start error (T-PRV-13). | sdk/README.md S16 |

### M7 — Automation host and DevTools

| Work | Tasks | Acceptance |
|---|---|---|
| Runtime choice | T-AUT-01 | Spike result recorded; choice made. |
| Host and sessions | T-AUT-02, T-AUT-03 | A script runs with no page open, drives two pages, executes in an isolated world, receives events and calls a registered command. A session without a capability is refused that command. |
| External channel | T-AUT-04 | Off by default. With it on, a second process connects with a token; a wrong token is refused. Nothing listens on a network port. |
| SDK | T-JS-04 | `@bdk/automation` bundles with Rolldown. Its entry point is separate from `@bdk/internal`. |
| DevTools | T-AUT-05 | Default, modified and replaced front ends work. |
| Capture | T-SVC-09 | Screenshots, PDFs and saved pages through `contents.*` and `page.capture`. |
| Engine verification | T-AUT-06 | The M7 engine work builds, and its engine scenarios pass. |

Exit criteria: a downstream application can build a CLI or agent wrapper using sessions, capability sets and events, with its own authentication and policy.

**Status, 2026-11-21: built and tested on Chromium 158; the engine suite is 342 of 343 on an unlocked screen.** The main session built M7 incrementally on the managed checkout (release, 15 patches) after seven compile fixes (`Caller`'s out-of-line constructor for Chromium's style plugin, `base::ok`, two V8 API changes, `ParseJsonDict` for `JSONReader::ReadDict`). With the screen locked it was 337 of 343: the 4 M3 scenarios that need an unlocked screen and a key window, the screenshots, and the two below. On an unlocked screen the screenshots and the M3 scenarios pass. A saved page "as HTML only" fetches the page again; a page that cannot be fetched again left no file, and Chromium still completed the save, so `contents.save` now fails when the item does not list its main file. `engine_user_scripts_follow_their_match_patterns` still fails: the page starts loading before `Page.addScriptToEvaluateOnNewDocument` reaches it, so the first document misses a `documentStart` script; the navigation has to wait for the registration (T-AUT-06 stays open for it). The runs kept Chromium's heavy-ad opt-out store in memory: its database load races profile deletion (see the M5 status). Verified on the engine: V8 starts in the macOS `kService` sandbox with `--jitless`, a script drives two pages with no page open, sessions are refused what they lack, cancel and revoke end a script, worlds, frames, CSS and input work on two pages, the DevTools protocol attaches, internal pages cannot be scripted, and the default, overlay, replaced and disabled DevTools front ends work. Before the engine build: Everything that needs no Chromium is in `packages/core`, `packages/js/automation`, `packages/cli` and `packages/swift` and passes: the stub conformance suite (213 tests, 24 new), Swift (159, 25 new), `js-automation` (52), the CLI (280 and 8 skipped), codegen, lint and format. The engine code (`chromium/bdk`) and one new patch were checked against the source at the pin; `chromium:patch-check` passes for the 15 patches. Nothing was built against Chromium, so no M7 behaviour has been seen in a real page. Decisions are in [amendments.md](amendments.md) section 2.16 and `packages/core/docs/conventions.md` ("Automation").

| Task | Status | Where it is recorded |
|---|---|---|
| T-AUT-01 | Decided: V8 in a `kService` utility process (`--jitless`, 512 MiB heap) for bundled scripts, an external runtime on the local socket for external tools, one protocol for both. The external option ran against the stub (Node client, `stub.integration.test.ts`); the V8 option is built and runs on the engine (it starts in the `kService` sandbox with `--jitless`). | [feasibility.md](feasibility.md) section 12 |
| T-AUT-02 | Done in the core and passing on the stub: sessions with a capability set and a scope of profiles and pages, revocation ending tasks, calls, connections and user scripts, tasks with progress, logs and an audit event per command. A session without a capability is refused (stub and endpoint tests). The host (`chromium/bdk/engine/automation`) is built; its acceptance scenario `engine_a_script_drives_two_pages` passes on the engine. | conventions.md, "Automation"; `tests_automation.c` |
| T-AUT-03 | The core's half done and passing on the stub (`contents.list` to `requestAttention`, `waitFor` on load and URL, `scripts.*`, the `debug.*` checks). The renderer's half (evaluate in page and isolated worlds and frames, query, text, snapshots, CSS, input, user scripts, the DevTools protocol over an in-process client) is built; each operation's engine scenario on two pages passes, with cleanup on navigation, except user scripts (their marker is not seen on the matching page). | `chromium/bdk/README.md`; `tests_engine_automation.c` |
| T-AUT-04 | Done and passing on the stub, in the core so the engine shares it: off unless `[automation.external] enabled`, a 0600 Unix socket in a 0700 directory, peer uid checked, hello with a token the application accepts or refuses; the suite checks that nothing listens on a network port. | `tests_endpoint.c`; Swift `EndpointTests` |
| T-JS-04 | Done: `@bdk/automation` (with `/client`, `/commands`, `/events`), bundled by `bdk build automation` with Rolldown into one module with no imports (`@bdk/*` inlined, amendments 2.16); its entry points are its own, apart from `@bdk/internal`. A bundled script ran against a simulated host; the real host is T-AUT-06. Loosely typed until T-JS-02: T-JS-06, M8. | `packages/js/automation/README.md` |
| T-AUT-05 | Built, and its engine scenarios pass: default, modified (overlay) and replaced front ends, `[devtools] frontend = "disabled"`, the `devtools` request with an undocked window as the default presenter, `devtools.open`, `close`, `state`. | `chromium/bdk/README.md`; [sdk/javascript-api.md](sdk/javascript-api.md) section 6 |
| T-SVC-09 | Commands, Swift (`page.capture`) and the stub's results done; the engine's screenshots (visible, full page, rectangle; PNG, JPEG), PDFs and saved pages are built; all pass on the engine on an unlocked screen; HTML only fails when the page cannot be fetched again. | [sdk/services.md](sdk/services.md) 5.15 |
| Exit criterion | Met on the stub: an external tool, written in TypeScript with `@bdk/automation/client`, is admitted by the application's own authenticator, holds a session with a capability set, is refused what it lacks, receives events in its scope and is audited. On the engine, the same session checks pass in `engine_a_script_drives_two_pages` and `engine_a_session_without_a_capability_is_refused`; the external channel itself was not run against the engine. | `stub.integration.test.ts` |

Not done, each a task: user scripts on the engine (T-AUT-06, M7); typed declarations for scripts (T-JS-06, M8); Swift's `webContentsCreated(_, by:)`, Swift wrappers for synthesised input and `waitFor`, a CPU budget for the host, coalescing for slow clients, `--watch` (T-AUT-07, M8). `Page.translate` in `@bdk/automation` calls `translation.*`, which T-PRV-03 (M6) defines.

### M8 — Arc-style reference application (public v1 gate)

A reference test application in `apps/examples/arc-style-browser`. It is not a product and BDK gains no spaces or tabs API from it.

| Scenario | Tasks | Acceptance |
|---|---|---|
| Sidebar, pinned items, two spaces | T-ARC-02 | Built with application code only. |
| Split view | T-ARC-02, T-ADP-06 | Two pages side by side behave as foreground for translate, find and dialogs. Three or more follow the documented behaviour from risk R2. |
| Move and reparent | T-ARC-03 | A page moves between panes, spaces and windows with state, tab identifier, `activeTab` grant and renderer preserved. |
| Hide and restore | T-ARC-03 | A page in a hidden space reports hidden, keeps running as a background tab, and restores. |
| Session restoration | T-ARC-03 | The application saves and restores its own layout and page state across relaunch. Chromium restores nothing on its own. |
| Extensions in the layout | T-ARC-03 | Popup and side panel appear in application-chosen containers. The probe's reports match the documented mapping. |
| Internal pages | T-ARC-03 | A bundled settings page at the branded URL with granted capabilities. |
| Automation | T-ARC-03 | A script drives two pages in different spaces, one of them signed in to a test site. |
| Left over from M7 | T-AUT-07, T-JS-06 | Swift reports pages sessions create and wraps synthesised input; the host has a CPU budget; slow clients are coalesced; scripts are typed from the generated declarations. |

Exit criteria: every scenario passes in CI, and no requirement in the brief's Cove section is met by weakening a browser service or an extension behaviour without that being recorded in the compatibility matrix.

### M9 — Release hardening

| Work | Tasks | Acceptance |
|---|---|---|
| Release procedure | T-PKG-06 | A release cut by hand from a clean tree, after the full local test pass, produces a signed, notarized runtime and Swift package. |
| Upgrade and rollback | T-PKG-05 | An application moves to a newer runtime and back. The profile is snapshotted and the downgrade policy is applied. |
| Reproducibility | T-CHR-09 | Two builds from the same inputs report the same ABI hash and equivalent manifests. Source-built and prebuilt runtimes pass the same conformance suite. |
| Security process | T-REL-01 | A written process and a measured rehearsal for shipping a runtime after an upstream security release. |
| Developer path | T-CLI-01, T-CLI-02, T-CLI-03, T-REP-06 | `bdk init` produces an application that runs from Xcode. `bdk doctor` reports toolchain, runtime compatibility and signature problems. Source mode builds with a custom patch. Documentation is published. |
| Robustness | T-REL-03 | Long-run, crash-recovery and memory tests pass. |
| Chromium's own tests | T-ADP-09 | The `browser_tests` filters named in T-ADP-02 pass on the BDK window, or each failure is recorded. Needed for the process-isolation gate. |
| Cross-platform review | T-XPL-01 | The ABI and service contracts are reviewed against Windows and Linux host requirements and a future .NET binding; findings are resolved or recorded. |
| Legal | T-REL-02 | Third-party notices ship. The legal review covers the store source (D4), component data (D5) and system codecs (D7). Widevine is deferred and not part of v1 unless the product owner decides otherwise. |
| Extension additions (after the product owner accepts S6) | T-EXT-14 | `developer_mode`, `blocklist` and bundled extensions behave as extensions.md says. |

### M10 — Windows feasibility

| Work | Tasks | Acceptance |
|---|---|---|
| Shared core on a second platform | T-XPL-02 | The platform-neutral part of the engine module compiles for Windows or Linux. |

## 3. Release gates for public v1

All must hold. Each maps to the spec's acceptance criteria.

| Area | Gate |
|---|---|
| Native hosting | A Swift application controls several independent pages with no BDK tab or workspace model. Focus, media, fullscreen, popups and downloads survive ordinary use, a renderer crash and a relaunch. |
| Browser services | Passwords, autofill, print to PDF, default-browser registration, history, bookmarks and downloads work. State is isolated per profile. |
| Default UI | Every browser-owned prompt, dialog and page works with no custom handler. Custom handlers can replace presentation without removing security checks. |
| Internal pages | The browser has a name and a stable scheme from `bdk.toml`. No `chrome://` appears in developer configuration. Discovery, override and Swift precedence are correct. Ambiguous routes fail the build. |
| Privilege boundary | Unregistered pages, subframes, extensions and web content cannot reach `@bdk/internal`. Aliases keep identity. Privilege ends on navigation. |
| Extensions | The suite passes and the matrix is published. Default popup works with no custom code. Disabled capabilities cause rejection with a reason. |
| Translation | A real page translates with no Google credential, through default and custom UI. |
| Security and reputation | Interstitials, certificate handling and download warnings work. The reputation default's protection level is stated. |
| Privacy | The network audit passes. |
| Process isolation | Sandbox, site isolation and process locks match upstream, checked by Chromium's own tests on the BDK build. |
| Automation | Internal pages and automation run in separate runtimes joined only at the broker. No unauthenticated remote endpoint exists. |
| Packaging | The prebuilt runtime embeds, signs and notarizes under another identity. Source and prebuilt builds expose the same contract. |
| Reproducibility | Runtimes carry a manifest, and their digest is recorded in the repository. Identical inputs give identical ABI hashes. |
| Upgrade | Applications choose their BDK version. Upgrade and rollback are tested. BDK never updates itself. |
| Arc-style reference | M8 passes. |
| Cross-platform | The ABI review is complete. |
| Documentation | Versioning, security-patch policy, compatibility gaps and experimental-API caveats are published. |

## 4. Cross-platform guardrails

Applied from M1 onward:

1. Shared engine code includes no platform header. Platform code lives in suffixed files.
2. The ABI headers compile as C on Linux in every pull request.
3. The conformance suite runs against the stub runtime on Linux.
4. Every ABI addition is checked against a short list: no AppKit concept, handle and ownership rules followed, a plausible Windows and Linux implementation named.
5. Default presenters are written against the request model, never against AppKit directly, so a second platform replaces presenters only.

## 5. What is deliberately late

| Item | Milestone | Reason |
|---|---|---|
| `bdk init`, templates, documentation site | M9 | Developer experience comes after the architecture is proven. |
| Nx plugin | Not planned | Explicit targets until repetition justifies it. |
| Chromium builds in CI, build attestation | With the `dev` branch | Cost. Until then the runtime is built and fully tested by hand before a release. |
| `dev` branch with beta releases, release-please, automated release workflow | Not scheduled | Releases are cut by hand first. |
| Remote build cache | Not planned | See [repository-and-tooling.md](repository-and-tooling.md) section 5.4. |
| Windows and Linux SDKs | After v1 | The shared core and ABI are designed for them now. |
| Optional Google-backed providers | After the defaults work | They need each application's own key. |

## 6. Open work after the M4 to M7 merge (2026-11-21)

M4 to M7 were merged into `main` with their engine scenarios not all passing. After the merge, on Chromium 158 and an
unlocked screen, the engine suite outside the extension scenarios is 451 of 456, and 38 of the 59 extension scenarios
pass when each runs alone. This section says which open work is being done now and which waits, and for what.

### Being fixed now

A fix-up session (branch `fix-engine-scenarios`) takes these, in this order. Each item names its task; the cause is
what the merge runs showed.

| Task | Failing | Cause, as far as known |
|---|---|---|
| T-EXT-02, T-EXT-03 | Two side-panel scenarios crash on a freed `BDKSidePanelContainerView`; `Check failed: browser_` at `host_context.cc:110` after the page-panel scenario fails; the disabled popup, context-menu and side-panel surface scenarios hang | A use after free in the default side-panel presenter; a host context outliving its `Browser`; requests for a disabled surface that never finish |
| T-AUT-06 | `engine_user_scripts_follow_their_match_patterns`, `engine_user_scripts_run_in_registration_order` | The script's marker is not seen on the matching page; the registration probably reaches the page after its document has started |
| T-PRV-04 | `engine_a_page_gets_its_position_from_the_app_provider` | Chromium gates geolocation on macOS's location permission before any provider starts; an application provider needs that gate lifted for it |
| T-UIR-07 | `engine.non_overridable_certificate_error` | The scenario's setup: Chromium ignores HSTS for `*.localhost`, so it needs another host name mapped to loopback |
| T-EXT-01, T-EXT-02, T-EXT-05, T-EXT-09, T-SWF-08 | The rest of the extension scenarios listed in the M5 status, and `ext_activation_needs_a_gesture_and_an_extension` | One cause each, in the M5 status above |
| T-SVC-12 | A dangling-pointer abort in long suite runs | A bookmarks export that outlives its profile |
| T-ADP-10 | No test fails; all best-effort work waits 3 minutes | Start-up completion is never signalled |
| T-REP-08 | No test fails; extension test windows come to the front in silent mode | The extension harness does not use the silent helpers |

The hand check of the M3 exit criterion in the minimal browser (part of T-UIR-07) is the product owner's.

### Waiting

| Task | Milestone | Waits for |
|---|---|---|
| T-AUT-07, T-JS-06 | M8 | The M8 session, in progress |
| T-EXT-10 | M5 | The extension scenarios above passing first; then the remaining matrix scenarios and the three real extensions |
| T-CLI-06, T-JS-05, T-SWF-09, T-EXT-11, T-EXT-12 | M5 | Nothing; features not yet built, after the fix-up |
| T-SVC-11 | M5 | The product owner confirming S12 |
| T-EXT-13, T-WUI-07, T-PRV-12 | M6 | Nothing; Google-contact work proved by the network audit, after the fix-up |
| T-PRV-10 | M6 | The product owner confirming S1 |
| T-PRV-02 | M6 | The Bergamot engine and its model source |
| T-PRV-11 | M6 | Feed licences, a signing key and where BDK publishes the lists |
| T-PRV-13 | M6 | The product owner accepting S16 |
| T-EXT-14 | M9 | The product owner accepting the S6 keys |
| T-ADP-09 | M9 | A `browser_tests` build |
| T-PRV-14 | After v1 | Patches to Chromium's Google protocol hosts |
