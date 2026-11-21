# BDK implementation tasks

Status: plan, 2026-11-14. Each task is sized for one implementation agent working in one worktree. Milestones are defined in [roadmap.md](roadmap.md); the design is in [architecture.md](architecture.md); tooling is in [repository-and-tooling.md](repository-and-tooling.md).

## How to use this list

- **Size** is relative effort: S is a small, contained change; M is several files in one subsystem; L crosses subsystems or needs a Chromium build cycle; XL should be split once its first step shows the shape. Sizes are not delivery estimates.
- **Mode** says what the agent needs. *Prebuilt* tasks need no Chromium checkout. *Source* tasks need the managed checkout and its lock. *Experiment* tasks need their own scratch checkout.
- A task is done when its tests pass in CI, or, for M0 tasks, when its result is written into [feasibility.md](feasibility.md) with the commands run.
- Never mark something as working from source reading alone. If a test could not be run, say so and leave the task open.
- Tasks in M0 may use throwaway code. From M1 on, code is kept.
- Work a milestone leaves undone becomes a task here before the milestone is marked done (see [README.md](README.md), "Recording gaps").

## Order of work

```text
T-CHR-01 → T-CHR-02 → T-CHR-05
                  ├→ T-MAC-01 → T-MAC-02 ─┐
                  └→ T-ADP-01 → T-ADP-02 ─┴→ T-ADP-03 → T-ADP-04 → T-EXT-01
                                                    ├→ T-MAC-04
                                                    ├→ T-WUI-01
                                                    ├→ T-ADP-08 → T-ARC-01
                                                    └→ T-PKG-01          ⇒ gate G0
T-REP-01 → T-REP-02 → T-REP-03          (may run alongside M0)
T-REP-01 → T-CLI-05 → T-CFG-02          (the CLI executable exists before the resolver, in M2)
T-ABI-01 → T-ABI-02 → T-ABI-05          (may run alongside M0)
G0 → T-CHR-03, T-CHR-04 → T-CHR-08 → T-CHR-09 → T-ABI-04 → T-SWF-* → T-SVC-01, T-UIR-01 → everything else
```

## Chromium checkout, build and runtime pipeline

| ID | Task | M | Size | Mode | Inputs | Outputs | Tests | Depends on |
|---|---|---|---|---|---|---|---|---|
| T-CHR-01 | Install depot_tools and create the managed checkout at the pin under `BDK_HOME`, using the shared Git cache | M0 | M | Source | `chromium/pin.json`; storage layout | `chromium/gclient/` template; a synced checkout | `gclient sync` reaches the pinned commit; a second sync is a no-op | None |
| T-CHR-02 | Build unmodified Chromium at the pin with a development GN preset | M0 | L | Source | Checkout | `chromium/gn/dev.gn`; a running Chromium | The built binary starts and loads a page | T-CHR-01 |
| T-CHR-05 | Measure checkout size, output size, full and incremental build time, and packaged size | M0 | S | Source | A completed build | Figures added to `feasibility.md` | Not applicable | T-CHR-02 |
| T-CHR-03 | Patch tooling: apply the ordered series, export from commits, and a no-build series check | M1 | M | Source | `chromium/patches/series` | `chromium:patch-apply`, `chromium:patch-export`, CI check | A deliberately conflicting patch fails the check with the file named | T-CHR-02 |
| T-CHR-04 | Mount `chromium/bdk` as `//bdk` and wire it into the build | M1 | M | Source | Overlay directory | GN integration; `chromium:build` target | A trivial `//bdk` target builds and links into the framework | T-CHR-03 |
| T-CHR-06 | Measure ccache hit rate across two checkouts with the recommended settings | M1 | M | Experiment | Two checkouts; ccache settings | Hit rates and any required setting changes recorded | A second checkout rebuild of `base` shows cache hits | T-CHR-02 |
| T-CHR-07 | Experiment checkouts: create by APFS clone or `tools/git/worktree.py`, with their own lock; record whether Siso state survives | M1 | M | Experiment | Managed checkout | `chromium:experiment` helper | A managed build and an experiment build run at the same time without touching each other's outputs | T-CHR-03 |
| T-CHR-08 | Package the runtime and write its manifest | M1 | M | Source | Built framework and helpers; manifest schema | `chromium:package`; `manifest.json` | Manifest digests match the files; missing file fails packaging | T-CHR-04 |
| T-CHR-09 | Runtime store resolver with separate source and build identities; reproducibility check | M1, M9 | M | Prebuilt | Prototype in `experiments/worktree-cache` | `chromium:runtime`; `.bdk/runtime.json` | The cases in feasibility 9 as automated tests: shared runtime, changed patch, tamper, eviction, toolchain change | T-CHR-08 |
| T-CHR-10 | Rehearse a rebase to the next upstream tag and record the effort | M1 | L | Experiment | Patch series; next tag | Notes; input to the roll bot (T-CHR-11) | The series applies and the build passes on the new tag | T-CHR-03 |
| T-CHR-11 | Chromium roll bot: detect a newer stable, bump `chromium/pin.json`, try the patch series with no build, open or update one roll pull request, raise an issue when the pin is stale | M1 | M | Prebuilt | Tooling document 6.1; T-CHR-03 | `tooling/` script; `chromium-roll.yml` | Against a fixture pin one version behind, a pull request is opened with the right title; a conflicting patch produces a draft that names the file; an up-to-date pin does nothing | T-CHR-03, T-REP-03 |
| T-CHR-12 | Spike for decision D7: H.264, AAC and HEVC through the system decoders (VideoToolbox, AudioToolbox), with FFmpeg kept in its open-codec configuration | M2 | L | Source | D7; `media/` at the pin | Finding in `feasibility.md`; a patch if it works | MP4 files with H.264 and AAC play from a file and through Media Source; `canPlayType` answers match; no bundled proprietary decoder is in the packaged runtime; the size of the AAC patch and the state of H.264 in WebRTC are recorded | T-CHR-04 |

