# Amendments to the BDK plan

Dated 2026-11-15; section 2.10 was added on 2026-11-16, 2.12 and 2.13 on 2026-11-19, 2.14 to 2.17 on 2026-11-20, 2.18 on 2026-11-21, 2.19, 2.20 and 2.22 on 2026-11-22, 2.21 on 2026-11-23. This lists everything that changed in the planning documents after the 2026-11-14 version, following the product owner's review. If you read the 2026-11-14 documents, reading this is enough. The other documents already contain these changes; nothing here still needs to be applied.

## 1. Summary

| # | Topic | Before | Now |
|---|---|---|---|
| 1 | Spaces | An open decision asked how "a space" maps onto Chromium | BDK has no space, workspace or group concept. The open decision is about off-screen pages in general. |
| 2 | Swift page registration | Named in the precedence list, not shown | Defined, with an example and limits |
| 3 | Google keys and other APIs | Google providers were "optional, after v1", with no mechanism | Three ways to choose a provider, a runtime mechanism for the application's own key, and a new M6 task |
| 4 | Docs site | "Vite-based static site" | Fumadocs |
| 5 | Chromium tracking | Release from Extended Stable, prototype on stable | One branch, `main`, on the newest stable. A roll bot opens the bump pull request. |
| 6 | Versioning | Not specified | SemVer, one version for all of BDK, Conventional Commits |
| 7 | Chromium builds in CI | A dedicated builder, $350 to $1,100 a month | None. Built and fully tested on a developer's Mac before a release. |
| 8 | Release flow | An automated, attested release workflow | By hand for now. release-please, the automated workflow and a `dev` beta branch are deferred. |
| 9 | SDK surface | Described only in outline, across the architecture document | Written out in [sdk/](sdk/README.md): configuration, Swift API, UI requests, service commands, extensions, JavaScript APIs, C ABI and CLI. A proposal, not yet reviewed. |
| 10 | CLI | Swift recommended, not decided; `bdk init` described in one line | TypeScript, shipped as a Node single executable built on a pinned Node 26. `bdk init` prompts for each choice and has a flag for each. |

## 2. Amendments

### 2.1 No space concept in BDK

The spec says BDK must not prescribe a tab, space, window or workspace model. The plan had drifted from that by wording decision D1 around spaces.

- BDK has no space, workspace, group or tab type, in the API or in configuration. It sees only "this page's view is attached to that window, or to none".
- Decision D1 is now: what do extensions observe for a page that is not on screen?
  - (a) It is always a background tab of the window it was last attached to.
  - (b) As (a) by default, plus one optional generic call that keeps a page in an opaque host context the application creates. Extensions see that context as a separate window.
  - Recommendation: (b) only if the M0 measurements show real extensions care. Otherwise (a) and no extra API.
- The word "spaces" remains only where it describes the Arc-style example application, which is application code. BDK gains no API from that example.

Changed: architecture sections 2, 4.1 and 17; risks D1 and R9; feasibility section 10; roadmap M0.7; task T-ADP-08.

### 2.2 What a Swift page registration is

New subsection in architecture 8.2. A Swift registration sets one route's mode from application code before the runtime starts. Names are placeholders.

```swift
var config = try BDKConfiguration.load()        // reads bdk.resolved.json
config.pages.register("history", .chromium)     // same as history = "chromium" in bdk.toml
config.pages.register("downloads", .native { request in
    DownloadsWindow.shared.show(path: request.path)
    return .handled
})
try BDKRuntime.start(config)                    // the route table freezes here
```

- For a Chromium page it passes no code. It says "serve the built-in page for this route" and outranks a TOML override and an auto-discovered directory.
- Use TOML when the choice is fixed. Use Swift for a choice known only at launch, and for `native` mode, which needs a handler.
- A registration after start is an error.
- Swift can point a route at a bundled page only if that page is in the signed bundle and already in the resolved file. Capabilities for bundled pages still come from `bdk.toml`.
- Clarified: the engine reads `bdk.resolved.json` and nothing else from disk. Swift registrations are applied on top, in memory, before the freeze.

### 2.3 Provider choice and the application's own Google key

New section, architecture 11.1. An application picks a provider per service in one of three ways, all behind the same contract:

| Way | Where | Effect |
|---|---|---|
| Named provider with the application's key | `bdk.toml`; key supplied at start | Uses a provider BDK ships, such as `google`, `deepl` or `web-risk` |
| Endpoint remap | `bdk.toml` | Same protocol, sent to a host the application names, such as its own proxy |
| Application provider | Swift, `provider = "app"` | The application implements the contract and calls any API |

- The key reaches Chromium at start through `InitializeAndOverrideAPIKey()`, enabled by the GN argument `support_external_google_api_key`. One prebuilt runtime serves applications with and without a key. This was read in the source at the pinned tag and has not been built on macOS.
- A key alone is not enough for every service:

| Service | With the application's Google key | Mapping needed |
|---|---|---|
| Geolocation | Works as in Chrome; Google bills the key's owner | None |
| Reputation | Chromium's Safe Browsing client works, but the API terms are non-commercial only | Commercial applications map to Web Risk |
| Translation | Chromium's built-in endpoint appears closed to third-party keys; not confirmed | Map to Cloud Translation or another vendor |

- Unchanged rules: BDK ships no key; requests go to Google only for services the application configured that way.
- New rules: the secret comes from Swift or a build setting, not `bdk.toml`. A key inside an application can be extracted, so restrict it in Google's console or use the proxy route. The network audit runs once with no Google provider and once per configured one.
- New task T-PRV-09 in M6 covers this plumbing. T-PRV-08 (Apple Translation, Web Risk adapters) stays after v1.

### 2.4 Docs site

Fumadocs on Next.js, MDX content, static export. Changed: tooling sections 1, 2, 3 and 10; task T-REP-06.

### 2.5 Branching and the Chromium roll

- One branch. `main` targets the newest Chromium stable through `chromium/pin.json`. No release or maintenance branches, and no second patch set.
- Extended Stable is no longer the plan. Decision D8 is "decided for now: follow stable", to be revisited before v1 with the measured cost of a roll.
- New roll bot: a daily GitHub Actions workflow, `chromium-roll.yml`, on a free hosted Linux runner. It replaces `chromium-rebase.yml`.

| Step | What it does |
|---|---|
| Detect | Reads the current stable version for macOS; stops if it equals the pin |
| Resolve and bump | Resolves the tag to a commit and writes both to `chromium/pin.json` on a branch |
| Check patches | Applies the patch series to a sparse checkout of the new tag, with no build |
| Pull request | Opens or updates one pull request. `feat(chromium): roll to …` for a milestone, `fix(chromium): roll to …` for a point release. A failing series makes it a draft that names each conflicting patch and file. |
| Build | By hand; see 2.7 |

- The bot also opens an issue when the pin is too many days behind stable, because that is a security exposure.
- A script was chosen over Renovate because a roll must resolve the commit and try the patch series. Renovate or Dependabot for ordinary dependencies is not decided.
- Cost of this choice: more frequent, smaller rolls than Extended Stable would give. Risks R3 and R4 are reworded to match.

Changed: architecture decision A9; tooling sections 1, 6.1 and 10; risks D8, R3, R4; new task T-CHR-11 in M1; T-CHR-10 now feeds it.

### 2.6 Versioning

New section, tooling 8.1.

- SemVer, with one version for the Swift SDK, C ABI headers, `@bdk/*` packages, CLI and runtime. The tag is `vX.Y.Z`.

| Bump | Trigger |
|---|---|
| Major | A breaking change to the Swift API, C ABI, `@bdk/*` APIs, CLI contract or `bdk.toml` schema |
| Minor | Additive API or schema; a new Chromium milestone |
| Patch | Fixes; a Chromium point or security release in the same milestone |

- Before 1.0 the version is `0.y.z` and a breaking change bumps `y`. 1.0 is the release that passes the public v1 gates.
- The Chromium version is not part of the BDK version. It is in the runtime manifest and the release notes.
- The `bdk.toml` schema has no number of its own and follows the BDK version. Inside one major version, every earlier valid fixture must still validate.
- `bdk_abi_version()` increases on any incompatible ABI change, which is a major bump after 1.0. The hash is still checked exactly.
- Conventional Commits are checked on pull-request titles and by a new Lefthook `commit-msg` hook (task T-REP-07, M1).

### 2.7 No Chromium builds in CI

Replaces the dedicated builder. Tooling section 10 is rewritten, with new sections 10.1 and 10.2.

- The runtime is built on a developer's Mac and published by hand with a new command, `pnpm chromium:publish`. It records the source key, URL, ABI hash and manifest digest in `chromium/pin.json`.
- CI runs only on free hosted runners and costs nothing.

What CI still tests:

| Needs | Checks |
|---|---|
| Nothing | Lint and format, unit tests, ABI conformance against the stub, schema and resolver fixtures, code generation, docs build |
| Sparse checkout of the pinned tag | The patch series applies |
| The pin record | The ABI hash of the headers matches the published runtime |
| The last published runtime | SDK integration tests, MV3 extension suite, example smoke tests, the signing script with an ad hoc identity |

What CI cannot see:

- Whether a change under `chromium/bdk` or `chromium/patches` compiles or behaves. The integration job reports "runtime rebuild needed" and skips.
- Whether a roll works beyond the patches applying.
- Notarization.

