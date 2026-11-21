# Amendments to the BDK plan

Dated 2026-11-15; section 2.10 was added on 2026-11-16. This lists everything that changed in the planning documents after the 2026-11-14 version, following the product owner's review. If you read the 2026-11-14 documents, reading this is enough. The other documents already contain these changes; nothing here still needs to be applied.

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

### 2.14 Automation and DevTools built in M7 (2026-11-20)

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
| A slow subscriber's events coalesced (services.md 7) | A connection 64 MiB behind is closed with `automation.overloaded` | Simple and safe for v0; coalescing is T-AUT-07 |

New in the ABI: `bdk_session_run`, `bdk_task_cancel`, `bdk_task_subscribe`, `bdk_automation_endpoint_open` and
`_close`, `bdk_automation_hello_accept` and `_refuse` (the export patch lists them). New patch: "bdk: register the
automation host in the utility process" (Chromium has no embedder hook for utility services). The series is 15.
A saved page (`contents.save`) appears in the profile's downloads, because Chromium saves pages as downloads.

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

The task list goes from 103 to 106 tasks. The README said 100 before, which was already wrong; it now says 106. With T-CLI-05 on 2026-11-16 it is 107.

| Workflow | Status |
|---|---|
| `pr.yml` | From 2026-11-16: also packages the CLI executable and smoke-tests it |
| `integration.yml` | Changed, as in 2.7 |
| `chromium-roll.yml` | New |
| `docs.yml` | Builds the Fumadocs site |
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

## 5. Where to look

| Document | Sections that changed |
|---|---|
| [architecture.md](architecture.md) | 1 (A9), 2, 4.1, 8.2, 11.1 (new), 13, 17 |
| [repository-and-tooling.md](repository-and-tooling.md) | 1, 2, 3, 4, 6.1 (new), 8, 8.1 (new), 9, 10, 10.1 (new), 10.2 (new) |
| [risks.md](risks.md) | D1, D8, R3, R4, R9, R15, R30 (new) |
| [roadmap.md](roadmap.md) | M0.7, M1, M2, M6, M9, v1 gates, section 5; M2 status (2026-11-19, 2.12) |
| [tasks.md](tasks.md) | The rows in section 3 above |
| [feasibility.md](feasibility.md) | Section 10, two sentences |
| [sdk/](sdk/README.md) | New. The index and all nine documents. |
| [README.md](README.md) | One row for `sdk/` |
| For 2.10 | [sdk/cli.md](sdk/cli.md) section 2; [repository-and-tooling.md](repository-and-tooling.md) 1, 2, 3, 4, 10, 10.1, 10.2, 12; [tasks.md](tasks.md) T-CLI-05; [risks.md](risks.md) R31; [roadmap.md](roadmap.md) M2 |
