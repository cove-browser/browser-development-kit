# BDK risks, fallbacks and open decisions

Status: plan, 2026-11-14. Evidence is in [feasibility.md](feasibility.md). Task and milestone identifiers refer to [tasks.md](tasks.md) and [roadmap.md](roadmap.md).

## 1. Decisions needed from the product owner

These affect functionality, security, compatibility, licensing or cost. Everything else is left to the implementation agent.

Status after the product owner's review on 2026-11-16: D2, D3, D4, D6 and D8 are decided. D1 is provisional until the M0 probe. D5 is delegated. D7 is decided for codecs and deferred for Widevine. Nothing here blocks M0.

| # | Decision | Options | Recommendation | Needed by |
|---|---|---|---|---|
| D1 | What extensions and services observe for pages that are not on screen. BDK has no space, workspace or group type either way. | (a) An off-screen page is always a background tab of the window it was last attached to. (b) As (a) by default, plus one optional generic call that lets the application keep pages in a host context of its own, which extensions see as a separate window. | **Provisionally (b), product owner 2026-11-16.** The default with no call is (a), which is also what Chrome does: every tab that is not selected is a background tab of its window. The M0 probe (T-ADP-08) still records what extensions observe under both. | Confirm at end of M0 |
| D2 | The default reputation provider | (a) Signed static block lists built from open feeds, served as files by BDK. (b) A BDK-run lookup service fed by open and licensed feeds. (c) No BDK default; each application must configure a provider, and BDK ships only local protections. | **Decided 2026-11-16: (a).** It is the `"default"` value of `[services.reputation] provider`, behind the same provider contract as translation and geolocation (architecture 11), so an application can still switch to `web-risk`, `google` or `app`. It has no per-request service to run and no per-user data. It is weaker than Chrome and must be labelled "reduced protection". Feed licences are still unverified. | Decided |
| D3 | Accept the branded scheme as an alias | (a) Accept: users see `<scheme>://settings`; the page's own origin is `chrome://bdk-settings`. (b) Require a true `<scheme>://` origin, available in source builds only and at high patch cost. | **Decided 2026-11-16: (a).** T-WUI-01 still has to show it works end to end. | Decided |
| D4 | Chrome Web Store as an installation source | (a) On by default. (b) Available as a named source that each application enables after its own legal review. (c) Not supported. | **Decided 2026-11-16: (b).** It works in any Chromium build, but Google's store terms describe distribution to Chrome users and contain no clause for other browsers. New requirement: when the source is enabled, the store's install button reads "Add to <browser name>". The button is part of Google's page, so this is a runtime-injected relabel that can break when the page changes; if it does, the label falls back to Google's text and installation still works. | Decided |
| D5 | Who runs the mirror for security component data | (a) BDK hosts a mirror of Google-signed component packages. (b) Each application hosts its own. (c) The runtime contacts Google's update server directly for those components only. | **Delegated to the implementation agent, 2026-11-16**, after T-PRV-06. Constraint from the product owner: low recurring cost to BDK. That rules out (a) unless it is nearly free to run. If the agent picks (c) as a default, that still needs the product owner's explicit exception to the "no Google-directed requests by default" rule. | Before M6 |
| D6 | Intel Mac support | (a) Apple Silicon only. (b) Universal runtime. | **Decided 2026-11-16: (a).** Apple Silicon only for v1. | Decided |
| D7 | Proprietary codecs and Widevine | Codecs: (a) ship none; (b) license and ship Chromium's bundled decoders; (c) patch H.264, AAC and HEVC to decode through the operating system's own decoders, so the runtime carries no proprietary decoder. Widevine: ship, application opt-in, or none. | **Codecs: (c), product owner 2026-11-16**, subject to the spike in T-CHR-12 and to the legal review. One runtime, no codec variant. If the spike fails, the runtime ships open codecs only until this is reopened. **Widevine: deferred by the product owner.** Not in the plan; DRM streaming does not play until it is decided. Nothing in the design may rule out adding it as an application opt-in later. | Codecs: spike in M2. Widevine: no date |
| D8 | Release train | (a) Follow Extended Stable, eight weeks. (b) Follow stable, two weeks. | **Decided 2026-11-16: (b).** `main` tracks the newest stable through one pin and a roll bot. Risk R3's fallback still applies if rolls prove too costly. | Decided |