## Chromium adapters

| ID | Task | M | Size | Mode | Inputs | Outputs | Tests | Depends on |
|---|---|---|---|---|---|---|---|---|
| T-ADP-01 | Create the BDK `BrowserWindow` by adapting `WebUIBrowserWindow`, and add the factory branch behind a switch | M0 | L | Source | `browser_window_factory.cc`; `webui_browser_window.*` | BDK window class; factory patch | A `Browser` is created with the BDK window and no `BrowserView` | T-CHR-02 |
| T-ADP-02 | Add the `BrowserWindowFeatures` arm and audit null `BrowserView` casts by running Chromium's browser tests | M0 | L | Source | `browser_window_features.cc`; test filters named in feasibility 1 | Patch; a list of crashes and unimplemented calls | The named test filters complete; every failure is classified | T-ADP-01 |
| T-ADP-03 | Host contexts: parking context, attach and move through the tab model | M0, M2 | L | Source | Architecture 4.1 | Context manager in `//bdk` | A page moved between two contexts keeps its tab identifier and state; no direct reparenting path exists | T-ADP-01 |
| T-ADP-04 | Service smoke test under the BDK window | M0 | M | Source | Feasibility 1 coupling table | Results table in `feasibility.md` | History entry recorded; bookmark saved; download completes; permission prompt answered; print preview opens; JavaScript dialog shows; find works | T-ADP-03, T-MAC-02 |
| T-ADP-05 | Route Chromium-created windows (popups, DevTools, picture-in-picture) to new-window requests | M2 | M | Source | `browser_navigator.cc`; architecture 4.1 | Request type and routing | `window.open` and undocked DevTools each produce one request with the right type | T-ADP-03 |
| T-ADP-06 | "Presented tab" predicate for layouts with more than two visible pages; enumerate and patch the call sites | M8 | L | Source | Feasibility 10; risk R2 | Patch; list of sites | In a three-pane layout, translate, find and dialogs work for the focused pane and behave as documented for the others | T-ADP-04 |
| T-ADP-07 | Sessions: stop automatic window restore; expose page navigation state for the application | M2 | M | Source | `session_service.h` | State export and import functions | After relaunch Chromium restores nothing; a page restored from saved state has its history | T-ADP-03 |
| T-ADP-08 | Host-context mapping experiment for decision D1: off-screen pages as background tabs of one window versus in a separate context | M0 | M | Source | MV3 probe | Recorded probe output for both mappings | Not applicable; this produces evidence | T-EXT-01 |
| T-ADP-09 | Run the `browser_tests` filters T-ADP-02 named against the BDK `BrowserWindow`, and fix or record each failure (left over from M3) | M9 | L | Source | T-ADP-02 filters; a `browser_tests` build | Results in `feasibility.md`; fixes | The named filters pass, or each failure is recorded with its cause | T-ADP-03 |

## macOS host

| ID | Task | M | Size | Mode | Inputs | Outputs | Tests | Depends on |
|---|---|---|---|---|---|---|---|---|
| T-MAC-01 | content_shell spike: a Swift application owns `NSApplication` and runs the content main runner | M0 | M | Source | Feasibility 2 | Throwaway Swift application | A page loads; typing, an input method and command keys work | T-CHR-02 |
| T-MAC-02 | `//chrome` start-up patches for an application-owned `NSApplication` | M0 | L | Source | The five patches in feasibility 2 | One patch with BDK hooks; BDK application base class | `//chrome` starts inside the Swift application with no Chrome window, menu or delegate; clean termination | T-MAC-01 |
| T-MAC-03 | Host view: attach, hide, reparent, focus, unhandled keys, accessibility parent | M2 | L | Source | Feasibility 2 | Host view in `//bdk` and Swift | Visibility events on hide and move; focus follows clicks; an unhandled shortcut reaches the application; VoiceOver reads page content; input method works | T-MAC-02, T-ADP-03 |
| T-MAC-04 | Test Chromium's dialog and bubble views on a native window that is not a Views widget | M0 | M | Source | Risk R7 | Result recorded; decision on default presenters | A permission prompt and a JavaScript dialog either display correctly anchored to a rectangle, or the failure is documented | T-ADP-04 |
| T-MAC-05 | Fullscreen and context-menu delegates | M2 | M | Source | `web_contents_view_mac.mm:379-385` | Delegates emitting UI requests | HTML fullscreen enters and exits; a context menu request carries the right items | T-MAC-03 |