Rules added to keep that gap small:

- Logic that does not need Chromium lives in `packages/core`, where CI tests it.
- A pull request that changes `chromium/` merges only after its author has built locally, run `pnpm test:release` and published the runtime.

Consequences:

- No build attestation. GitHub attestations can only be made inside Actions, so trust rests on the digest committed to `chromium/pin.json` through a reviewed pull request. Attestation returns when a CI build does.
- New risk R30: engine regressions are found late.
- Risk R15 no longer assumes a rented builder. Renting one is now the fallback for when the `dev` branch arrives.

Changed: tooling sections 1, 4, 8, 10, 10.1 and 10.2; architecture section 13; risks R4, R15 and new R30; tasks T-REP-04 and T-REP-05; roadmap M1, M2 and the v1 gates.

### 2.8 Releases by hand

- Before a release, on a developer's Mac from a clean tree: sync, build and package; `pnpm test:release` (new script: unit, integration, MV3, end-to-end, network audit, conformance against the real runtime); sign and notarize the example under a second team identifier; publish, commit the pin record and SwiftPM checksum, then tag. This is tooling section 10.2.
- Deferred and not designed: release-please, an automated `release.yml`, and a `dev` branch that publishes beta prereleases (`X.Y.Z-beta.N`) with `main` carrying stable releases.
- No signing certificate or notarization key is stored in GitHub while releases are cut by hand.
- Task T-PKG-06 is now "release procedure by hand".

### 2.9 SDK surface specification

New directory, [sdk/](sdk/README.md): an index and nine documents. Unlike 2.1 to 2.8, this was not requested as a change to a decision and has not been reviewed by the product owner. It is a proposal.

- It writes out what the architecture document only outlines: every `bdk.toml` key, the Swift API, each browser-owned surface with its request and completions, the full command and capability catalogue, the extensions API, `@bdk/internal/*` and `@bdk/automation`, the C ABI's header layout and the CLI.
- All names are placeholders, as the spec requires. Nothing was built or compiled.
- It changes no existing decision, task or milestone. The only existing files edited are the planning index and this one.
- It proposes twelve additions (S1 to S12) that a production browser needs and the plan did not have, among them address-bar services, stable storage identifiers that survive a rename of the browser, eight further browser-owned surfaces, and a fourth CLI command for the Xcode build. They are listed with reasons in [sdk/README.md](sdk/README.md) section 6. None has a task yet.
- It proposes five scope statements, such as no installable web apps in v1 and no page capability for driving other pages, in the same section.
- It records nine new unverified assumptions, in [sdk/README.md](sdk/README.md) section 8.
- Where it departs from the spec's illustrative configuration and CLI, the differences are listed in [sdk/configuration.md](sdk/configuration.md) section 10 and [sdk/cli.md](sdk/cli.md) section 10.

### 2.10 CLI in TypeScript, shipped as a Node executable

Product-owner decisions from the 2026-11-16 review of the SDK documents.

- The `bdk` CLI is written in TypeScript, not Swift, so the same tool can serve Windows and Linux hosts later. It has a platform-neutral core and one module per host platform. All Xcode work is in the `xcode` module. Nothing in the CLI needed Swift: each macOS step runs an Apple tool or edits a text file.
- It ships as one self-contained executable per platform, built with Node's own `node --build-sea` and no wrapper package. A developer who writes no JavaScript still installs no Node.
- The repository pins one Node 26 release, 26.9 or later, in `.node-version` (read by fnm) and in `engines`. That release is also the runtime inside `bdk`.
- The executable cannot install packages, so a project with Vite pages needs Node and a package manager.
- New task T-CLI-05 in M2 builds the executable and checks it: no Node on the machine, the Swift package artifact bundle, an Xcode build phase, start-up time, embedded assets, signing. Nothing has been built yet. Bun's `bun build --compile` is the fallback (risk R31).
- `bdk init` prompts for each choice, has a flag for every prompt, and offers page templates per JavaScript framework. T-CLI-02 grows from M to L.
- Not part of this amendment: additions S13 to S16 in [sdk/README.md](sdk/README.md) section 6, which are proposals waiting for the product owner.

### 2.11 Product-owner decisions on D1 to D8

Recorded 2026-11-16. The table in [risks.md](risks.md) section 1 carries the detail.

| Decision | Outcome |
|---|---|
| D1 Off-screen pages | Provisionally (b): background tab of the last window by default, plus the optional host-context call. Confirm after the M0 probe. |
| D2 Reputation default | (a) Signed static block lists, as the `"default"` reputation provider. |
| D3 Alias scheme | (a) Accept the alias. |
| D4 Chrome Web Store | (b) A named source each application enables. New requirement: the store's install button reads "Add to <browser name>". No task yet; belongs with T-EXT-09. |
| D5 Component data mirror | Delegated to the implementation agent after T-PRV-06, with low recurring cost to BDK as the constraint. |
| D6 Intel Macs | (a) Apple Silicon only for v1. |
| D7 Codecs and Widevine | Codecs: decode H.264, AAC and HEVC through the operating system's decoders, with no bundled proprietary decoder and one runtime. Subject to the new spike T-CHR-12 (M2) and the legal review. Widevine: deferred by the product owner; not in the plan. |
| D8 Release train | (b) Newest stable. |

Propagated to architecture 1, 4.1, 8.3, 10 and 11; tooling 6.1; roadmap G0, M2 and M9; tasks T-CHR-12 (new), T-EXT-09 and the decisions table; `sdk/configuration.md`, `sdk/extensions.md` and `sdk/production-browser.md`. The D1 and D5 items in the SDK documents stay "Conditional". The task list is now 108.

Still waiting for the product owner: additions S1 to S16 and the five scope statements in [sdk/README.md](sdk/README.md) section 6, and Widevine.

### 2.12 M0 skipped; M2 built on `WebContents` (2026-11-19)

The product owner asked for M0 to be skipped and M2 completed. What that changed:

| Planned | Built in M2 | Consequence |
|---|---|---|
| Gate G0 and the M0 evidence (T-ADP-01, T-ADP-02, T-ADP-04, T-ADP-08, T-EXT-01, T-WUI-01, T-PKG-01) before M2 | Not run. T-MAC-01 and T-MAC-02 were done as part of M2, in their final form (no throwaway spike kept). | D1 stays provisional; the extension, WebUI and services evidence of M0 is still owed, now by M3 to M5. |
| Pages as tabs of `Browser` host contexts with a BDK `BrowserWindow` (architecture 4.1 and 4.2, T-ADP-01 to T-ADP-03) | Each page is a `WebContents` the engine owns directly, with BDK as its delegate. No `Browser`, tab strip or BDK window exists. | Chrome's tab helpers are not attached, so tab-scoped services (permission prompts, find, translate, the extension tab model) are absent. M3 must add the host contexts (T-ADP-01 to T-ADP-03) before T-ADP-04 and the services; the C ABI does not change for it. |
| `NSApplication` hook patches (feasibility 2) | `BDKApplicationBase`, exported by the framework, subclasses Chromium's `BrowserCrApplication`; the Swift SDK's `BDKApplication` subclasses it. Chromium's loop runs nested in the application's, started from a run-loop timer; quitting cancels and re-terminates (`BDKRuntime.terminate(replyingTo:)`). | Applications must use `BDKApplication` (or a subclass) as their principal class. |
| The stub and the engine as separate implementations of the ABI | One runtime core (`packages/core/runtime`) with two backends, the stub's and the engine's, so the conformance suite tests the same semantics on both. | Adding an ABI function means adding it to the core once and to a backend if it needs one. |
| Conformance against the real runtime (T-ABI-04) through ordinary ABI calls | Through test hooks the engine exports (`chromium/bdk/engine/bdk_testing.h`, not ABI): scripted navigations without the network, UI requests raised for the application's handler, script with a user gesture, clicks. | The shipped framework carries the hooks. They act only as the application, on its own pages; no page or session can reach them. |
| Undocked DevTools through Chrome's `DevToolsWindow` | Chrome's bundled front end in a BDK page with `DevToolsUIBindings`, raised as a new-window request of kind `devTools`. | Docked DevTools and `DevToolsWindow` features (docking, the eye dropper) are absent until a host context exists. |

Notarization and a real Developer ID identity are not M2 work: they happen when a release is cut (T-PKG-06, M9), by the product owner's decision of 2026-11-19. T-PKG-03 is complete with ad hoc signing.

Seven patches are in the series (`chromium/patches`); each message says why. Three behaviours of the suite moved towards Chromium's: a failed navigation commits an error page for its URL, a title event follows only a title change, and the stub's text session format is tested only against the stub.

### 2.13 Host contexts built in M3 (2026-11-19)

M3 Phase 1 added the host contexts that 2.12 left for it (T-ADP-01 to T-ADP-03): every page is a tab of a `Browser`. The C ABI did not change. Where the build departs from architecture 4.1 and 4.2, feasibility 1 and tasks T-ADP-01 to T-ADP-03:

| Planned | Built | Why |
|---|---|---|
| A third branch in `BrowserWindow::CreateBrowserWindow` and in `BrowserWindow::FromBrowser` | One patch: a settable factory (`embedder_browser_window_factory.h`) that `CreateBrowserWindow` asks first. `FromBrowser` needed nothing: it already falls back to `Browser::GetWindow()` | `BrowserWindowCreateParams::window` is test-only and cannot work for an embedder (a window is built with its `Browser`). The factory touches one `.cc` and adds one header that nothing widely included sees |
| A third arm in `BrowserWindowFeatures` initialisation | None. `BrowserWindowFeatures` already guards a missing `BrowserView` and a missing `WebUIBrowserWindow` (`StubBrowserFocusController`, null checks) | Found by running it: Browser creation, tab insertion, moves and closing worked with those |
| "Null guards where non-Views code casts to `BrowserView`" | One patch (a side-panel `CHECK_IS_TEST` that upstream relaxes only for Webium), plus real objects in the window | See the list below |
| One host context per window | One per window **per profile**, plus one parking context per profile | A `Browser` belongs to one profile; a window may show pages of several |
| Private profiles unchanged | One patch: `OTRProfileID::AllowsBrowserWindows` accepts BDK's profile ID prefix | Chromium bans a `Browser` in every non-primary off-the-record profile except DevTools, Media Router and captive portal; BDK's private profiles are each their own off-the-record profile, so a shared primary profile was not an option |
| BDK as the page's delegate | Unchanged: after every insertion, move and replacement the engine sets BDK's delegate again, because the `Browser` takes it with the tab. Chrome's `BrowserWebContentsDelegate` is not forwarded to yet | Keeps every M2 behaviour (new-window requests, fullscreen, context menus, keys, `window.close()`) without Chrome's delegate opening windows. Phase 2 forwards the calls a service needs |

Found by running it (T-ADP-02; the Chromium `browser_tests` filters were not run, since no test build exists):

| Where | What | Handling |
|---|---|---|
| `ReadAnythingSidePanelController::TabWillDetach` | `CHECK_IS_TEST()` when a detached, active tab's `Browser` has no `SidePanelUI`, unless Webium is on. Crashed the first tab removal | Patch: also accepted when an embedder supplies windows |
| `Browser::OnTabDeactivated` | Calls `window_->GetLocationBar()->SaveStateToContents()` without a null check | `HostLocationBar`, a stub `LocationBar` |
| `~BrowserWindow` contract | A window must call `Browser::GetFeatures().TearDownPreBrowserWindowDestruction()` before it goes, or `BrowserLiveTabContext` holds a dangling `raw_ref` to it (dangling-pointer detection aborts) | Done in `~BdkBrowserWindow`, as `WebUIBrowserWindow` and `BrowserWidget` do |
| `Browser::TabStripEmpty` | Calls `window_->Close()` whenever the tab strip empties, which happens at every move out of a context | `Close()` does nothing; BDK closes a context itself (`BrowserManagerService::SynchronouslyDestroyBrowser`) |
| `Profile::AllowsBrowserWindows` | See the private-profile row | Patch |
| `ExclusiveAccessContext`, find bar, tab-modal dialog host | Dereferenced where a feature starts | Stub implementations; behaviour is Phase 2 |
| `tab_strip_model_event_bridge.cc:111` | Upstream `NOTIMPLEMENTED` on a tab strip change | Not ours; logged once |

No `BdkBrowserWindow` method that logs (`NOTIMPLEMENTED_LOG_ONCE`) was reached by the 144 conformance and engine tests.

Four patches were added or changed: the series is ten (the test-hook export patch also lists `bdk_testing_host_context_count` and `bdk_testing_tab_info`).

### 2.14 Internal pages built in M4 (2026-11-20)

M4 was written with no Chromium build: the parts that need none are tested, and the engine parts wait for the main session's build (roadmap M4 status). Where the build departs from architecture 8 and 9, feasibility 4 and 5, and the SDK documents:

| Planned | Built | Why |
|---|---|---|
| T-WUI-01 (M0): an alias-scheme prototype before M4 | Built in its final form in M4: the scheme comes from `browser.internal_scheme`, the browser gets `--bdk-internal-scheme` at start and copies it to every child, and a new patch registers it as standard and secure in `ChromeContentClient::AddAdditionalSchemes` | Child processes run Chrome's own `ChromeMainDelegate` and `ChromeContentClient`, so no subclass can add a scheme there; Chromium registers schemes once per process, so a second runtime in one process keeps the first's |
| A route registry in the engine (T-WUI-02) | The route table, its registrations and every rule are in the core (`packages/core/runtime/routes.h`); the engine applies the core's answers in a `BrowserURLHandler` pair and a navigation throttle | One set of rules, tested on the stub and the engine by the same conformance tests |
| A disabled page shows "the standard error page" | `ERR_BLOCKED_BY_CLIENT` for a disabled route (and a native handler answering blocked), `ERR_INVALID_URL` for an unknown one. A web page's or a redirect's navigation to a branded URL is dropped with no error page, as Chromium drops a web page's `chrome://` link | Chromium's own codes, which every backend reports |
| "No alternate route" to an overridden built-in page | Every `chrome://` navigation of a BDK page must be the internal URL its route is served at now; anything else (an overridden or disabled built-in, a page that is no route, an excluded page) shows the error page. Frames inside Chromium's own pages are not policed | Applies to pages BDK owns; Chromium's DevTools, print and extension pages are left alone |
| Branded URLs in history and session data | Every URL the engine reports is its branded form. Internal pages are not recorded in history, as in Chrome; session data is opaque and restores the internal URL. An alias is shown as its target route | Chromium's history excludes `chrome://`; an alias shares the target's identity |
| BDK's new-tab page (configuration.md 4.1) | Served at `chrome://bdk-new-tab` when `new-tab` is in `chromium` mode; a bundled `new-tab` page uses the same host | A route has one mode at a time, so the host is never ambiguous |
| Swift registration of a route | Any valid route name may be registered `native` or `disabled`; `chromium` needs a built-in; `bundled` needs the route's page in the resolved file; `credits` cannot be disabled; an alias or reserved name cannot be registered | architecture 8.2's limits, made exact |
| The broker reached by "a Mojo interface bound to that page" | `bdk.mojom.PageBroker` (`chromium/bdk/engine/pages/page_broker.mojom`): Invoke, Cancel, Subscribe, Unsubscribe and a client for progress and events, registered for `BundledPageUI` only. Its WebUI bindings are compiled by Chromium's TypeScript build and embedded in the engine, served at `/@bdk/page_broker.mojom-webui.js` | Bindings that always match the runtime, without changing the framework's resource packs |
| The gesture of a page's call | The frame's transient user activation, read by the browser at the call; the page cannot claim one | A page's message is not evidence of a gesture |
| Content security policy "Chromium's standard for WebUI" | Chromium's, made stricter: `default-src 'self'`, scripts, styles and fonts from the page and `chrome://resources`, images also `data:` and `chrome://favicon2`, no frames, no forms, Trusted Types required with no policy | javascript-api.md 2.2; frameworks that assign strings to sinks need a policy, still the open choice of sdk/README.md section 7 |
| The SDK served at `/@bdk/internal/` | `bdk build pages` copies the built `@bdk/internal` modules into `Contents/Resources/BDK/js/internal`; the Vite preset leaves `@bdk/internal/*` imports external, so every page uses that copy | One module for Vite and no-build pages |
| Vite builds of all pages at once | One Vite build per page directory | A multi-page build put shared chunks above a page's directory, outside its own origin |
| T-UIR-08: host print preview, or record the system panel | **The system print panel is the permanent default.** Print preview is not hosted | `PrintPreviewDialogController` places the preview in a `WebContentsModalDialogHost` of the `Browser`'s window through Views' constrained web dialog; the BDK window has no Views widget and its modal host is a stub. PDFs come from `contents.pdf` (T-SVC-09, M7) and the panel's own Save as PDF |
| `page.zoom` is a number | `page.zoom` is a `ZoomControl` (`factor`, `set(_:scope:)`); reading the level is `page.zoom.factor` | swift-api.md 6.7's `page.zoom.set(...)` cannot live on a `Double` |
| Generated bindings "are Nx outputs and are not committed" (repository-and-tooling.md 11) | Split by who reads them. Committed, and checked by `js-codegen:codegen-check`: `interface/ids.json`, `bdk_ids.h`, `generated/bdk_command_table.inc` and `BDKServices.swift`. Git-ignored, and written by `js-codegen:codegen`, on which every `js-internal` target depends: the `@bdk/internal` service modules. `@bdk/automation`'s service modules (T-JS-04, M7) are to be handled like `@bdk/internal`'s | The registry is the record of every identifier ever assigned, not a derivation; the Chromium GN build and SwiftPM compile the header, the tables and the Swift wrappers and cannot run the generator. Only Nx targets read the TypeScript modules, which are one function per command, so a definition's diff already shows the change |

The patch series is 15: the new scheme patch, and the C ABI export patch lists the six new functions (the test-hook export patch is regenerated against it). `chromium:patch-check` passes.

### 2.15 Providers and privacy in M6 (2026-11-20)

M6 departs from architecture 11, 11.1 and 12, feasibility 6 and tasks T-PRV-02 to T-PRV-09 as follows. The engine half is written, not built.

