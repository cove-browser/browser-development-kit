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
| Broker | T-SVC-01, T-SVC-05 | Commands are defined once, checked per caller, cancellable. Application commands register from Swift. |
| Services | T-SVC-02, T-SVC-03, T-SVC-04, T-SVC-06 | History, bookmarks with folders and moves, downloads, site settings, cookies, permissions, passwords and autofill with Keychain storage, print to PDF, screenshots, default-browser registration. |
| UI requests and defaults | T-UIR-01 to T-UIR-06, T-SWF-04 | Every surface works with no handler registered. Each can be replaced from Swift. Handled, use-default and reject behave as specified. |
| Security flows | T-UIR-05 | Certificate and reputation interstitials cannot be disabled or bypassed. A permission cannot be granted without a user gesture. |

Exit criteria: a browser built with no custom UI code is usable for everyday browsing, including passwords, autofill, downloads, printing and permission prompts.

### M4 — Internal pages and privileged SDK

| Work | Tasks | Acceptance |
|---|---|---|
| Route registry and alias | T-WUI-02, T-WUI-05, T-WUI-06 | Logical routes resolve by the documented precedence. Collisions fail the build. Disabled pages show an error. Settings and extension management behave like any other page. Branded URLs display correctly in history and session data. |
| Bundled pages | T-WUI-03, T-CLI-04 | `Pages/Settings` is discovered, built with the Vite preset and served under the standard content security policy. |
| Capability broker | T-WUI-04 | A granted page reads and writes settings through `@bdk/internal`; Swift sees the same values. An ungranted page, a subframe, an extension and a web page are refused. A wrong-page interface request terminates the renderer. An alias carries its target's grants. |
| SDK and generation | T-JS-01, T-JS-02, T-JS-03 | Declarations are generated locally from interface definitions and registered commands. Nothing is published to npm. |

### M5 — Extensions (public v1 gate)

| Work | Tasks | Acceptance |
|---|---|---|
| Action, popup, context menu, options | T-EXT-02, T-EXT-04 | With no custom code, a popup opens from BDK's default extensions menu. With a Swift handler, the same popup appears in the application's layout with focus and close behaviour intact. |
| Side panel | T-EXT-03 | `chrome.sidePanel` opens in the default host and in an application-supplied container. |
| Installation | T-EXT-05, T-EXT-09 | Unpacked, CRX and self-hosted update sources work with Chromium's install prompt. Sources are configurable. The store source follows decision D4. |
| Compatibility checks | T-EXT-06 | A manifest requiring a disabled surface is rejected with a reason, at install and at update. A disabled API is absent, not silently accepted. |
| Suite and matrix | T-EXT-07, T-EXT-08 | The probe suite and a set of real extensions run in CI. The matrix is generated from results and lists known gaps. |

Exit criteria: representative real extensions, including a content blocker, a password manager and a developer tool, install and work. Every known difference from Chrome is documented.

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

### M7 — Automation host and DevTools

| Work | Tasks | Acceptance |
|---|---|---|
| Runtime choice | T-AUT-01 | Spike result recorded; choice made. |
| Host and sessions | T-AUT-02, T-AUT-03 | A script runs with no page open, drives two pages, executes in an isolated world, receives events and calls a registered command. A session without a capability is refused that command. |
| External channel | T-AUT-04 | Off by default. With it on, a second process connects with a token; a wrong token is refused. Nothing listens on a network port. |
| SDK | T-JS-04 | `@bdk/automation` bundles with Rolldown. Its entry point is separate from `@bdk/internal`. |
| DevTools | T-AUT-05 | Default, modified and replaced front ends work. |

Exit criteria: a downstream application can build a CLI or agent wrapper using sessions, capability sets and events, with its own authentication and policy.

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
| Cross-platform review | T-XPL-01 | The ABI and service contracts are reviewed against Windows and Linux host requirements and a future .NET binding; findings are resolved or recorded. |
| Legal | T-REL-02 | Third-party notices ship. The legal review covers the store source (D4), component data (D5) and system codecs (D7). Widevine is deferred and not part of v1 unless the product owner decides otherwise. |

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