## C ABI and core

| ID | Task | M | Size | Mode | Inputs | Outputs | Tests | Depends on |
|---|---|---|---|---|---|---|---|---|
| T-ABI-01 | ABI conventions, header skeleton, version and hash | M1 | M | Prebuilt | Architecture 5 | `packages/core/include`; conventions document | Headers compile as C and as C++ with warnings as errors | None |
| T-ABI-02 | Stub runtime and conformance suite | M1 | L | Prebuilt | T-ABI-01 | In-memory implementation; test suite | Suite passes against the stub | T-ABI-01 |
| T-ABI-03 | Interface definition format for commands, capabilities and events, with validation | M2 | M | Prebuilt | Architecture 5 and 9 | Format, validator, first definitions | Invalid definitions fail with a specific message; generated output is stable | T-ABI-01 |
| T-ABI-04 | Implement host functions in the engine: runtime, profiles, WebContents, navigation, events | M2 | XL | Source | T-ABI-01; T-ADP-03 | `//bdk` implementation | Conformance suite passes against the real runtime | T-ADP-03, T-MAC-02, T-CHR-04 |
| T-ABI-05 | CI check that the headers compile as C on Linux and the suite runs against the stub there | M1 | S | Prebuilt | T-ABI-02 | Workflow job | Job passes | T-ABI-02 |

## Swift SDK

| ID | Task | M | Size | Mode | Inputs | Outputs | Tests | Depends on |
|---|---|---|---|---|---|---|---|---|
| T-SWF-01 | Swift package over the C ABI using the stub; application base class | M2 | M | Prebuilt | T-ABI-02 | `packages/swift` | Unit tests against the stub; ABI mismatch produces a readable error | T-ABI-02 |
| T-SWF-02 | Profiles, WebContents, navigation and events | M2 | L | Prebuilt | T-ABI-04 | Swift API | Unit tests on the stub; integration tests on the real runtime | T-SWF-01, T-ABI-04 |
| T-SWF-03 | Host view and SwiftUI wrapper | M2 | M | Prebuilt | T-MAC-03 | View types | Integration test: two pages visible; one moved to a second window | T-SWF-02, T-MAC-03 |
| T-SWF-04 | UI-request handler API with handled, use-default and reject | M3 | M | Prebuilt | T-UIR-01 | Handler registration | Each result path covered per surface | T-UIR-01 |
| T-SWF-05 | Minimal browser example | M2 | M | Prebuilt | T-SWF-03 | `apps/examples/minimal-browser` | Launch smoke test; renderer-crash recovery; relaunch | T-SWF-03 |
| T-SWF-06 | Page commands and page controls left over from M2: `page.canPerform` and `perform(PageCommand)`, `page.zoom.set(_:scope:)`, settable `isMuted`, `exitPictureInPicture()` (swift-api.md 6.7 and 9) | M4 | M | Source | T-MAC-03 | C ABI functions; engine; Swift | Each command and control on the stub and in an engine scenario; `canPerform` is false where Chromium would disable the command | T-SWF-02 |
| T-SWF-07 | `page.find` (`start`, `next`, `previous`, `stop`, `present`) and `page.print()` started by the application (left over from M3; the surfaces exist) | M3 | M | Source | T-UIR-04, T-UIR-06 | C ABI functions; engine; Swift | `find.start` reports match counts and the active match; `present()` and `print()` raise the find and printing requests, handled and unhandled | T-UIR-06 |
| T-SWF-08 | Public window registration (S11): `runtime.windows.register`, `BDKWindow` with `kind`, `pages`, `activePage`, `extensionWindowID`, `anchors` and `delegate` (swift-api.md 8.1). Registration is automatic and internal today | M5 | M | Source | T-ADP-03 | Swift API; anchors used by the default presenters | A registered popup window reports `kind` `popup` to `windows.get`; a default presenter attaches to a supplied anchor | T-SWF-03 |

## Browser-owned UI requests and default presenters

| ID | Task | M | Size | Mode | Inputs | Outputs | Tests | Depends on |
|---|---|---|---|---|---|---|---|---|
| T-UIR-01 | Request router, handler results, configuration modes | M3 | M | Source | Architecture 7 | Router in `//bdk` | No handler uses the default; reject denies; disabled denies | T-ABI-04 |
| T-UIR-02 | Permissions, JavaScript dialogs, HTTP authentication, file pickers | M3 | L | Source | T-MAC-04 result | Requests and default presenters | Each works with and without a handler; no grant without a user gesture | T-UIR-01 |
| T-UIR-03 | Context menus, new-window requests, downloads | M3 | L | Source | T-ADP-05 | Requests and defaults | Default menu includes extension items later; download progress and destination control | T-UIR-01 |
| T-UIR-04 | Autofill and password prompts; printing and print preview | M3 | L | Source | Feasibility 1 coupling table | Requests and defaults | Save and fill a password; print to PDF | T-UIR-01 |
| T-UIR-05 | Interstitials: certificate errors and reputation warnings | M3 | M | Source | Architecture 7 security rules | Tests and enforcement | Setting the surface to disabled or returning reject cannot bypass an interstitial | T-UIR-01 |
| T-UIR-06 | Find in page, fullscreen, notifications | M3 | M | Source | T-MAC-05 | Requests and defaults | Each works with and without a handler | T-UIR-01 |
| T-UIR-07 | M3 verification left open: HTTP authentication end to end (a test server that answers 401), a non-overridable interstitial (HSTS or pinning), the card autofill prompt, and the M3 exit criterion checked by hand in the minimal browser | M4 | M | Source | T-UIR-02, T-UIR-04, T-UIR-05 | Engine scenarios; a recorded manual check in the roadmap | Each scenario passes; `proceed` on a non-overridable error is refused | T-UIR-05 |
| T-UIR-08 | Print preview: host Chromium's preview, or record the system print panel as the permanent default in amendments.md (M3 printed through the system panel because the preview needs a parent widget) | M4 | M | Source | T-MAC-04 result | Presenter or recorded decision | Print to PDF through the preview, or the decision recorded | T-UIR-04 |