## 2. Ranked risks

Severity is the effect on the product if the risk materialises. Likelihood is a judgment from the evidence so far.

### Critical

| # | Risk | Likelihood | Evidence | Mitigation | Fallback |
|---|---|---|---|---|---|
| R1 | A BDK window that is not `BrowserView` hits crashes or missing behaviour in `//chrome`. 44 non-Views files cast to `BrowserView`. | Medium | Feasibility 1. Upstream's Webium work tolerates a null `BrowserView` in some paths; the rest are untested. | M0 runs Chromium's own browser tests against the BDK window and logs every null cast and unimplemented call. Fix by null guards, upstreaming where possible. | CEF-style hidden `BrowserView` for the affected window types. This limits one page per window and conflicts with split view, so it is a fallback for auxiliary windows only. |
| R2 | One active tab per window breaks service behaviour in layouts with three or more visible pages. Translate UI, find and dialog foregrounding ignore the extra panes. | High | Feasibility 10; `chrome_translate_client.cc:621-631`; `multi_contents_view.h:49`. | Ship one- and two-pane mapping first. Then patch the foreground checks to use a BDK "presented" predicate (T-ADP-06). | One internal window per visible pane. Extensions then see more windows; document it. |
| R3 | Rebase cost. Chromium ships a stable release every two weeks; the window and tab interfaces gain methods every milestone. | High | Feasibility 6; report on build tooling. | Keep patches to hooks. The roll bot applies the patch series to each new stable tag and opens a pull request that names any conflict (tooling document 6.1). Tracking stable instead of Extended Stable means more frequent, smaller rolls; decision D8 accepted that. Rehearse one rebase in M1 (T-CHR-10) and measure it. | Move to a dedicated fork repository if rebases exceed about two engineer-days each. |
| R4 | Security patch gap. A BDK-based browser is exposed between an upstream security release and the application shipping a rebuilt runtime. | High | Structural. | Publish a target: runtime security release within a stated number of days of each upstream stable security update. The roll bot raises an issue when the pin falls behind. The roll is built, tested and published by hand until a builder exists; automate it then. Notify downstream applications. | None that removes the gap. State it plainly in documentation. |
| R5 | The default reputation provider gives materially weaker protection than Chrome, and users or downstream vendors assume otherwise. | Certain unless handled | Feasibility 6. | Decision D2. Label the default "reduced protection" in documentation and default UI. Keep interstitials, certificate checks and download warnings fully working for whatever the provider reports. Offer Web Risk as an option with the application's key. | Applications that need parity license a commercial provider. |

### High

| # | Risk | Likelihood | Evidence | Mitigation | Fallback |
|---|---|---|---|---|---|
| R6 | `//chrome` start-up patches for an application-owned `NSApplication` are numerous and drift. | Medium | Feasibility 2. | Keep them in one patch with a single BDK hook each. Cover with a start-up test in CI. | A CEF-style external message pump, unverified for `//chrome`. |
| R7 | Chromium's dialog and bubble views cannot be parented to a native window that is not a Views widget, so default presenters cannot reuse them. | Medium | Unverified; Webium still uses a widget for its window. | Test in M0 (T-MAC-04). | Write platform-native default presenters. More work per platform, no change to the request model. |
| R8 | The alias scheme leaks or misbehaves: web pages reaching internal pages, wrong URLs in history or session data, developer confusion over the page's real origin. | Medium | Feasibility 4. | Negative tests in M0 and M4. Display mapping in one place. Document the origin clearly. | Restrict branded URLs to application-initiated navigations only. |
| R9 | Extension behaviour differs from Chrome in ways the manifest cannot predict: off-screen pages in query results, event order when a page moves between windows or host contexts, side-panel scope in split view. | High | Feasibility 3 and 10. | Decision D1. A compatibility suite driven by the MV3 probe plus real extensions. Publish the matrix and the known differences. | The optional host-context call in D1, so a product can pick the mapping its extensions tolerate best. |
| R10 | Chrome Web Store access is withdrawn or restricted for non-Chrome browsers. | Low to medium | Store terms; no known history of blocking, unverified. | Decision D4. Keep unpacked, CRX and self-hosted update sources first-class. | Applications run their own catalogue. |
| R11 | The component update mirror is not possible without Google's signing key for the update check. | Unknown | Feasibility 6. | T-PRV-06 before M6. | Bundle revocation and transparency data in each runtime release and accept staleness between releases; or decision D5 option (c). |
| R12 | Nested helper signing fails under Xcode or notarization: wrong team identifier, missing entitlements, library validation. | Medium | Feasibility 7. | BDK signing script with a test that embeds the runtime in an application with a different team and notarizes it (T-PKG-03). | Document a manual signing procedure. |
| R13 | Licensing: Widevine, patent pools for H.264, AAC and HEVC, trademark use, feed licences, redistribution of Google-signed component data. | Medium | Unverified; needs legal review. | Decision D7: system decoders for H.264, AAC and HEVC (T-CHR-12), no bundled proprietary decoder, Widevine deferred. Legal review before M9, including whether calling the system decoder needs a licence. Ship third-party notices with every runtime. | Open codecs only; no DRM. |
| R14 | A translation default based on Bergamot is slower, larger or lower in quality than users expect, and the engine integration is harder than estimated. | Medium | Feasibility 6. Models are 13 to 60 MB each; the renderer contract must be replaced. | Spike in M6 before committing (T-PRV-02). Download models on demand. | Apple Translation as the macOS default where the language pair is installed; cloud providers with the application's key. |