| Planned | Built | Why |
|---|---|---|
| A BDK renderer agent replacing script injection (architecture 11) | Two scripts run by the browser in the translate isolated world of the main frame read and write text nodes; no renderer code | The browser-process API (`ExecuteJavaScriptInIsolatedWorld`) needs no patch and keeps the page's script away from them. Frames other than the main frame are not translated |
| Chromium's language detection reused | Reused, but on macOS it needs a TFLite model the optimization guide downloads from Google; BDK does not fetch it, so detection rests on the page's declared language or the provider's `detect` | No Google request by default (feasibility 6) |
| The Google key "supplied at start" | `google_api_key` in `bdk_start_config_t`; `bdk_provider_set_key` takes no key in this runtime | Chromium allows the key to be set once, before anything reads it (`InitializeApiKeyCacheInstance`) |
| Endpoint remap for every provider | Only for the default reputation lists; Safe Browsing's and the network location provider's hosts are constants (T-PRV-14, after v1) | No overlay hook reaches them |
| A component-data mirror (D5 options a to c) | The data ships with each runtime release and the component updater is off | Update-check responses are signed by Google per request (CUP) |
| `translation.*` from services.md 5.12 | Plus `translation.present`; every command takes a profile (a call that names a page addresses the page's profile) | Swift's `page.translation.present()` needs a command; the generator puts one namespace on one scope |
| A translation request that moves through its phases (ui-requests.md 5.10) | The request is the offer; later phases are events | A request is completed once |

New tasks: T-PRV-11, T-PRV-12 and T-PRV-13 (M6), and T-PRV-14 (after v1). The task list is 128.

### 2.16 Automation and DevTools built in M7 (2026-11-20)

M7 was written with the engine unbuilt: the parts that need no Chromium pass their tests, the engine's are written
against the pinned source and wait for a build (T-AUT-06). Where it departs from architecture 9.3, javascript-api.md
5 to 8, c-abi.md 9, services.md 5.15 to 5.17 and ui-requests.md 5.15:

| Planned | Built | Why |
|---|---|---|
| A runtime for scripts chosen by a spike | V8 in a `kService` utility process for bundled scripts, an external runtime on the local socket for external tools, one JSON protocol for both ([feasibility.md](feasibility.md) section 12) | Both were needed anyway (T-AUT-04 is the external one); one protocol keeps one set of checks |
| Automation in the engine (architecture 2, "automation host endpoint") | Sessions, tasks, the protocol, the endpoint, the user-script registry and the `contents.*` commands that need no renderer are in the runtime core; the engine only runs scripts and renderer work | Logic that needs no Chromium lives where CI tests it (2.7); the stub and the engine cannot disagree |
| `@bdk/*` left external, provided by the host (javascript-api.md 8) | `bdk build automation` inlines `@bdk/automation`, `@bdk/commands` and `@bdk/events` into each script: one ES module with no imports | The host then provides only a transport; the runtime carries no copy of the JavaScript library, and the protocol is versioned in `start` and `welcome` |
| `bdk_automation_subscribe_audit` (c-abi.md 9) | The built-in events `automation.sessionCreated`, `sessionRevoked` and `commandInvoked` (capability `automation.audit`) | One subscription mechanism; an auditing session can be granted them |
| `bdk_automation_endpoint_open(authenticator_cb)` returning a session | The hello reaches a callback with a `bdk_context_t`; the application answers later with `bdk_automation_hello_accept(session)` or `_refuse` | Authentication may be asynchronous (an application asking its own server) |
| A session's profiles | Open profile ids resolved to handles when the session is created: a profile closed and reopened leaves the scope | Scope stays a set of handles in the broker; reopening is a new grant |
| Undocked DevTools as a new-window request of kind `devTools` (2.12) | A `devtools` request carrying the front end as a page (ui-requests.md 5.15), with an undocked window of BDK's own as the default presenter | The surface the spec defines; `[ui.surfaces] devtools` now governs it |
| A replaced front end "served in place of the default" | Chromium's `--custom-devtools-frontend` switch, set at each start on the bundle's `BDK/DevTools/Frontend/`, entry `index.html`; packaged by `bdk build devtools` | Chromium's data source reads the switch per request: no patch |
| A slow subscriber's events coalesced (services.md 7) | A connection 64 MiB behind is closed with `automation.overloaded` | Simple and safe for v0; coalescing came in M8 (T-AUT-07, section 2.18) |

New in the ABI: `bdk_session_run`, `bdk_task_cancel`, `bdk_task_subscribe`, `bdk_automation_endpoint_open` and
`_close`, `bdk_automation_hello_accept` and `_refuse` (the export patch lists them). New patch: "bdk: register the
automation host in the utility process" (Chromium has no embedder hook for utility services). With M4's two patches, the series is 17.
A saved page (`contents.save`) appears in the profile's downloads, because Chromium saves pages as downloads.

### 2.17 Extensions written in M5 (2026-11-20)

M5's engine code is written and not yet built; the rest passes without Chromium. Where the build departs from
[sdk/extensions.md](sdk/extensions.md), [sdk/swift-api.md](sdk/swift-api.md) 8.1 and tasks T-EXT-01 to T-EXT-09 and
T-SWF-08 (detail in `packages/core/docs/conventions.md`, "Extensions (M5)"):

| Planned | Built | Why |
|---|---|---|
| Extension surfaces named by their `[extensions.ui]` keys | Eight surfaces, 25 to 32: the six keys plus install and uninstall prompts, which cannot be disabled | The keys `context_menus` and `devtools` are already `[ui.surfaces]` names; the prompts need a surface and must not be switched off |
| A custom surface needs a handler | Not for `actions`, `context_menus`, `options_pages` and `devtools` | They raise no request of their own |
| `accept()`, `sizeChanges`, `side`, `visibilityChanges`, `closed` on popups and side panels | A popup or panel is a request that stays open while its view is up; `bdk_ui_update` and `bdk_ui_set_update_handler` carry changes both ways; a withdrawal is `closed` | One request per view keeps the router's rules (one at a time, withdrawal, release) |
| `activateAction`, `performCommand`, `presentDefaultMenu` as calls | C functions in `bdk_extensions.h` that take the confirming event | A command from the host carries no gesture |
| `extensions.actions(for:)` synchronous | `extensions.actions`, an async command with the page in the call's options | Every other read is a command |
| Install and update checks in the engine | `runtime/extension_policy` in the shared core: source policy and manifest inspection for the stub and the engine | The same refusals on both, tested in CI |
| `tabGroups` absent | Degraded: it works on the host context's tab model and the application shows no groups | Chromium's implementation needs nothing from BDK; hiding it would need a patch for no gain |
| MV2 | Not loadable: Chromium 158 disables every non-component MV2 extension in every build (`manifest_v2_handler.cc`) | `EngineFacts.manifest_versions` is `{3}` |
| Default extensions menu placement "open" | A trailing title-bar accessory button when actions are `default` and the window has no anchor provider; `bdk_extension_menu_present` from the application's own menu | Settles extensions.md section 11 |
| The parking context, open (T-EXT-01 assertion 10) | Hidden from the extension windows API by a patch | Chromium lists it and fires `windows.onCreated` for it, which wakes service workers |
| Side panel needs a `BrowserWindowFeatures` patch (T-EXT-03) | No patch: `chrome.sidePanel.open` ends at the abstract `SidePanelUI::Show`, which BDK implements | Smaller than the planned patch |

Three patches were added, each with its reason in its message: extension install and uninstall prompts, keeping a
`Browser` out of the extension window list, and opening an extension's options page. With M4's and M7's patches, the series is 20.
`developer_mode`, `blocklist` and `[extensions.bundled]` (S6) are still Added and not read (T-EXT-14).

### 2.18 Engine scenarios fixed after the M4 to M7 merge (2026-11-21)

The fix-up of roadmap section 6 took these decisions where the documents were silent or Chromium does otherwise:

| Planned or assumed | Decided | Why |
|---|---|---|
| A secure view's lifetime (not written down) | The runtime's: valid until its request finishes, then taken out of the application's container and possibly destroyed (`bdk_ui.h`, conventions.md "Extensions (M5)" item 3) | The engine frees the panel's view with its host; an application holding it longer used freed memory |
| "One extension panel is open per window; opening another closes the first" (extensions.md 4.3) | Kept, but a window panel opened while the active page shows its own panel leaves that panel, as Chromium does (`OpenGlobalExtensionSidePanel`) | BDK follows Chromium's choice of which panel a `Show()` means |
| `activeTab` "revoked when it navigates" (extensions.md 4.1) | Revoked on a navigation to another origin; kept within the origin (extensions.md 4.1 now says so) | Chromium's `ActiveTabPermissionGranter` |
| `checkForUpdates` applying an update | Installed at once, even while the old version is running | It is an explicit check, as Chromium's Update button in `chrome://extensions` is; otherwise the update waits for the extension to be idle |
| `documentStart` for user scripts (services.md 5.16) | Runs once the document's root element exists, before the page's first script; a document loaded right after a change waits for it | Chrome's `document_start`; a DevTools new-document script runs before the root element exists |
| Chromium's start-up completion (T-ADP-10) | Complete once `bdk_runtime_start` has returned and the main loop is idle (`StartupInProgressRef` plus `ImprovedStartupBestEffortDelay` with no wait for a visible tab) | Chromium waited for a visible tab of a window BDK never opens, then for its three-minute failsafe |
| The macOS location permission for provider = "app" (T-PRV-04) | Granted, as Chromium sees it, while the runtime uses the application's provider; the operating system's otherwise | It guards the operating system's provider; a page still needs the page's permission |
| A host context's `Browser` when its tab strip empties | Kept: BDK cancels the close Chromium starts, as Chromium cancels one the person declines | A host context outlives its tabs; the M8 branch fixes the same in `host_context.cc`, and the second to merge drops one |

The window-filter patch also gates `IsVisibleToTabsAPIForExtension`, so the series is still 20 patches. New test switch:
`BDK_ENGINE_HOST_RESOLVER_RULES` (chromium/bdk/README.md); new test verbs: `network.passthrough`, `startup.post`,
`startup.state` (`bdk_testing.h`).

### 2.19 WebAuthn requests built ahead of S3 (2026-11-22)

`navigator.credentials` ended the embedding application whenever Chromium's authenticator dialog had to appear (a
tab-modal views widget with no parent, `native_widget_mac.mm`), so the `webauthn` surface was built before the
product owner has decided S3 (T-UIR-09, M4). Where the build departs from [sdk/ui-requests.md](sdk/ui-requests.md) 5.18
as proposed:

| Planned | Built | Why |
|---|---|---|
| One `WebAuthnRequest` whose `step` changes | One request per step, with a `ceremonyID`; the next step withdraws the last | Chromium moves on by itself (a key touched, a wrong PIN); a request per step keeps the router's rules (one at a time, withdrawal, release) and needs no new ABI |
| Steps `.chooseMechanism`, `.touchSecurityKey`, `.enterPIN`, `.chooseAccount`, `.error` | Also `.connectPhone`, `.confirm` and `.other`, with `detail` (Chromium's step name) and Chromium's own text and button labels | These are the steps Chromium's dialog model reports; its sheet models supply the wording a custom presenter must show |
| `chooseAccount(_:)` | `chooseAccount(_:confirmedBy:)`, a `grant` | It releases a credential to the site: architecture section 7 says a credential prompt is not granted without a gesture |
| Default presenter: Chromium's WebAuthn dialog | A sheet of BDK's own, from the same payload; Chromium's dialog is never shown on a BDK page | Chromium's needs a views widget (feasibility section 11, T-MAC-04). A new patch lets an embedder supply the dialog's controller |
| `disabled` in `bdk.toml` | The runtime honours `disabled`; the `[ui.surfaces] webauthn` key still waits for S3 with the other seven | Accepting one S3 key is the product owner's call; meanwhile `WebAuthnRequest.cancelAll` turns passkeys off |
| Platform passkeys "unverified" | Security keys built (tested with a virtual key); phones and the system provider are Chromium's paths, unverified (T-UIR-12, M9); Touch ID not offered (T-UIR-11, M9) | Chromium's Touch ID keychain group is compiled in; phones need Bluetooth keys in `Info.plist`; the system provider needs Apple's browser entitlement |

New: `BDKWebAuthnSupport` (what each mechanism needs, read from `Info.plist` and the signature), the CLI note
`plist.passkeys_phone`, the test verbs `webauthn.*` (a virtual CTAP2 key through content's
`ScopedAuthenticatorEnvironmentForTesting`, no switch) and `present.script` answers for `webauthn`
(`bdk_testing.h`). The series is 21 patches; the task list is 143. HTTPS-First's "ask before HTTP" dialog fails the same way (`tab_dialog_manager.cc`,
`Check failed: host`) and is T-UIR-10, M4.

### 2.20 Chromium's other browser dialogs on BDK pages (2026-11-22)

T-UIR-10 listed every caller of Chromium's tab-modal views dialogs and bubbles (`TabDialogManager`,
`constrained_window::ShowWebModalDialogViews`, `chrome::ShowTabModal`) and found which a BDK page can reach; the list
is [sdk/ui-requests.md](sdk/ui-requests.md) 8.2. Each one reached failed a check, because a BDK window is not a views
widget (feasibility section 11). None of their surfaces is routed before S3, so the defaults are BDK's:

| Planned ([sdk/ui-requests.md](sdk/ui-requests.md)) | Built | Why |
|---|---|---|
| `external_protocols`: default presenter Chromium's confirmation dialog | A sheet of BDK's own with Chromium's wording, Cancel the default; a patch gives the dialog to the engine once Chrome has decided to ask | Chrome's dialog cannot be parented. Chrome keeps the decision (blocked and allowed schemes, `mailto:` without asking, policy, gestures) |
| `client_certificates`: Chromium's certificate selector | Chrome's automatic selection, then a sheet with one button per certificate (the first eight) | The selector is a views table; no patch is needed (the browser client's `SelectClientCertificate`) |
| `device_choosers`: Chromium's chooser dialogs | Refused, as a cancel: `requestDevice()` and `requestPort()` reject, HID resolves empty. Web Bluetooth is off at start | The chooser bubbles dereference a `BrowserView`. Web Bluetooth's adapter would also need the application's Bluetooth usage string |
| `screen_capture`: Chromium's media picker | Refused: `getDisplayMedia` rejects with NotAllowedError, as `disabled` would | The picker is a tab-modal views dialog |
| HTTPS-First's "ask before HTTP" dialog (not in the plan) | `HttpsFirstDialogUi` off: HTTPS-First asks with its interstitial page, in the page | The interstitial is a page; the dialog is views |
| Payment Request, Secure Payment Confirmation, FedCM, Digital Credentials (not in the plan) | Off at start (`WebPayments`, `SecurePaymentConfirmation`, `FedCm`, `WebIdentityDigitalCredentials*`): sites see them as unsupported | Their UI is views only and no surface exists for them |
| Page-initiated web application installs (not in the plan) | Off: `profile.web_app.install_by_user_enabled` defaults to false | The install dialog is views; a BDK application owns its windows |
| Tab-modal dialog models (not in the plan) | A sheet: title, paragraphs, OK and Cancel, the model's default first; a patch gives `chrome::ShowTabModal`'s model to the engine | File System Access, DevTools and extension notices use it; a model's own callbacks decide. A model with a field a sheet cannot show is closed as dismissed |
| The Credential Management API's account chooser and auto sign-in notice (not in the plan) | Sheets of the `passwords` surface, through a patch; Chrome's controller decides | `navigator.credentials.get({password: true})` reached them |

This was written against the source at the pin and first built and run on Chromium at the M8 merge (2026-11-22),
where the 7 scenarios of `tests_engine_dialogs.c` pass after two fixes: the account chooser and auto sign-in sheets
drop their controller before answering (Chrome's `OnCloseDialog` destroys it at once and deletes the prompt later,
without `ControllerGone()`, so the sheet held a dangling `raw_ptr`), and the HTTPS-First scenario lets its host reach
the network (the harness served it before HTTPS-First could upgrade it). The client-certificate sheet has no scenario
yet (T-UIR-10, M4). Routing what is refused or off waits for S3 (T-UIR-13, M9). The series is 24 patches; the task
list is 144.

### 2.21 Developing Cove against unreleased BDK (2026-11-23)

The product owner asked how Cove, in its own repository, should take BDK while both change. The answer is [repository-and-tooling.md](repository-and-tooling.md) section 7.3:

| Considered | Decided | Why |
|---|---|---|
| BDK as a Git submodule of Cove | No submodule. A `bdk-main` worktree of this repository that only tracks `origin/main`; Cove's `bdk:sync` pulls it, builds the `js-*` packages and resolves the runtime | One BDK clone, its hooks and `BDK_HOME`; no pointer bumps; commits cloud agents push reach Cove on the next sync |
| Swift from GitHub, JavaScript and runtime from local | Swift from `bdk-main` too, through `.package(path:)` when `BDK_LOCAL` is set; a pinned `.package(url:revision:)` otherwise | Swift, JavaScript and the runtime from one commit; a newer Swift package against an older runtime fails at start on the ABI hash |
| `@bdk/*` installed from a GitHub link | `link:` to `bdk-main` | Their `dist/` is not committed and they build through Nx, so a Git install has no code. A CI-built branch would fix that; deferred |
| A Chromium build per consumer | None: Cove resolves the runtime from `BDK_HOME` through `bdk-main/.bdk/runtime`; a changed patch or overlay stops the sync instead of building | The store is shared by source key (section 7.1) |

Publishing a runtime (`chromium:publish`) runs locally and uses no GitHub Actions; it is still left to the product owner, and `pin.json` records none. Nothing was set up or built for this: the worktree, the script and the switch come with Cove's repository.

### 2.22 The Arc-style reference application in M8 (2026-11-21, merged 2026-11-22)

M8 built `apps/examples/arc-style-browser` (T-ARC-02, T-ARC-03) on the public SDK only, settled T-ADP-06, and
finished the M7 leftovers T-AUT-07 and T-JS-06. BDK gained no space, split or tab API; the one new public
identifier is the error code `automation.budgetExceeded`. Where the build departs from roadmap M8, risks R2,
feasibility 10, tasks T-ARC-02, T-ARC-03, T-ADP-06 and T-AUT-07, and the SDK documents:

| Planned | Built | Why |
|---|---|---|
| T-ARC-03's scenario suite in `tests/arc/` | In the application's package: `Tests/ArcBrowserTests` (every M8 scenario as the application performs it, on the stub, in process, run by CI) and `Tests/ArcBrowserE2ETests` (the built bundle driven with `--do` actions); and on the engine, `packages/core/tests/tests_engine_arc.c` (the same view moves in windows the harness registers) | A SwiftPM target cannot reach outside its package, and Chromium's half needs the engine harness's registered windows and test hooks |
| Exit criterion: "every scenario passes in CI" | Every scenario passes in CI on the stub, as the application's behaviour; what Chromium makes of it passes on a developer's Mac | CI builds no Chromium (section 2.7) |
| Two visible pages represented as an upstream split, so both are foreground (swift-api.md 8.2); three or more patched through a "presented" predicate (risk R2) | No upstream split. Every page on screen is foreground for BDK's prompts whatever the number of panes, and extensions see the focused pane as the window's active tab and the others as its background tabs | BDK's find (`FindTabHelper`), JavaScript dialogs (its own `JavaScriptDialogManager`) and translation offers (its own request) never ask for the active tab. Of the sites feasibility 10 lists, `ChromeTranslateClient::ShowBubble`, `FindBarController` and `JavaScriptTabModalDialogManagerDelegateDesktop::IsWebContentsForemost` are not reached; `PermissionRequestManager` is, and a new patch lets an embedder have its prompts follow page visibility. An upstream split holds two tabs and would make two panes behave unlike three |
| T-ADP-06's output: "patch; list of sites" | The list above and one patch ("let an embedder have permission prompts follow page visibility"); `tabs.query({active: true})` is unchanged and documented | The rest need no patch |
| Session restoration "needs BDK to keep Chrome's session service from restoring windows" (feasibility 10) | Nothing further: after a relaunch no window and no tab exist until the application makes them (`engine_arc_after_a_relaunch_chromium_restores_nothing_and_the_application_does`) | Host contexts are left out of session restore since M3 (section 2.13) |
| "CPU and memory budgets for the host are open" (javascript-api.md 5.4) | `[automation] cpu_budget_ms` (default 60000): the CPU time one task's script may use. The host measures each entry into the script and a watchdog thread terminates a run at the budget left; the task ends with the new code `automation.budgetExceeded`, which a script's own `failed` frame cannot claim | A key in `bdk.toml` lets an application and a test choose it; `BDK_CODE_BUDGET_EXCEEDED` changes `bdk_ids.h`, so the ABI hash is regenerated (no runtime is published, so the pin record has nothing to compare) |
| A slow client's state events coalesced instead of closing it | Past 4 MiB waiting, a queued `contents.loadingChanged` or `contents.titleChanged` of the same subscription and page is replaced by the newer one, which goes behind everything sent since; past 64 MiB of other frames the connection still closes with `automation.overloaded` | Only these two carry their page's whole state with the page as their scope (packages/core/docs/conventions.md, Automation rule 7) |
| `webContentsCreated(_, by: .automation(session))` | `.automation(session: String)`, the session's name; `.extension(id)` for a page an extension opened, when its new-window request reaches a Swift handler (T-EXT-12, 2026-11-23; `NewWindowRequest.initiatorExtensionID`); `.application` exists and is never emitted | A session from the external endpoint has no Swift object; the runtime reports only pages Swift or a session created (an extension's: T-EXT-12) |
| `bdk build … --watch` for pages and automation (cli.md) | `bdk build automation --watch` only | Pages' watch with a reloading debug runtime is T-CLI-06 |
| `@bdk/automation` service calls with `@bdk/internal`'s result shapes | The same commands and argument types, typed from the generated declarations; results as the broker sends them (a one-field result is an object, a paginated command returns a page) | Unwrapping would change the runtime library; javascript-api.md 5.3 says so |