## Service broker and services

| ID | Task | M | Size | Mode | Inputs | Outputs | Tests | Depends on |
|---|---|---|---|---|---|---|---|---|
| T-SVC-01 | Broker core: command registry, capability check, cancellation, progress | M3 | L | Source | T-ABI-03 | Broker in `//bdk`; stub equivalent | A caller without the capability is refused; cancellation stops work | T-ABI-04 |
| T-SVC-02 | History, bookmarks and downloads commands | M3 | L | Source | Chromium services | Commands and Swift wrappers | Search and clear history; nested bookmark folders and moves; download control; profile isolation | T-SVC-01 |
| T-SVC-03 | Settings, site settings, cookies and permissions | M3 | L | Source | Chromium services | Commands and Swift wrappers | Read and write through Swift and see the same value in Chromium | T-SVC-01 |
| T-SVC-04 | Passwords and autofill surface; per-application Keychain item name | M3 | L | Source | `keychain_password_mac.mm` | Commands; start-up naming | Stored passwords survive relaunch; the Keychain item carries the application's name | T-SVC-01 |
| T-SVC-05 | Application-registered commands from Swift | M3 | M | Prebuilt | T-SVC-01 | Registration API; declarations for generation | A registered command is callable from Swift and listed for code generation | T-SVC-01 |
| T-SVC-06 | Default-browser registration, print to PDF, screenshots | M3 | M | Source | Platform adapters | Commands | Each verified on macOS | T-SVC-01 |
| T-SVC-07 | Commands and events defined with Swift wrappers that the engine did not implement at first: the commands `bookmarks.import`, `bookmarks.export`, `permissions.certificateExceptions` and `permissions.revokeCertificateException` (rejected as `broker.notImplemented`), and the events `cookies.changed`, `passwords.changed`, `passwords.issuesChanged` and `autofill.changed` (never emitted) | M3 | M | Source | services.md 5 | Engine implementations | An engine scenario for each, called or observed through the Swift wrapper: each event fires after the change that should cause it | T-SVC-02, T-SVC-03 |
| T-SVC-08 | Typed application settings: `config.settings.register(BDKSettingKey<T>, default:, scope:, exposedToPages:)` and typed `get` and `set` (swift-api.md 12.3; left out of T-SVC-05) | M3 | M | Prebuilt | T-SVC-05 | Registration; storage with browser settings; declarations for generation | A registered key reads its default and survives relaunch; `exposedToPages: false` is refused to every page | T-SVC-05 |
| T-SVC-09 | `contents.screenshot`, `contents.pdf` and save-page commands, and `page.capture` in Swift (left over from T-SVC-06: the engine can capture but no command exposes it) | M7 | M | Source | services.md `contents.*` | Commands; Swift | Visible, full-page and rectangle screenshots; a PDF; each format of save | T-AUT-03 |
| T-SVC-10 | Address-bar services (S1): `search.classify`, `search.suggestions` and search-engine management; `profile.search` in Swift | M4 | L | Source | services.md 5.13; S1 confirmed | Commands; Swift | Input classified as a URL or a search with the configured engine; suggestions from history and bookmarks with no Google endpoint | T-SVC-02 |

## Internal pages and configuration