### Medium

| # | Risk | Likelihood | Evidence | Mitigation | Fallback |
|---|---|---|---|---|---|
| R15 | Build infrastructure cost and time. No measurement yet; the only figure is a 2023 report of about 90 minutes on similar hardware. Hosted CI cannot build Chromium. | High | Build tooling research. | Measure in M0 (T-CHR-05). No Chromium build runs in CI. The runtime is built on a developer's Mac and the full suite runs there before a release (tooling document 10.1 and 10.2). ccache. | A rented Apple Silicon builder, roughly $350 to $1,100 a month, when the `dev` branch and beta releases arrive. |
| R16 | Nx cache poisoning or false hits. The local cache is shared across worktrees; remote caches have a known poisoning weakness. | Medium | Feasibility 9; CVE-2025-36852. | Runtime resolution is never cached. Toolchain identity is an input. No remote cache; none in release jobs. | Disable caching for a target that proves hard to describe. |
| R17 | Concurrent agents corrupt the shared Chromium checkout or compiler cache. | Low with the design | Feasibility 9. | Kernel-released lock per checkout; separate experiment checkouts; immutable runtimes. | One checkout per agent, at disk cost. |
| R18 | Profile data is not compatible when an application rolls back to an older engine. | Medium | `downgrade_manager.cc:313-337`. | Snapshot before first run of a newer milestone. | Fresh profile on downgrade, with the application warned. |
| R19 | The C ABI changes under early SDKs and breaks binary compatibility. | Medium | Structural. | Version and hash check at start; size-prefixed structs; conformance suite against stub and real runtime. Swift and TypeScript stay experimental. | Version the ABI per runtime release until v1. |
| R20 | Windows and Linux assumptions leak into the shared core. | Medium | Structural. | Platform files by suffix; ABI headers compiled as C on Linux in CI; review checklist per ABI change. | Refactor at the Windows milestone. |
| R21 | The automation host's JavaScript runtime choice proves wrong: no Node APIs in a V8 utility process, or sandbox limits. | Medium | Open in the spec. | Spike in M7 (T-AUT-01) before building. | External process on the authenticated local channel. |
| R22 | Residual Google-directed requests remain after removal work. | Medium | Many endpoints are constants in source. | Network audit as a release gate (T-PRV-07). Use ungoogled-chromium's patch list as a checklist. | Block at the network layer by host list as a second line. |
| R23 | Off-store CRX installation weakens extension safety, because the store's review is absent. | Medium | `extension_management.cc:388-402`. | Keep Chromium's install prompt and permission warnings. Make sources configurable per application. | Applications restrict sources to their own catalogue. |
| R24 | Geolocation on macOS still calls Google's network provider in hybrid mode. | Unknown | Feasibility 6. | Force platform-only mode; cover with the network audit. | Patch out the network provider. |
| R25 | Accessibility, input methods and keyboard handling regress outside Chromium's own window code. | Medium | Feasibility 2. | Explicit test pass with VoiceOver and an input method in M2. Keep Chromium's event handling in the BDK application class. | Adopt Chromium's command-dispatching window behaviour for BDK-hosted windows. |
| R30 | Engine regressions are found late. CI never compiles `chromium/bdk` or the patch series, and runtimes built by hand carry no build attestation. | High | Product-owner decision; tooling document 10.1. | Keep Chromium-free logic in `packages/core`, where CI tests it. Require a local build, `pnpm test:release` and a published runtime before merging any change under `chromium/`. Review the digest recorded in `chromium/pin.json`. Run the full local pass before every release. | Add a build machine and a build workflow. |