Found and fixed while building it, recorded because the M5 status lists them: a host context whose last page
moved out or closed had its `Browser` deleted by Chromium (`UnloadController::TabStripEmpty` starts a window
close and a posted task finishes it), so the next page to enter it hit `CHECK(browser_)` in
`HostContext::Add`. The engine-scenario fix-up (2.18) fixed the same in `BdkBrowserWindow::ScopedTabOperation`,
which cancels that close, so this branch's copy in `HostContext` was dropped at the merge. Two M5 panel scenarios crashed in the test, not the
engine: they used a side panel's secure view after its request ended, when the engine has released it.

Found and not fixed: a page whose view has never been in a window, and the focused pane after its view leaves
the window, report visible (Chromium starts a `WebContents` visible). Creating pages hidden
(`initially_hidden`) fixed the first and broke pages in the engine harness's background windows, which then
never became visible, so it was reverted; T-ADP-11 (M9) has the host view report it instead.

The patch series is 25 (this branch's patch is the last). No other C ABI function changed. New tasks: T-ARC-04 and
T-ADP-11 (M9); the task list is 146. T-ADP-11 was T-ADP-10 on this branch, a number the fix-up had taken.

### 2.23 Product-owner decisions for M9: S3, S6, phone passkeys and Widevine (2026-11-25)

The product owner decided the M9 items that were waiting for them. Each is work for M9; nothing here was built
when it was recorded.

| Item | Decision | What M9 does |
|---|---|---|
| S3, the Added browser-owned surfaces ([sdk/README.md](sdk/README.md) section 6) | **Accepted, all of them.** | The eight `[ui.surfaces]` keys (`client_certificates`, `autofill_suggestions`, `exclusive_access`, `device_choosers`, `screen_capture`, `webauthn`, `external_protocols`, `page_health`) move from Added to Plan: the schema accepts them and drops `UNACCEPTED_SURFACES` (`packages/cli/src/core/config/schema-check.ts`), and the core, Swift and the engine honour each mode. Every surface is a routed request with a default presenter (T-UIR-13, T-UIR-15). |
| What T-UIR-10 refused or turned off ([sdk/ui-requests.md](sdk/ui-requests.md) 8.2) | **Routed, all of it, except web application installs.** | Device choosers (WebUSB, WebHID, Web Serial, and Web Bluetooth, turned on), screen capture, client certificates and external protocols become requests instead of refusals or BDK's fixed sheets; the File System Access pickers go through `file_pickers`; an extension's `chrome.desktopCapture.chooseDesktopMedia` goes through `screen_capture` (T-UIR-13). Payment Request, Secure Payment Confirmation, FedCM and the Digital Credentials API are turned on and routed through new surfaces (T-UIR-14). |
| Page-initiated web application installs | **Not decided** when this was recorded; decided later the same day (2.24). | Nothing under this section; see 2.24. The scope statement that installable web apps are not in v1 ([sdk/README.md](sdk/README.md) section 6) stands until then. |
| S6, extensions shipped in the bundle, and the other Added extension keys | **Accepted.** | `[extensions] developer_mode`, `[extensions.install] blocklist` and `[extensions.bundled]` move to Plan (T-EXT-14). |
| Contacting a phone's tunnel service for a passkey on a phone (T-UIR-12) | **A configuration switch, off by default.** | `[services.webauthn] phones` ([sdk/configuration.md](sdk/configuration.md) 3.8). With it off, the engine offers no phone and contacts no tunnel host, whatever `Info.plist` says; with it on and the Bluetooth usage string present, phones are offered and the tunnel host is contacted only after the person chooses a phone. Off by default because the tunnel service is a Google-run host for Android phones, which invariant 9 allows only when the application asks for it (architecture 16). |
| Widevine (decision D7) | **Not in v1.** | Nothing. DRM streaming does not play. Nothing may rule out adding it as an application opt-in later ([risks.md](risks.md) D7). |

Changed: [sdk/README.md](sdk/README.md) section 6 (S3, S6), [sdk/ui-requests.md](sdk/ui-requests.md) sections 4,
5.16, 5.17 and 8.2, [sdk/configuration.md](sdk/configuration.md) 3.7, 3.8 and 9, [risks.md](risks.md) D7, the M9
rows of [roadmap.md](roadmap.md), and tasks T-UIR-12, T-UIR-13, T-EXT-14, with T-UIR-14 and T-UIR-15 new. The task
list is 148.

Still waiting for the product owner: S1, S12 and S16 (roadmap section 6), and the other
Added items and scope statements in [sdk/README.md](sdk/README.md) section 6.

### 2.24 Installable web apps in M9 (2026-11-25)

The product owner decided the item 2.23 left open. Installable web apps (PWAs) move from "not in v1" to M9. Nothing
here was built when it was recorded, and none of it was checked against the Chromium source beyond what T-UIR-10
found; the design is [sdk/web-apps.md](sdk/web-apps.md).

| Topic | Decision | What M9 does |
|---|---|---|
| Whether BDK supports installable web apps | **Yes, as an opt-in.** `[web_apps] enabled`, `false` by default. | With `false` the runtime behaves as today. With `true` a page may offer itself for installation and the `webApps.*` commands work (T-PWA-02 to T-PWA-04). |
| Which profile an app runs in | **The application can choose.** An app is installed in one profile and always launches there. | `profile.webApps`: a page's own install goes to the page's profile; the application can install from a URL into another. Private profiles refuse installs (T-PWA-03, T-PWA-04). |
| The install prompt and the app's window | **The application's to customise, with BDK defaults.** | The install prompt is a new surface, `web_app_installs`, with a gesture-gated `install`. A launch is a new-window request of a new kind, `.webApp`. Each has a default presenter of BDK's own, because Chromium's are views (T-PWA-03, T-PWA-04). |
| Push messages | **A separate provider switch, off by default, with a warning.** `[services.push] provider`, `"none"` or `"google"`. | Chromium delivers push through Google's messaging service, which invariant 9 allows only when the application asks for it. `"google"` needs the developer's own project in Google's console; `bdk build` and `bdk doctor` say so with the warning `services.push_google`, and print the note `web_apps.push_unconfigured` when web apps are on with no push provider (T-PWA-02, T-PWA-05). |
| Registration with the operating system: a Dock icon per app, file and protocol handlers, run at login | **Not in M9 and not in v1**, as proposed with the design; to be confirmed by the product owner. | Nothing. Chromium does this on macOS with a signed app shim per installed app. An application shows its installed apps in its own UI instead ([sdk/web-apps.md](sdk/web-apps.md) section 8). |

Order: T-PWA-01 reads Chromium's web-app system at the pin before anything is built, because the largest unknown is
whether a page launched as an app works in a BDK host context without Chromium's app-type `Browser` (risk R32). The
configuration, the stub, the core and the Swift API (T-PWA-02, T-PWA-03) need no Chromium build.

This adds XL engine work to a milestone that was release hardening. If it threatens the v1 gates, the fallback in
R32 is to ship the install request and the registry and leave app windows for a later release.

Changed: [sdk/web-apps.md](sdk/web-apps.md) (new), [sdk/README.md](sdk/README.md) sections 1, 6 and 8,
[sdk/configuration.md](sdk/configuration.md) 3.7A (new), 3.8 and 9, [sdk/ui-requests.md](sdk/ui-requests.md)
sections 3, 4, 5.6, 5.21 (new) and 8.2, [sdk/services.md](sdk/services.md) 4 and 5.11A (new),
[sdk/swift-api.md](sdk/swift-api.md) 5, [sdk/cli.md](sdk/cli.md) 6, [sdk/production-browser.md](sdk/production-browser.md),
[risks.md](risks.md) R32 and section 4, the M9 rows of [roadmap.md](roadmap.md), and task T-UIR-13, with T-PWA-01
to T-PWA-05 new. The task list is 153.

### 2.25 Navigation policy hook in M9 (2026-11-25)

The product owner moved the navigation policy hook from "not in v1" to M9. Nothing was built when this was recorded.

| Topic | Before | Decision |
|---|---|---|
| Cancelling or redirecting a navigation before it starts | "Later", on the grounds that new-window requests cover the common case. An application could only call `stop()` after the start, route its own internal URLs natively, or ship an extension with blocking rules. | In v1, built in M9: allow, cancel or redirect ([sdk/swift-api.md](sdk/swift-api.md) 6.3). With no handler nothing changes. |

T-SWF-10 (stub, core, C ABI, Swift; no Chromium build) settles the API before T-SWF-11 (engine). The engine side is
expected to be a navigation throttle in the overlay with no patch; unverified.

Changed: [sdk/swift-api.md](sdk/swift-api.md) 6.3, [sdk/swift-api-reference.md](sdk/swift-api-reference.md) 6.3,
[sdk/production-browser.md](sdk/production-browser.md) 2.2, the M9 rows of [roadmap.md](roadmap.md), with T-SWF-10 and
T-SWF-11 new. The task list is 155.

### 2.26 M9 without a Chromium build (2026-11-25)

M9 was worked on one branch (`m9-final-touches`) in five parallel pieces, none of which built Chromium (AGENTS.md:
no build, sync or roll unless asked). What needs no build is built and tested on the stub; every engine half is
open, under the task it belongs to. The status is in [roadmap.md](roadmap.md), M9. Decisions taken while building:

| Topic | Decision |
|---|---|
| Navigation policy names (T-SWF-10) | Nested Swift types, `BDKWebContents.NavigationDecision`, `BDKWebContents.NavigationAction` and `BDKRuntime.NavigationPolicy`, by swift-api-reference.md's prefix rule. The handler's page is optional: nil when the application holds no object for the page yet. |
| What the policy is asked (T-SWF-10) | Main-frame navigations after their start event and before the request, and again at each server redirect (`redirectedFrom`). Not subframes, same-document navigations, back/forward cache restores or prerender activations (Chromium runs no throttle for an activation); a prerender's own navigation is asked with `isPrerender`. A redirect loads with the application's authority (a GET, no body, no referrer). Cancel is net error -3 with `cancelledByPolicy`. No timeout: a stop, a newer navigation or the page closing makes the decision stale. The 21st redirect decision in a row is `ERR_TOO_MANY_REDIRECTS`, matching Chromium's limit of 20. The handler is a property settable at any time, not a registration. |
| Page health (T-UIR-15) | Surface 33. `BlockedPopupRequest.allowOnce(_:confirmedBy:)` and `alwaysAllow(confirmedBy:)` take the confirming event and a listed popup. |
| Exclusive access (T-UIR-15) | A rejected or dropped notice is shown by the default presenter, because the exit instruction must be shown. |
| Autofill preview (T-UIR-15) | `preview` is an update of the open request, `{"preview": id}` or `{"preview": null}`; a completion naming a suggestion the list did not offer is `ui.widensRequest`. |
| `developer_mode = "user"` (T-EXT-14) | Behaves as `"on"` until the setting `extensions.developerMode` exists. |
| Bundled extensions (T-EXT-14) | Paths are relative to the project; the build copies them to the same path under `Contents/Resources`. New codes `extension.blocklisted` (also under `allow_all`) and `extension.notRemovable`. |
| T-UIR-14 surfaces | Proposed in [sdk/ui-requests.md](sdk/ui-requests.md) 5.22, waiting for review: the names `payments`, `identity`, `digital_credentials`; `digital_credentials` follows `[services.webauthn] phones`, because the desktop provider reads a credential only from a phone over the same transport; Payment Request calls that ask for shipping or payer details are refused in v1. Secure Payment Confirmation uses a keychain group compiled into Chromium, so it waits for the T-UIR-11 fix. |
| Web apps in a host context (T-PWA-01, R32) | Read, not run: `display-mode` comes from the page's `WebContentsDelegate::GetDisplayMode`, which BDK's `EngineContents` implements, so an app page can live in an ordinary host context with overlay code. R32's fallback is not taken; R32 is Low. |
| Web app install dialogs (T-PWA-01) | Every path converges on `OnWebAppInstallShowInstallDialog` with no embedder seam: one patch. Turning off OS integration (app shims) is a second patch. [sdk/web-apps.md](sdk/web-apps.md) sections 9 and 10. |
| Web Install API (T-PWA-01) | Not in v1. `navigator.install()` and `<install>` (features `WebAppInstallation`, `InstallElement`, on for macOS at the pin) reach views dialogs whether installs are on or off, the crash class T-UIR-10 removed; they are to be turned off at start (T-PWA-06). |
| Web app ABI (T-PWA-03) | Surface 37; contents event `WEB_APP` (12); domain `webApps` with `privateProfile` and `notInstallable`; the host's confirmed install and uninstall are ABI functions (`bdk_web_apps.h`), not commands; icons are references and an optional `data:` URL, never image buffers; `WebAppInfo.hasPendingUpdate` reports a held name or icon change, applying it is later (T-PWA-07). |
| Push credentials (T-PWA-05) | Read, not run: Chromium's GCM client authenticates with a device check-in that identifies as `DEVICE_CHROME_BROWSER` and reads no API key, so `InitializeAndOverrideAPIKey` does not configure push and the requirement may be a question of terms, not a console credential. Until T-PWA-05 settles it, `provider = "google"` needs the Google key (`config.keyMissing`). A provider that is not a string is `services.provider_unknown`, like the other providers. |
| Profile downgrade (T-PKG-05) | The existing `[runtime] downgrade` and `snapshot_on_upgrade` keys, applied by the core before the backend starts, to the whole data directory, with Chromium's own `Last Version` and `Snapshots/<version>/`. Restoring a snapshot deletes newer data, as Chromium does. Codes `runtime.downgradeRefused` and `runtime.profileMigrationFailed` are strings, not ABI constants. The core code uses `std::filesystem`; it is not Chromium code, and it has not been compiled under GN yet. |
| Third-party notices (T-REL-02) | Both GN presets set `generate_about_credits = true` (without it `chrome://credits` is a placeholder), which changes the source key: the next runtime build is a rebuild. The notices ship at the top of the runtime as `THIRD_PARTY_NOTICES.html`, outside the framework, so its signature is untouched. |
| Release documents | A new `docs/release/` directory: the procedure, the security-patch policy, the licence review, versioning, compatibility, experimental APIs and the cross-platform review. The security response targets in it (3 and 10 working days) are proposals for the product owner. |
| `bdk init` frameworks (T-CLI-02) | Vanilla, React and Preact only: Vue, Svelte, Solid and Lit assign HTML strings to `innerHTML`, which the engine's `trusted-types` policy blocks. `--search-engine` and `[storage]` are not offered while S1 and S2 are not accepted. The project references BDK at the CLI's exact version (`--bdk-package` for another). The generated application runs on the stub until `bdk build runtime` and `embed` exist (T-CLI-04). Page templates are composed from shared pieces. |
| `bdk patch` and `bdk doctor` (T-CLI-03, T-CLI-01) | `bdk patch new` records no base ref; `export` finds the base as the pin plus BDK's series. A runtime built with application patches records `application: {patches, patches_digest}` in its identity and manifest; without them the source key is unchanged. In `bdk doctor`, integrity and signature failures exit 1. `doctor` lists every diagnostic the configuration resolver reports. |

Fixed on the way: the packaged CLI ran its bundled tooling modules' own `main()`, and packaged `bdk build automation`
always failed to load Rolldown; the framework's exports now name the navigation policy functions.

Changed: the M9 status in [roadmap.md](roadmap.md); tasks T-PKG-05, T-REL-01, T-REL-02 and T-XPL-02, with T-PWA-06,
T-PWA-07, T-CLI-07 and T-REP-09 new. The task list is 159.

## 3. Tasks and workflows

| Task | Change |
|---|---|
| T-CHR-11 | New, M1. Chromium roll bot. |
| T-PRV-09 | New, M6. Provider selection plumbing and the application's key at start. |
| T-REP-07 | New, M1. Conventional Commits check. |
| T-ADP-08 | Renamed: host-context mapping experiment for off-screen pages. |
| T-CHR-10 | Output is now notes that feed the roll bot. |
| T-REP-04 | Checks the runtime digest against the pin record instead of an attestation; skips when a rebuild is needed. |
| T-REP-05 | Was the dedicated-builder workflow. Now local runtime publishing and the pre-release pass. |
| T-REP-06 | Fumadocs. |
| T-PKG-06 | Was the automated release workflow. Now the release procedure by hand. From 2026-11-16 it also packages the CLI executable. |
| T-CLI-05 | New, M2 (2026-11-16). CLI foundation and the packaged executable. |
| T-CLI-02 | From 2026-11-16: prompts, a flag for every prompt, page templates. M to L. |
| T-CLI-04, T-CFG-02 | From 2026-11-16: depend on T-CLI-05. |
| T-REP-01 | From 2026-11-16: also writes `.node-version`. |
| T-CLI-06, T-JS-05, T-SWF-09, T-SVC-11 | New, M5 (2026-11-20): left over from M4 (roadmap M4 status). |
| T-WUI-07, T-PRV-10 | New, M6 (2026-11-20): left over from M4. |
| T-UIR-09, T-UIR-10 | New, M4 (2026-11-22): WebAuthn (built, 2.19) and the other tab-modal views dialogs, found after M3. |
| T-UIR-11, T-UIR-12 | New, M9 (2026-11-22): Touch ID passkeys, phones and system passkeys, which need a signed, entitled application. |
| T-UIR-13 | New, M9 (2026-11-22): routing what T-UIR-10 refused or turned off, after S3 (2.20). |
| T-ARC-04 | New, M9 (2026-11-21): left over from M8; the reference application checked by hand and its extension scenarios on the engine through its own windows. |
| T-ADP-11 | New, M9 (2026-11-21): left over from M8; a never-shown page and a window's focused pane after its view leaves report visible. |
| T-EXT-12 | Widened (2026-11-21): a page an extension opens is also announced as `webContentsCreated(_, by: .extension(id))`. |
| T-UIR-12, T-UIR-13, T-EXT-14 | Decided (2026-11-25, 2.23): the phone tunnel is behind `[services.webauthn] phones`; everything T-UIR-10 refused is routed except web application installs; S6 is accepted. |
| T-UIR-14 | New, M9 (2026-11-25): Payment Request, Secure Payment Confirmation, FedCM and the Digital Credentials API turned on and routed (2.23). |
| T-UIR-15 | New, M9 (2026-11-25): the S3 `[ui.surfaces]` keys accepted, and the three S3 surfaces with no task (`autofill_suggestions`, `exclusive_access`, `page_health`) finished (2.23). |
| T-PWA-01 to T-PWA-05 | New, M9 (2026-11-25): installable web apps and push (2.24). |
| T-SWF-10, T-SWF-11 | New, M9 (2026-11-25): the navigation policy hook (2.25). |
| T-UIR-13 | Changed (2026-11-25, 2.24): its scenario that a page-initiated install stays off now holds for the default, `[web_apps] enabled = false`. |
| T-PWA-06 | New, M9 (2026-11-25, 2.26): turn off `navigator.install()` and `<install>` at start, found in T-PWA-01. |
| T-PWA-07 | New, after v1 (2026-11-25, 2.26): apply a held manifest update. |
| T-CLI-07 | New, M9 (2026-11-25, 2.26): what `bdk doctor` does not check yet. |
| T-REP-09 | New, M9 (2026-11-25, 2.26): publishing the documentation site. |
| T-REL-01, T-REL-02, T-PKG-05, T-XPL-02 | Widened (2026-11-25, 2.26): `SECURITY.md`; BDK's own licence; the engine half of the downgrade policy; the cross-platform findings recorded for M10. |

The task list goes from 103 to 106 tasks. The README said 100 before, which was already wrong; it now says 106. With T-CLI-05 on 2026-11-16 it is 107.

| Workflow | Status |
|---|---|
| `pr.yml` | From 2026-11-16: also packages the CLI executable and smoke-tests it |
| `integration.yml` | Changed, as in 2.7 |
| `chromium-roll.yml` | New |
| `docs.yml` | Builds the Fumadocs site. Not written yet: `pr.yml` builds the site and checks its links; publishing is T-REP-09 |
| `chromium-build.yml` | Removed |
| `chromium-rebase.yml` | Removed; replaced by the roll bot |
| `release.yml` | Removed for now; deferred |

## 4. New unverified items

| Item | Where it gets settled |
|---|---|
| `support_external_google_api_key` builds and works on macOS | T-PRV-09 |
| Chromium's translate endpoint is closed to third-party keys | T-PRV-08 |
| The version endpoints and the sparse checkout work on a hosted runner | T-CHR-11 |
| A Chromium-based runtime launches and renders on a hosted macOS runner with no real GPU. If not, integration tests move to the local pass. | T-REP-04 |
| Every build-time figure: about 90 minutes for a full build is a 2023 report | T-CHR-05 |
| ~~SwiftPM expects `Package.swift` at the repository root, and the package lives in `packages/swift`~~ Settled in M2 (T-SWF-01): the manifest is the root `Package.swift`; sources stay in `packages/swift`. T-PKG-04 still tests binary-target resolution from a clean machine. | T-PKG-04 |
| A page launched as an installed web app works in a BDK host context without Chromium's app-type `Browser` (2.24). Read, not run, in T-PWA-01: it can (2.26) | T-PWA-04 |
| Chromium's web-app install can be confirmed from a request, and can install from a URL into a chosen profile (2.24). Read, not run, in T-PWA-01: with one patch, and from a URL with no patch (2.26) | T-PWA-04 |
| What Chromium's push client needs from the developer's Google project, and that it is silent with no provider (2.24) | T-PWA-05 |

## 5. Where to look

| Document | Sections that changed |
|---|---|
| [architecture.md](architecture.md) | 1 (A9), 2, 4.1, 8.2, 11.1 (new), 13, 17 |
| [repository-and-tooling.md](repository-and-tooling.md) | 1, 2, 3, 4, 6.1 (new), 7.3 (new, 2.21), 8, 8.1 (new), 9, 10, 10.1 (new), 10.2 (new), 12 |
| [risks.md](risks.md) | D1, D8, R3, R4, R9, R15, R30 (new) |
| [roadmap.md](roadmap.md) | M0.7, M1, M2, M6, M9, v1 gates, section 5; M2 status (2026-11-19, 2.12) |
| [tasks.md](tasks.md) | The rows in section 3 above |
| [feasibility.md](feasibility.md) | Section 10, two sentences |
| [sdk/](sdk/README.md) | New. The index and all nine documents. |
| [README.md](README.md) | One row for `sdk/` |
| For 2.20 | [sdk/ui-requests.md](sdk/ui-requests.md) 4, 5.3, 5.14, 5.16, 5.17, 8.2; [tasks.md](tasks.md) T-UIR-09, T-UIR-10, T-UIR-13; [roadmap.md](roadmap.md) M4, M9; `chromium/bdk/README.md`; `chromium/bdk/engine/bdk_testing.h`; `apps/examples/minimal-browser/README.md` |
| For 2.24 | [sdk/web-apps.md](sdk/web-apps.md) (new); [sdk/README.md](sdk/README.md) 1, 6, 8; [sdk/configuration.md](sdk/configuration.md) 3.7A, 3.8, 9; [sdk/ui-requests.md](sdk/ui-requests.md) 3, 4, 5.6, 5.21, 8.2; [sdk/services.md](sdk/services.md) 4, 5.11A; [tasks.md](tasks.md) T-PWA-01 to T-PWA-05, T-UIR-13; [roadmap.md](roadmap.md) M9; [risks.md](risks.md) R32 |
| For 2.19 | [sdk/ui-requests.md](sdk/ui-requests.md) 4, 5.18, 8, 8.1; [sdk/README.md](sdk/README.md) S3; [sdk/production-browser.md](sdk/production-browser.md) 2.4; [sdk/configuration.md](sdk/configuration.md) 6, 9; [tasks.md](tasks.md) T-UIR-09 to T-UIR-12; [roadmap.md](roadmap.md) M4, M9; `packages/core/docs/conventions.md` (the router); `chromium/bdk/README.md` |
| For 2.10 | [sdk/cli.md](sdk/cli.md) section 2; [repository-and-tooling.md](repository-and-tooling.md) 1, 2, 3, 4, 10, 10.1, 10.2, 12; [tasks.md](tasks.md) T-CLI-05; [risks.md](risks.md) R31; [roadmap.md](roadmap.md) M2 |