| ID | Task | M | Size | Mode | Inputs | Outputs | Tests | Depends on |
|---|---|---|---|---|---|---|---|---|
| T-WUI-01 | Alias scheme prototype: startup switch, per-process registration, rewrite handler, one bundled test page with a privileged call | M0 | L | Source | Feasibility 4 and 5 | Patch and test page | One binary serves the page under two scheme names; granted call succeeds; ungranted page, subframe and web page are refused; a web page cannot navigate to the branded or internal URL | T-ADP-03 |
| T-CFG-01 | `bdk.toml` JSON Schema with valid and invalid fixtures | M2 | M | Prebuilt | Spec sections 4, 5, 8A | Schema; `tests/config/` | Every invalid fixture fails with its expected message | None |
| T-CFG-02 | Resolver: discovery, name normalisation, collisions, aliases, output of `bdk.resolved.json` | M2 | M | Prebuilt | Architecture 8.2 | Resolver in the CLI | Precedence, collision and alias-chain fixtures | T-CFG-01, T-CLI-05 |
| T-WUI-02 | Route registry in the engine: load the resolved table, freeze at start | M4 | M | Source | T-CFG-02 | Registry | A registration after start is refused; unknown routes are blocked | T-WUI-01, T-CFG-02 |
| T-WUI-03 | Bundled-page data source and Vite preset | M4 | L | Source | Architecture 8.4 | Data source; preset | Inline script and remote script are blocked; a Vite-built page loads | T-WUI-02 |
| T-WUI-04 | Capability broker binding for pages, with per-call checks | M4 | L | Source | Architecture 9.2 | Mojo interface and binder | The negative list in roadmap M4; wrong-page request terminates the renderer; privilege ends on navigation | T-WUI-03, T-SVC-01 |
| T-WUI-05 | Built-in page modes: `chromium`, `native`, `disabled`, including settings and extension management | M4 | M | Source | Architecture 8.1 | Mode handling | An overridden built-in page has no alternate route; a disabled page shows an error | T-WUI-02 |
| T-WUI-06 | Display mapping for branded URLs in navigation, history and session data | M4 | M | Source | Risk R8 | Mapping in one place | The application never receives a `chrome://` URL for a page it named logically | T-WUI-02 |

## CLI

| ID | Task | M | Size | Mode | Inputs | Outputs | Tests | Depends on |
|---|---|---|---|---|---|---|---|---|
| T-CLI-01 | `bdk doctor`: toolchain, runtime compatibility, signatures. No disk reporting. | M9 | M | Prebuilt | Tooling document | Command | Each failure mode has a fixture and a message | T-CHR-09 |
| T-CLI-02 | `bdk init` with prompts and a flag for every prompt; the Swift starter template; page templates per JavaScript framework (`sdk/cli.md` section 4) | M9 | L | Prebuilt | T-SWF-05 | Command; `apps/templates/swift-browser`; `apps/templates/pages` | A generated application builds and runs for every combination of choices; a run with flags asks nothing | T-CLI-04 |
| T-CLI-03 | `bdk patch` for source mode | M9 | M | Source | T-CHR-03 | Command | A custom patch builds into a runtime whose manifest records it | T-CHR-09 |
| T-CLI-04 | Xcode build-phase integration: resolve configuration, build pages, generate declarations, run the signing script. Includes the check that the packaged CLI loads a project's own Vite (`sdk/cli.md` 2.3). | M4 | L | Prebuilt | T-CFG-02, T-PKG-03 | Build-phase scripts; the `xcode` platform module | A change to a page or to `bdk.toml` is picked up by an Xcode build; `bdk build pages` runs Vite from the project's `node_modules` with no other Node on the path | T-CFG-02, T-PKG-03, T-CLI-05 |
| T-CLI-05 | CLI foundation: a TypeScript project with a platform-neutral core and a platform interface; bundled with Rolldown; packaged with `node --build-sea` on the pinned Node 26; shipped as an executable in the Swift package's artifact bundle (`sdk/cli.md` section 2) | M2 | M | Prebuilt | `sdk/cli.md` 2.1 to 2.3 | `packages/cli`; the `cli:package` target; one adapter for embedded assets; a record of the checks | The executable runs on a machine with no Node. It runs from the artifact bundle and from an Xcode run-script phase. Start-up time is measured. An embedded template and schema are read. The signed, notarized binary passes Gatekeeper. A lint rule stops the core importing a platform module. | T-REP-01 |

## JavaScript packages

| ID | Task | M | Size | Mode | Inputs | Outputs | Tests | Depends on |
|---|---|---|---|---|---|---|---|---|
| T-JS-01 | Protocol package: command, event and transport types | M4 | M | Prebuilt | T-ABI-03 | `packages/js/protocol` | Vitest; type tests | T-ABI-03 |
| T-JS-02 | Code generators for declarations from interface definitions and registered commands | M4 | M | Prebuilt | T-SVC-05 | `packages/js/codegen` | Snapshot tests; a changed definition changes output | T-JS-01 |
| T-JS-03 | `@bdk/internal` | M4 | M | Prebuilt | T-WUI-04 | `packages/js/internal` | Unit tests with a mock transport; integration through T-WUI-04 | T-JS-02 |
| T-JS-04 | `@bdk/automation`, commands and events, bundled with Rolldown | M7 | M | Prebuilt | T-AUT-02 | `packages/js/automation` | Unit tests; a bundled script runs in the host | T-JS-02, T-AUT-02 |
| T-JS-05 | Type `@bdk/automation`'s service namespaces, `@bdk/commands` and `@bdk/events` from the generated declarations, and keep `Automation/node_modules/@bdk` linked to them (javascript-api.md 7). M7 built them loosely typed because T-JS-02 is M4 work running in parallel | M8 | M | Prebuilt | T-JS-02 output | Typed declarations for scripts | A type test: a wrong argument name to a service call fails `tsc`; an application command declared in Swift is typed in a script | T-JS-02, T-JS-04 |

## Extensions