### Low

| # | Risk | Mitigation |
|---|---|---|
| R26 | Runtime artifact size strains SwiftPM or release-asset limits. Chrome's universal framework is about 730 MB; a release asset limit is 2 GiB. | Apple Silicon only (D6); test the distribution path in M2 (T-PKG-04). Fallback: an OCI registry or object storage. |
| R27 | Taplo stops being maintained. | Tombi as a replacement. |
| R28 | The Grep MCP index does not include upstream Chromium. | Use the pinned checkout for upstream; Grep for forks. |
| R29 | `lockf` and APFS clones are macOS-specific. | `flock` and reflink copies on Linux; revisit at the Linux milestone. |
| R31 | The CLI ships as a Node single executable. Node marks that feature "Active development" and its file system for embedded assets "Early development", so a Node update could change either. | One pinned Node 26 release; plain named assets, not the asset file system; packaging behind one adapter; checked in M2 (T-CLI-05). Fallback: Bun's `bun build --compile`. |

## 3. Approaches that would compromise the brief's requirements

These were considered and are not used, or are used only as stated.

| Approach | Why it is rejected or limited |
|---|---|
| Host bare WebContents without Chromium's tab model | Loses about 177 tab-scoped services and history recording. CEF's version of this defaults permission prompts to deny and cancels downloads. |
| Hide a `BrowserView` and embed its contents, as CEF does | At most one page per window. Conflicts with split view. Kept only as a fallback for auxiliary windows. |
| Implement the window interface without `Browser` | Rebuilds most of `Browser` and follows upstream churn. |
| A second privileged origin for the branded scheme | Large patch across security checks and compiled-in metadata; two privileged origins. |
| Enable Google Safe Browsing with a BDK key | The API terms allow non-commercial use only, and keys may not be shared across legal entities. |
| Describe open-feed reputation as equivalent protection | Untrue. |
| Cache the runtime resolution in Nx | Verified to report a tampered or missing runtime as valid. |
| A BDK disk-usage or storage-management command | Excluded by the brief. |
| A BDK agent permission engine, CLI wrapper or MCP gateway | Excluded by the spec for v1. BDK provides scoped sessions and events instead. |

## 4. Assumptions that remain unverified

Each is tied to the task that settles it.

| Assumption | Task |
|---|---|
| A BDK `BrowserWindow` runs `//chrome` services without `BrowserView` | T-ADP-01, T-ADP-02, T-ADP-04 |
| A Swift-owned `NSApplication` can host `//chrome` | T-MAC-01, T-MAC-02 |
| The alias scheme works end to end from a startup switch | T-WUI-01 |
| The MV3 probe behaves correctly under native hosting | T-EXT-01 |
| Chromium's dialog views can serve as default presenters on a native window | T-MAC-04 |
| The number of call sites gating on the active tab is small | T-ADP-06 |
| A component mirror can answer update checks | T-PRV-06 |
| Xcode and notarization accept the re-signed runtime under another team | T-PKG-03 |
| Chromium builds in acceptable time on available hardware, and ccache helps across checkouts | T-CHR-05, T-CHR-06 |
| Siso and GN state tolerate a cloned source tree | T-CHR-07 |
| Bergamot integrates as a renderer-side translation engine with acceptable speed | T-PRV-02 |
| H.264, AAC and HEVC can be decoded through the system's decoders with Chromium's bundled proprietary decoders left out, including the build-flag combination that needs | T-CHR-12 |
