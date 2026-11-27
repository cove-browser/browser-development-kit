# Security patches

Task T-REL-01. Section 1 is the policy applications can rely on; section 2 is the process the maintainers
follow; section 3 is the timed rehearsal the roadmap asks for, which has **not been run** (it needs a Chromium
build).

## 1. Policy

- BDK follows the newest Chromium stable on one branch, `main` (decision D8). There are no maintenance
  branches: a security fix reaches applications as a new BDK release on the current Chromium milestone.
- A Chromium stable point release that fixes security bugs becomes a BDK **patch** release
  (`vX.Y.Z+1`, [versioning.md](versioning.md)). A new milestone is a minor release, and carries that
  milestone's security fixes as well.
- **BDK never updates itself** (architecture invariant 10). A release fixes nothing for users until each
  application ships an update with the new BDK version. Applications should be able to ship a browser update
  within days of a BDK security release.
- No fixed response target (product owner, 2026-11-27): a security release ships as soon as it is ready, a
  release Google rates Critical or reports as exploited in the wild first. The roll bot's daily pull request is
  the alert, and the issue it opens when the pin is 14 days behind stable (risk R4) is the outer bound.
- Security data that Chrome refreshes through its component updater (CRLSet, the Certificate Transparency log
  list, the root store) does not refresh in BDK between releases: the component updater is off (decision D5).
  The CT log list and the root store are compiled into each runtime; there is no CRLSet revocation. Each release
  carries the data of its Chromium version.
- The release notes of a security release say which upstream release it carries, link its Chrome Releases post,
  and list any BDK-owned fix with its advisory.

### Reporting a vulnerability in BDK

[SECURITY.md](../../SECURITY.md) (2026-11-27) asks for reports through GitHub's private vulnerability reporting
and says fixes are published with a GitHub security advisory. Bugs in Chromium itself go to Chromium's tracker,
not here. Turning private vulnerability reporting on is a repository setting (T-REL-01): until it is on, the
"Report a vulnerability" button that SECURITY.md names is absent.

## 2. Process

Each step names who does it and what it produces. Times in brackets are the measured or estimated cost; the
rehearsal replaces the estimates.

| # | Step | How | Cost |
|---|---|---|---|
| 1 | **Detect.** The roll bot finds a newer stable for macOS | `chromium-roll.yml` daily; `fix(chromium): roll to <v>` pull request for a point release, with the Chrome Releases link and the days behind | automatic, under 24 h |
| 2 | **Assess.** Read the release's security fixes and decide the target (section 1) | Chrome Releases post; the bugs' severities | minutes |
| 3 | **Patch series.** The bot already applied the series at the new tag | the pull request is a draft labelled `needs-rebase` if a patch fails; rebase it with `chromium:patch-apply` and `chromium:patch-export` in an experiment checkout | 0 (applies) to hours |
| 4 | **Build.** On the roll branch | `pnpm chromium:sync`, `pnpm chromium:build` | within a milestone about 83 min on an M2 Max (12,868 compiles, measured for `.37` to `.65`); a new milestone about 3 h |
| 5 | **Package** | `pnpm chromium:package`, `--reproducible-check` | minutes |
| 6 | **Test** | `pnpm test:release`, `pnpm exec nx run core:test-engine` | estimated 1 to 2 h |
| 7 | **Release** | [procedure.md](procedure.md) sections 2.4 to 2.7: sign, notarize, publish, record the pin and checksum, merge the roll pull request, tag | estimated 1 h, plus notarization queue time |
| 8 | **Announce** | GitHub release notes; tell the applications known to use BDK | minutes |

Rules:

- A security roll is not combined with unrelated changes. Feature work waits for the next release.
- A patch that no longer applies is fixed in the smallest way that keeps its intent; if that is not possible
  within the target, the feature behind it is turned off for the release and the gap is written in the notes
  with a task ID. A security fix is never held back for a BDK feature.
- If the release fails a test that the previous release passed, the failure is triaged: an upstream behaviour
  change that BDK must follow is fixed; a flaky scenario is recorded and the release goes ahead only if the
  same scenario fails on the previous runtime too.
- A BDK-owned vulnerability (in `packages/core`, `chromium/bdk`, the patches, the CLI or the Swift SDK) is
  fixed on a private fork of the repository, released as a patch release, and disclosed in an advisory when the
  release is out.

## 3. Rehearsal

The roadmap's M9 acceptance is "a measured rehearsal for shipping a runtime after an upstream security
release". **Not done.** It needs a full Chromium build and a publish, which this change does not run. When it is
run, pick the newest stable point release after the pin, start a stopwatch at step 1, and fill in this table
in a copy of this section committed with the result.

| # | Step | Started | Finished | Duration | Notes (conflicting patches, failures, waits) |
|---|---|---|---|---|---|
| 1 | Roll pull request exists for the point release | | | | |
| 2 | Security fixes read; target chosen | | | | |
| 3 | Patch series applies (or rebased) | | | | |
| 4 | `chromium:sync` | | | | |
| 4 | `chromium:build` (record compile count and ccache hits from `out/release/bdk-build-timings.jsonl`) | | | | |
| 5 | `chromium:package` and `--reproducible-check` | | | | |
| 6 | `pnpm test:release` | | | | |
| 6 | `core:test-engine` | | | | |
| 7 | `bdk` executable signed and notarized | | | | |
| 7 | Example signed under the second team, notarized, stapled, launched from quarantine | | | | |
| 7 | `chromium:publish`; checksum committed; `chromium:release-check` passes | | | | |
| 7 | Pull request merged; tag pushed; release created | | | | |
| | **Total, upstream release to published BDK release** | | | | |

Checklist before calling it done:

- [ ] The total is recorded, with what made it longer than it had to be.
- [ ] Each step that took longer than its estimate has a note saying why, and a task if it can be shortened
      (for example, pre-building the next milestone's beta to fill ccache, tooling/README.md).
- [ ] The published runtime resolves on a second machine (`pnpm exec nx run chromium:runtime` with an empty
      `BDK_HOME`), and `chromium:release-check` passes on the tagged commit.
- [ ] Private vulnerability reporting is on (`SECURITY.md` exists since 2026-11-27).

What would close T-REL-01: the table above filled from a real run, committed.