| ID | Task | M | Size | Mode | Inputs | Outputs | Tests | Depends on |
|---|---|---|---|---|---|---|---|---|
| T-EXT-01 | Run the MV3 probe under native hosting and record results | M0 | M | Source | `experiments/mv3-probe`; assertions below | Results in `feasibility.md`; fixes to the probe | The assertion list below | T-ADP-04 |
| T-EXT-02 | Action state, activation, and a native extensions container and popup view | M5 | L | Source | Feasibility 3 | Container, view, requests, default extensions menu | Popup opens from the default menu and from a Swift handler; closes on blur; `action.openPopup` works | T-UIR-01 |
| T-EXT-03 | Side panel: `BrowserWindowFeatures` patch and BDK implementation | M5 | L | Source | `browser_window_features.cc:819-894` | Side-panel host; request | `sidePanel.open` shows the panel in the default host and in an application container | T-EXT-02 |
| T-EXT-04 | Context-menu items, options page | M5 | M | Source | T-UIR-03 | Requests | The item appears, grants `activeTab` and fires its event; options page opens | T-EXT-02 |
| T-EXT-05 | Installation: unpacked, CRX with BDK-owned off-store policy, self-hosted update URL, configurable sources | M5 | L | Source | `extension_management.cc:388-402` | Policy; configuration | Each source installs with Chromium's prompt; a disallowed source is refused with a reason; an update applies | T-UIR-02 |
| T-EXT-06 | Pre-install manifest inspection and API availability for disabled surfaces | M5 | M | Source | `_api_features.json` delegated checks | Checks; diagnostics | A manifest requiring a disabled surface is rejected at install and at update; a disabled API is absent | T-EXT-05 |
| T-EXT-07 | Compatibility suite and generated matrix, with real extensions | M5 | L | Prebuilt | T-EXT-01 | `tests/extensions/`; published matrix | Suite runs in CI; matrix lists results and known gaps | T-EXT-02 to T-EXT-06 |
| T-EXT-08 | DevTools extension pages | M5 | M | Source | `devtools_window.cc` | Support or a recorded gap | A `devtools_page` panel loads in undocked DevTools | T-ADP-05 |
| T-EXT-09 | Store source according to decision D4 | M5 | M | Source | `extension_urls.cc`; D4 | Named source; when it is enabled, the store's install button reads "Add to <browser name>" | Enabled only when configured; update checks follow configuration; the relabel falls back to the store's own text if the page changes, and installation still works | T-EXT-05 |

### Assertions for T-EXT-01

Load the probe unpacked with two pages A and B visible, B focused.

1. The service worker starts, and each content script receives an acknowledgement.
2. `tabs.query({active: true, currentWindow: true})` returns exactly B. `windows.getCurrent()` matches B's window.
3. Focusing A fires `tabs.onActivated` for A.
4. Moving A to a second window keeps A's tab identifier and fires detach then attach.
5. Clicking the action with B focused lets the worker inject into B and not into A. Navigating B removes the grant.
6. The popup opens in a BDK host anchored to an arbitrary rectangle, reports the same active tab, and closes on blur.
7. `tabs.sendMessage` reaches the content script in both pages, and still reaches A after the move.
8. The context-menu item is present in the menu model and its click event names the right tab.
9. `sidePanel.open` either works or fails with an explicit error. Record which.
10. Record what `windows.getAll` reports for the parking context, and whether it must be hidden from extensions.

## Providers and privacy

| ID | Task | M | Size | Mode | Inputs | Outputs | Tests | Depends on |
|---|---|---|---|---|---|---|---|---|
| T-PRV-01 | Provider contracts for translation, reputation and geolocation | M6 | M | Prebuilt | Architecture 11 | Contract definitions | A fake provider for each passes a contract test | T-ABI-03 |
| T-PRV-09 | Provider selection plumbing: named provider, endpoint remap and `provider = "app"` in the schema; application key supplied at start through `InitializeAndOverrideAPIKey` | M6 | M | Source | Architecture 11.1; `google_apis/BUILD.gn` | Schema entries; GN argument; start-up call | With no key no request reaches Google; with a key the geolocation network provider uses it; a remapped endpoint receives the request | T-PRV-01, T-CFG-01 |
| T-PRV-02 | Translation spike: renderer agent replacing script injection, with a Bergamot engine | M6 | XL | Source | Feasibility 6; `translate.mojom` | Agent; engine integration; measurements | A real page is translated offline; speed and memory recorded | T-PRV-01 |
| T-PRV-03 | Translation state, preferences, UI requests and default presenter | M6 | L | Source | T-PRV-02 | Requests; presenter | Default and custom UI drive the same service; always and never preferences persist | T-PRV-02, T-UIR-01 |
| T-PRV-04 | Geolocation through the operating-system provider only | M6 | M | Source | `device_features.cc:116-173` | Mode override; contract adapter | A page gets a position; the network audit shows no Google request | T-PRV-01 |
| T-PRV-05 | Reputation provider according to decision D2, mapped to Chromium's interstitials | M6 | L | Source | D2; `sb_protocol_manager_util.cc` | Provider; list format and update | A listed test URL is blocked; an unlisted one is not; documentation states the protection level | T-PRV-01, T-UIR-05 |
| T-PRV-06 | Component data: establish whether a mirror can serve update checks; implement the chosen route | M6 | L | Source | Feasibility 6; D5 | Finding; configuration | Revocation and transparency data update, or the fallback is implemented and documented | None |
| T-PRV-07 | Google-removal audit list and the network audit test | M6 | L | Source | Architecture 12 | Audit list; test | A scripted session contacts no Google-owned host other than sites it visits | T-ADP-04 |
| T-PRV-08 | Optional providers: Apple Translation, Google Web Risk | After v1 | M | Source | T-PRV-01 | Adapters | Each works with the application's own key or installed languages | T-PRV-03, T-PRV-05 |

## Automation and DevTools

| ID | Task | M | Size | Mode | Inputs | Outputs | Tests | Depends on |
|---|---|---|---|---|---|---|---|---|
| T-AUT-01 | Spike: V8 in a sandboxed utility process against an external runtime on the local channel | M7 | M | Source | Architecture 9.3 | Recorded choice | Both options run one script that calls one command | T-SVC-01 |
| T-AUT-02 | Automation host and sessions with an application-chosen capability set | M7 | L | Source | T-AUT-01 | Host; session API | A session without a capability is refused; revoking a session stops its scripts | T-AUT-01 |
| T-AUT-03 | Page operations: script in isolated and page worlds, frame targeting, CSS injection, persistent scripts, DevTools protocol over an in-process channel | M7 | L | Source | Spec section 7 | Commands | Each operation tested on two pages; cleanup on navigation | T-AUT-02 |
| T-AUT-04 | External local endpoint: off by default, user-restricted, token-authenticated | M7 | M | Source | Architecture 9.3 | Endpoint | Wrong token refused; nothing listens on a network port | T-AUT-02 |
| T-AUT-05 | DevTools: default, modified and replaced front ends | M7 | L | Source | Spec section 7 | Configuration; launcher | Each mode opens and inspects a page | T-ADP-05 |
| T-AUT-06 | Build and run M7's engine work: `chromium:build` with the automation host patch, then `core:test-engine` with the 15 scenarios of `tests_engine_automation.c`; fix what the build and the scenarios find. Settles the V8 half of T-AUT-01 (feasibility 12): V8 in the `kService` sandbox on macOS, `--jitless`, the heap limit, start-up time | M7 | L | Source | The M7 engine code (written 2026-11-20, not built) | A built runtime; recorded results in the M7 status | Every new engine scenario passes; the replaced front end is run in a process of its own (`--run`); start-up of the host measured | T-AUT-01 to T-AUT-05, T-SVC-09 |
| T-AUT-07 | M7 follow-ups in the SDKs: Swift `webContentsCreated(_, by: .automation(session))` (swift-api.md 13; pages sessions create arrive only in `runtime.automation.events`), Swift wrappers for `contents.click`, `hover`, `type`, `press`, `scroll` and `waitFor`, task events and `isOpen` exercised through the stub, a CPU budget for the host (only a heap limit is set), coalescing a slow external client's state events instead of closing it (`automation.overloaded`), `bdk build automation --watch` | M8 | M | Prebuilt | M7 status in roadmap.md | Swift API; host limits; CLI flag | A Swift test for each wrapper and event; a host test that a busy script is stopped at its budget; a slow client keeps its connection | T-AUT-06 |

## Packaging and release

| ID | Task | M | Size | Mode | Inputs | Outputs | Tests | Depends on |
|---|---|---|---|---|---|---|---|---|
| T-PKG-01 | Neutral framework and helper names; resolve the framework path from the loaded image; run inside a second application | M0 | M | Source | Feasibility 7 | Patch | The runtime starts inside an application with a different bundle identifier | T-MAC-02 |
| T-PKG-02 | Framework layout and identity from the application: Keychain name, data directory, product strings | M2 | M | Source | Architecture 13 | Layout; start-up configuration | Two applications with different identities keep separate data and Keychain items | T-PKG-01, T-CHR-08 |
| T-PKG-03 | Signing script, and a test that embeds and signs under another team (notarization: T-PKG-06, decided 2026-11-19) | M2 | L | Prebuilt | `parts.py:60-295` | Script; CI test | Helpers launch after re-signing | T-PKG-02 |
| T-PKG-04 | SwiftPM binary-target distribution test | M2 | M | Prebuilt | Published runtime | Package manifest | A clean machine resolves the package; checksum and symlinks are intact | T-CHR-08 |
| T-PKG-05 | Profile snapshot and downgrade policy | M9 | M | Source | `downgrade_manager.cc` | Policy option | Upgrade then rollback follows the configured policy | T-PKG-02 |
| T-PKG-06 | Release procedure by hand: build, package the CLI executable, `pnpm test:release`, sign, notarize, publish, tag. An automated `release.yml`, release-please and the `dev` beta branch are deferred. | M9 | L | Prebuilt | Tooling document 8.1 and 10.2 | Written procedure | A dry run produces a runtime whose digest matches the pin record, and a Swift package that resolves | T-PKG-03, T-CHR-09 |

## Repository and CI

| ID | Task | M | Size | Mode | Inputs | Outputs | Tests | Depends on |
|---|---|---|---|---|---|---|---|---|
| T-REP-01 | Root workspace: pnpm, Nx, project skeletons, root scripts | M1 | M | Prebuilt | Tooling document sections 2 to 4 | `package.json`, `.node-version`, `nx.json`, `pnpm-workspace.yaml`, `project.json` files | Root scripts run on a clean clone; a Node release other than the pinned one fails `pnpm run setup` and names the pin; the project graph matches the table | None |
| T-REP-02 | Formatter and linter configurations and Lefthook | M1 | M | Prebuilt | Tooling document section 9 | Configurations; `lefthook.yml` | A badly formatted staged file is fixed; a staged secret is refused | T-REP-01 |
| T-REP-03 | Pull-request workflow | M1 | M | Prebuilt | Tooling document section 10 | `pr.yml` | Affected checks run; actions pinned by SHA; default permissions read-only | T-REP-02 |
| T-REP-04 | Integration workflow using the prebuilt runtime | M2 | M | Prebuilt | T-CHR-09 | `integration.yml` | Runtime is downloaded and its digest checked against the pin record; integration tests run; a change that alters the source key reports that a rebuild is needed and skips | T-CHR-09, T-SWF-02 |
| T-REP-05 | Local runtime publishing and the pre-release pass. No Chromium build in CI. | M1 | M | Source | Tooling document 10.1 and 10.2 | `chromium:publish`, which records source key, URL, ABI hash and manifest digest in `chromium/pin.json`; `pnpm test:release` | A published runtime resolves on a second machine and in the integration workflow; a tampered archive is refused | T-CHR-08 |
| T-REP-06 | Documentation site on Fumadocs (Next.js, MDX, static export) | M9 | M | Prebuilt | Published API | `apps/docs` | `pnpm docs:build` succeeds; links checked | None |
| T-REP-07 | Conventional Commits check on pull-request titles and in Lefthook. release-please is deferred. | M1 | S | Prebuilt | Tooling document 8.1 | commitlint configuration; pull-request title check | A badly titled pull request fails its check | T-REP-03 |

## Arc-style reference application

| ID | Task | M | Size | Mode | Inputs | Outputs | Tests | Depends on |
|---|---|---|---|---|---|---|---|---|
| T-ARC-01 | Throwaway sketch: sidebar, two spaces, a two-pane split, a move between windows | M0 | M | Source | Feasibility 10 | Sketch; recorded observations | Page state, tab identifier and renderer survive the move | T-ADP-03 |
| T-ARC-02 | Reference application: sidebar, pinned items, spaces, split view, application-owned session state | M8 | L | Prebuilt | SDK from M2 to M5 | `apps/examples/arc-style-browser` | Builds and launches from `pnpm` | T-SWF-04, T-EXT-02, T-WUI-04 |
| T-ARC-03 | Scenario suite for the reference application | M8 | L | Prebuilt | Roadmap M8 table | `tests/arc/` | Every M8 scenario automated | T-ARC-02 |

## Cross-platform and release process

| ID | Task | M | Size | Mode | Inputs | Outputs | Tests | Depends on |
|---|---|---|---|---|---|---|---|---|
| T-XPL-01 | Review the ABI and service contracts against Windows and Linux host requirements and a future .NET binding | M9 | M | Prebuilt | ABI; architecture 14 | Review record; fixes | Each finding resolved or recorded | T-ABI-04 |
| T-XPL-02 | Compile the platform-neutral engine code for a second platform | M10 | XL | Source | Windows or Linux build host | Build target | Target builds | T-XPL-01 |
| T-REL-01 | Security-patch process and a timed rehearsal | M9 | M | Source | T-CHR-10 | Process document | A rehearsal from an upstream security release to a published runtime is timed | T-PKG-06 |
| T-REL-02 | Third-party notices, licence review record, compatibility and caveats pages | M9 | M | Prebuilt | Risk R13; decisions D4, D5, D7 | Notices in the runtime; documentation | Notices are present in the packaged runtime | T-CHR-08 |
| T-REL-03 | Robustness: long-run, crash-recovery and memory tests | M9 | L | Prebuilt | Example applications | Test suite | Suite passes on the release candidate | T-ARC-03 |

## Tasks that settle open decisions

| Decision in [risks.md](risks.md) | Evidence comes from |
|---|---|
| D1 Off-screen page mapping (provisionally (b)) | T-ADP-08 confirms |
| D2 Reputation default (decided: static block lists) | T-PRV-05 implements; feed licences still to verify |
| D3 Alias scheme (decided: accept) | T-WUI-01 verifies |
| D4 Chrome Web Store (decided: named source per application) | T-EXT-09 implements; legal review is each application's |
| D5 Component data mirror (delegated; low recurring cost to BDK) | T-PRV-06 |
| D6 Intel support (decided: Apple Silicon only) | Nothing further |
| D7 Codecs (system decoders) and Widevine (deferred) | T-CHR-12 spike; legal review; T-REL-02 |
| D8 Release train (decided: newest stable) | T-CHR-10 and T-CHR-11 measure roll effort for risk R3 |
