# BDK repository, tooling and build storage

Status: plan, 2026-11-14. Evidence for every "verified" claim is in [feasibility.md](feasibility.md) (experiments E8 and E9, and the build-tooling research). Fixtures that produced the results are in [experiments/](experiments/).

## 1. Decisions in this document

| Topic | Decision | Basis |
|---|---|---|
| Root orchestrator | Nx 24 with pnpm workspaces; explicit `project.json` targets using `nx:run-commands`; no second task runner | Decided in the brief; verified workable for C++, Swift and TypeScript (E8) |
| Native dependency edges | Declared by hand with `implicitDependencies` | Verified: Nx infers no Swift→C++ or TS→C++ edges, and an undeclared edge builds in the wrong order and fails (E8, case h) |
| Chromium source | External checkout under `BDK_HOME`, never inside a BDK worktree | Brief; verified isolation model with dummy sources (E9) |
| Chromium changes | Overlay directory plus an ordered patch series in this monorepo; no separate fork repository yet | Section 6; triggers for revisiting are listed there |
| Runtime artifacts | Content-addressed, immutable store outside the worktree; Nx caches only a small pointer file | Verified (E9); see section 5 for why the runtime itself is not an Nx output |
| Runtime resolution task | Never cached by Nx; it re-verifies the store on every run | Verified: a cached resolve task reports a tampered or evicted runtime as valid (E9, cases 11–12) |
| Locking | OS advisory lock held by the build process (`lockf` on macOS, `flock` on Linux) | Verified on macOS (E9, case 7) |
| Compiler cache | ccache through GN `cc_wrapper`, one shared directory | Research plus a toy cross-path test (E9, case 16); not yet verified on a Chromium build |
| Remote Nx cache | None at first | Section 5.4 |
| Chromium build executor | GN plus `autoninja`, which runs Siso by default at M158; plain Ninja remains a fallback | Research; this updates the "GN/Ninja" wording in the spec and brief |
| Branching and Chromium tracking | One branch, `main`, pinned to the newest Chromium stable. A scheduled roll bot opens the bump pull request. No maintenance branches yet. A `dev` branch that produces beta releases comes later. | Product owner; section 6.1 |
| Versioning and releases | SemVer, one version for the whole of BDK, with Conventional Commits from the start. Releases are cut by hand for now; release-please and the automated flow are deferred. | Product owner; section 8.1 |
| Chromium builds in CI | None. The runtime is built on a developer's Mac and published by hand. CI tests everything it can against the stub runtime and the last published runtime. The full suite runs locally before a release. | Product owner; sections 10.1 and 10.2 |
| Documentation site | Fumadocs | Product owner |
| CLI | TypeScript, with a platform-neutral core and one module per host platform. Shipped as one executable per platform, built with Node's own `node --build-sea` and no wrapper package. Bun is the fallback if T-CLI-05 fails. | Product owner, 2026-11-16; [sdk/cli.md](sdk/cli.md) section 2 |
| Node version | One Node 26 release, 26.9 or later, pinned in `.node-version` (read by fnm) and in `engines`. It builds the repository and is the runtime inside `bdk`. | Product owner, 2026-11-16 |

Where this document departs from the brief or the spec, section 11 lists the difference.

## 2. Repository layout

The layout follows the brief. The additions are marked.

```text
bdk/
├── apps/
│   ├── docs/                       # Documentation website (Fumadocs)
│   ├── examples/
│   │   ├── minimal-browser/        # Small runnable Swift reference app
│   │   └── arc-style-browser/      # Cove-like compatibility app (reference test, not a product)
│   └── templates/
│       ├── swift-browser/          # Starter application used by `bdk init`
│       └── pages/                  # Internal-page starters per framework, used by `bdk init`
├── packages/
│   ├── core/                       # Public C ABI headers, interface definitions, stub runtime
│   ├── swift/                      # macOS SDK sources (the SwiftPM manifest is the root Package.swift)
│   ├── js/
│   │   ├── automation/             # Trusted automation SDK
│   │   ├── internal/               # Privileged internal-page SDK
│   │   ├── protocol/               # Shared typed events and transport contracts
│   │   └── codegen/                # Binding and type generators
│   └── cli/                        # `bdk` developer CLI (init, build, patch, doctor); TypeScript
├── chromium/
│   ├── pin.json                    # Chromium tag, commit, BDK ABI version      (added)
│   ├── bdk/                        # //bdk GN module: Chromium-linked C++       (added name)
│   ├── patches/                    # Ordered patch series against upstream files
│   ├── gn/                         # GN argument presets (dev, release)
│   └── gclient/                    # .gclient template for managed checkouts    (added)
├── tooling/                        # TypeScript and shell helpers called by Nx targets
├── tests/                          # Cross-component fixtures: extensions, pages, e2e
├── docs/                           # Architecture, ADRs, planning (this directory)
├── .github/workflows/
├── nx.json
├── pnpm-workspace.yaml
├── package.json
├── .node-version                   # the pinned Node 26 release, read by fnm     (added)
├── lefthook.yml
└── .nxignore                       # excludes docs/planning/experiments          (added)
```

Two clarifications to the brief's layout:

- **`packages/core` and `chromium/bdk` are separate on purpose.** `packages/core` holds everything that must compile without Chromium: the C ABI headers, the interface definitions that drive code generation, the ABI version and hash, and a small stub runtime that implements the C ABI in memory. `chromium/bdk` holds the implementation that links against `//chrome` and can only be built by GN inside a Chromium checkout. This split is what lets Swift, TypeScript and docs work proceed, and lets hosted CI run, with no Chromium build.
- **`docs/planning/experiments/` contains nested `project.json` and `nx.json` files.** The root `.nxignore` must exclude that directory, or the root workspace will pick up fixture projects named `core` and `swift`.

## 3. Nx project graph

All edges below are declared with `implicitDependencies`. Nx does not discover them.

| Project | Path | Build tool | Depends on | Main targets |
|---|---|---|---|---|
| `core` | `packages/core` | CMake + Ninja (clang) | none | `build`, `test`, `lint`, `format` |
| `chromium` | `chromium` | depot_tools, GN, autoninja | `core` | `runtime`, `sync`, `patch-apply`, `patch-export`, `build`, `package`, `lint` |
| `swift` | `packages/swift` | SwiftPM | `core`, `chromium` | `build`, `test`, `test-integration`, `lint`, `format` |
| `js-protocol` | `packages/js/protocol` | tsc | `core` | `codegen`, `build`, `test`, `lint` |
| `js-codegen` | `packages/js/codegen` | tsc | `js-protocol` | `build`, `test`, `lint` |
| `js-internal` | `packages/js/internal` | tsc, Vite for samples | `js-protocol`, `js-codegen` | `build`, `test`, `lint` |
| `js-automation` | `packages/js/automation` | tsc, Rolldown | `js-protocol`, `js-codegen` | `build`, `test`, `lint` |
| `cli` | `packages/cli` | tsc, Rolldown; `node --build-sea` for one executable per platform | `core`, `js-codegen` | `build`, `test`, `lint`, `package` |
| `minimal-browser` | `apps/examples/minimal-browser` | xcodebuild | `swift`, `chromium`, `cli` | `build`, `dev`, `test-e2e` |
| `arc-style-browser` | `apps/examples/arc-style-browser` | xcodebuild | `swift`, `chromium`, `cli`, `js-internal` | `build`, `dev`, `test-e2e` |
| `template-swift-browser` | `apps/templates/swift-browser` | xcodebuild | `swift`, `cli` | `build` |
| `docs-site` | `apps/docs` | Fumadocs on Next.js, MDX content, static export | none | `build`, `dev`, `lint` |
| `integration-tests` | `tests` | Swift test runner plus fixtures | `swift`, `chromium`, `js-automation` | `test-extensions`, `test-e2e` |
| `tooling` | `tooling` | tsc, shell | none | `build`, `test`, `lint` |

Notes:

- `nx affected` works at project granularity. A C++-only change marks `swift` and `js-protocol` as affected (verified), but task-level inputs then keep the TypeScript build cached when the interface definition did not change (verified, E8 cases d and f). Do not rely on `affected` alone for speed; get the inputs right.
- The CLI is TypeScript, compiled to a self-contained executable per platform (product owner, 2026-11-16, so the tool can serve Windows and Linux later). The executable carries its own runtime, which keeps the spec's "a developer may use no JavaScript at all" promise. Xcode-specific work sits in a platform module. It is packaged with Node's `node --build-sea` ([sdk/cli.md](sdk/cli.md) 2.3). The contract that must outlive any tooling choice is the `bdk.toml` JSON Schema and its fixture suite in `tests/`.
- A small internal Nx plugin is deferred, as the brief suggests. Revisit it when three or more projects repeat the same target block.

## 4. Root scripts

| Script | Backed by | Behaviour |
|---|---|---|
| `pnpm install` | pnpm | Installs JS dependencies. `pnpm-workspace.yaml` must list `nx` under `allowBuilds`, or pnpm 11 skips Nx's install script (observed in E8). |
| `pnpm run setup` | `nx run tooling:setup` | Checks toolchain versions, including Node against `.node-version`, installs Lefthook hooks, resolves the pinned runtime. Does not fetch Chromium. `run` is required: `pnpm setup` is a pnpm built-in and never reaches the script. |
| `pnpm format` | `nx run-many -t format` | swift-format, clang-format, Biome, TOML formatter, shfmt. Writes changes. |
| `pnpm lint` | `nx run-many -t lint` | SwiftLint, clang-tidy where configured, Biome, tsc `--noEmit`, schema checks, actionlint, ShellCheck. No writes. |
| `pnpm test` | `nx run-many -t test` | Unit tests only. Uses the stub runtime, so no Chromium is needed. |
| `pnpm build` | `nx run-many -t build` | Builds packages and apps against the prebuilt runtime. Never compiles Chromium. |
| `pnpm cli:package` | `nx run cli:package` | Bundles the CLI with Rolldown and runs `node --build-sea` for the host platform. The result runs with no Node installed. |
| `pnpm chromium:sync` | `nx run chromium:sync` | Managed source-build mode: `gclient sync` to the pin, under the checkout lock. |
| `pnpm chromium:build` | `nx run chromium:build` | Applies patches, `gn gen`, `autoninja`, under the checkout lock. |
| `pnpm chromium:package` | `nx run chromium:package` | Assembles the runtime, writes the manifest, moves it into the runtime store. |
| `pnpm chromium:publish` | `nx run chromium:publish` | Uploads the packaged runtime as a release asset and records its source key, URL, ABI hash and manifest digest in `chromium/pin.json`. Run by a person, from a clean tree. |
| `pnpm test:release` | `nx run-many -t test test-integration test-extensions test-e2e` | The pre-release pass, run locally against the source-built runtime (section 10.2). |
| `pnpm dev:example` | `nx run minimal-browser:dev` | Builds and launches the minimal example. |
| `pnpm docs:build` | `nx run docs-site:build` | Builds the documentation site. |
| `pnpm exec nx affected -t lint test build` | Nx | The PR check. |

`pnpm build` switches to a source-built runtime only when `BDK_RUNTIME_MODE=source` is set or `chromium/pin.json` has no published runtime and the developer passes `--allow-source-build`. It fails with an explanatory message otherwise (verified behaviour of the prototype resolver, E9 cases 1 and 6).

## 5. Caching

### 5.1 What Nx caches

| Target kind | Cached | Inputs beyond the project's own files |
|---|---|---|
| TypeScript `build`, `test`, `codegen` | Yes | `packages/core/interface/**`, generator sources, `externalDependencies` for the compiler |
| `core:build`, `core:test` | Yes | `nativeToolchain` runtime input |
| `swift:build`, `swift:test` | Yes | `nativeToolchain`; output files of dependency tasks; `packages/core/include/**`; the runtime pointer file |
| App `build` | Yes | As for Swift, plus `bdk.toml`, `Pages/**` |
| `chromium:runtime` | **No** | Not applicable; always runs |
| `chromium:sync`, `build`, `package` | **No** | Not applicable; GN and Siso own incrementality |
| `lint`, `format` | Lint yes, format no | Tool configuration files |

The `nativeToolchain` named input is `{ "runtime": "sh tooling/toolchain-id.sh" }`. The script prints the clang, Swift, SDK and architecture identity. Verified: changing only that identity invalidates native targets and leaves TypeScript targets cached (E8 case g).

### 5.2 Why the runtime is not an Nx output

- A packaged macOS runtime is large. Chrome's framework directory is about 730 MB per version and Arc's is 592 MB (measured on installed bundles).
- Nx 24 keeps its local cache in `~/.nx/<workspace-id>/cache` and shares it across every worktree of the repository (verified, E9). Entries are retained for about a week according to the Nx documentation. A runtime must outlive that.
- The runtime's identity includes state Nx cannot see by hashing worktree files: the external Chromium checkout, the builder toolchain, and whether the artifact in the store is intact.

So the runtime lives in a store under `BDK_HOME`, and `chromium:runtime` writes a pointer file, `.bdk/runtime.json`, containing the runtime key and the SHA-256 of its manifest. Dependent tasks take that file as an input through `dependentTasksOutputFiles`. When the key is unchanged, dependents stay cached even though the resolve task ran again (verified, E9 case 5).

This satisfies the brief's request to cache "suitable packaged runtime artifacts": the artifact is reused across worktrees and machines, but through the runtime store and release storage, with Nx holding the proof of which artifact was used.

### 5.3 Runtime identity

Two identities are kept apart. The prototype in E9 used a single key that included the local toolchain. That is right for source builds and wrong for consumers, who cannot be expected to have the builder's Xcode.

| Identity | Contents | Used for |
|---|---|---|
| **Source identity** | Chromium tag and commit; digest of `DEPS`-pinned dependency revisions; SHA-256 of every file in `chromium/patches` and `chromium/bdk`; GN args; target OS and architecture; BDK ABI version | Looking up a prebuilt runtime; naming the store directory |
| **Build identity** | Source identity plus builder Xcode, SDK and clang versions, builder OS, depot_tools revision, build timestamp, CI run reference | Recorded in the manifest as provenance; hashed into the manifest digest |

Dependent Nx tasks hash the manifest digest, so two builds of the same source identity with different toolchains do not share downstream cache entries.

### 5.4 Remote cache

Start with none. The reasons:

- Nx's S3, GCS, Azure and shared-filesystem cache packages were deprecated on 2026-05-21 because of CVE-2025-36852, where anyone able to write to the bucket from a PR can poison entries that the main branch later trusts.
- The expensive artifact, the runtime, already has its own store and release channel.

If remote caching becomes worthwhile, use Nx Cloud, measure first, and keep it off for the release workflow. Release jobs must build from clean inputs and must not accept remote cache hits.

## 6. Chromium source: overlay and patches versus a dedicated fork

Recommendation: keep BDK's Chromium changes in this monorepo as an overlay directory (`chromium/bdk`, mounted at `src/bdk`) plus an ordered patch series (`chromium/patches`), following Electron's layout. New code goes in the overlay. Patches touch upstream files only to add hooks.

| | Overlay + patches in the monorepo | Dedicated fork repository |
|---|---|---|
| Agent worktrees | Small text files; most agents never need a Chromium checkout | Any engine change needs a Chromium worktree |
| Rebase cost | Only failing patches are fixed; cost grows with patch count and with hot files touched | Whole-branch rebase; `git rerere` helps |
| Conflict visibility | Patch application fails loudly in CI, per file | Seen only while rebasing |
| Bisecting upstream regressions | Awkward: each step re-applies patches | Natural with `git bisect` |
| Reproducibility | Chromium commit plus patch hash is the runtime key | Good if tagged; needs a mirror of a very large repository |
| Precedent | Electron, Brave, CEF, ungoogled-chromium, BrowserOS | Edge, Vivaldi, Opera (private) |

Move to a dedicated fork if any of these happens. These thresholds are judgment, not measurements:

1. A milestone rebase regularly takes more than about two engineer-days because of patch conflicts.
2. BDK must maintain two diverging patch sets for more than one active milestone at a time.
3. Security fixes have to be cherry-picked from upstream between milestones often enough that patch files become the bottleneck.
4. Patches start to depend on each other through mechanical refactors that cannot be expressed as independent hunks.

Patch workflow, modelled on Electron's:

- `chromium:patch-apply` runs `git am --3way` for each patch in the order given by `chromium/patches/series`, inside the external checkout.
- `chromium:patch-export` regenerates patch files from commits on top of the pinned commit.
- Every patch has a commit message stating why the hook is needed and which BDK component uses it.
- CI has a job that applies the series to the pinned commit with no build. It costs seconds and catches broken series early.

### 6.1 Branching and the Chromium roll

Chrome moved to a two-week stable cycle with Chrome 153 on 2026-09-08. Extended Stable keeps an eight-week cycle and is a public branch stream (M156 at the time of writing).

For now there is one branch. `main` always targets the newest Chromium stable release through `chromium/pin.json`, and every change merges to `main`. There are no release or maintenance branches and no second patch set. Decision D8 in [risks.md](risks.md) confirmed this on 2026-11-16: releases stay on the newest stable. The measured cost of a roll feeds risk R3.

A roll bot keeps the pin from going stale. It is a scheduled GitHub Actions workflow, `chromium-roll.yml`, backed by a script in `tooling/`. It runs on a free hosted Linux runner.

| Step | Detail |
|---|---|
| 1. Detect | Daily, read the current stable version for macOS from Chromium Dash or the VersionHistory API. Stop if it equals the pin. |
| 2. Resolve | Resolve the tag to its commit with `git ls-remote`. |
| 3. Bump | On a branch `chromium-roll/<version>`, write the new tag and commit to `chromium/pin.json`. |
| 4. Check patches | Make a blobless, sparse checkout of the new tag limited to the files the series touches, and apply the series with `git am --3way`. No build. |
| 5. Open or update one pull request | Title `feat(chromium): roll to <version>` for a new milestone and `fix(chromium): roll to <version>` for a point release, so the SemVer bump is clear from the title and release tooling can use it later. The body lists the upstream release notes link, days behind, and each patch that failed with the conflicting file. A failing series opens the pull request as a draft with a `needs-rebase` label. |
| 6. Build, by hand | CI does not build Chromium. A person checks out the roll branch, builds and packages locally, runs `pnpm test:release`, publishes the runtime with `pnpm chromium:publish` and pushes the updated pin record to the roll branch. The integration workflow then runs against that runtime, and the person merges. |

If a newer stable appears while a roll is open, the bot updates the same pull request. If the pin is more than a set number of days behind stable, it also opens an issue, because that is a security exposure (risk R4).

Why a script and not Renovate for this: Renovate's custom datasource can notice a new version and rewrite one string, but a roll also needs the commit resolved and the patch series tried, which on hosted Renovate cannot run. Renovate, or Dependabot, is still a reasonable choice for npm packages, pinned action SHAs and SwiftPM dependencies. That is not decided here.

The endpoints in step 1 and the sparse checkout in step 4 are unverified on a hosted runner. The feasibility work made an equivalent 1.2 GB sparse checkout from the GitHub mirror in under a minute.

## 7. Storage outside the worktrees

Everything large lives under one configurable root, `BDK_HOME`. The default is `~/.bdk` on macOS and Linux and `%LOCALAPPDATA%\bdk` on Windows. The path must contain no spaces, because Chromium's build does not tolerate them reliably.

| Path under `BDK_HOME` | Contents | Sharing | Protection |
|---|---|---|---|
| `runtimes/<source-key>/` | Packaged runtime plus `manifest.json` | Shared, immutable | Written to a temporary directory, verified, renamed into place, then made read-only |
| `git-cache/` | depot_tools bare mirrors (`GIT_CACHE_PATH`) | Shared, append-mostly | depot_tools' own per-mirror lock |
| `chromium/managed/` | The one managed checkout: `.gclient`, `src/`, `src/out/<config>` | Exclusive | Advisory lock for the whole sync, patch and build sequence |
| `chromium/exp/<id>/` | Experiment checkouts | Exclusive to one agent | Own lock file; created by APFS clone or Chromium's `tools/git/worktree.py` |
| `chromium/locks/` | Lock files and owner records | Shared | Kernel-released on process exit |
| `ccache/` | Compiler cache | Shared | ccache's size cap and eviction |

No BDK command reports disk usage. Retention uses existing mechanisms only: ccache's `max_size`, Nx's cache expiry, `git worktree prune`, and deleting an experiment directory when its owner is done. `bdk doctor` checks toolchain, runtime compatibility and signatures. It does not inspect disk space.

### 7.1 The three modes

**Default prebuilt mode.** A worktree runs `chromium:runtime`. The resolver computes the source identity, finds `runtimes/<key>`, verifies the manifest and file digests, and writes `.bdk/runtime.json` and a `.bdk/runtime` symlink. Verified with two worktrees: both resolved to the same inode, and a write into the runtime from a worktree was refused (E9 cases 3 and 4). If no runtime exists for the key, the task fails and says why. That is the correct outcome when an agent has edited a patch without building.

**Managed source-build mode.** `chromium:sync` and `chromium:build` run under `lockf -t 0 $BDK_HOME/chromium/locks/managed.lock`. A second worktree that tries to build is refused at once, and the owner record names the worktree and process holding the lock (verified, E9 case 7). The lock is released by the kernel when the process exits, so a crashed build leaves no stale lock. Output directories are per configuration, so switching between a development and a release configuration does not clobber either.

**Isolated experiment mode.** An agent with a conflicting revision or patch set gets `chromium/exp/<id>/` with its own lock and its own output directory. Verified with dummy sources: an experiment build ran concurrently with a managed build and the outputs were segregated by checkout and by key (E9 case 8). Two ways to create the checkout:

- APFS clone of the managed source tree: `cp -cR`. Measured on a 406 MB, 4,001-file tree: 0.59 s and about 2 MiB of extra space, against 1.31 s and 406 MiB for a full copy. Modification times were preserved.
- Chromium's in-tree `tools/git/worktree.py`, which creates a Git worktree of `src` and can skip the dependency sync.

Clone source trees only. Do not clone an `out/` directory to a new path: GN writes absolute paths for the SDK and remote-execution roots, and whether Siso's state survives a move is unverified. Rebuild into a fresh output directory with a warm compiler cache instead.

### 7.2 Compiler cache

Use ccache through `cc_wrapper`. It cannot be combined with `use_remoteexec`; GN asserts on that.

First configuration to try:

```gn
cc_wrapper = "env CCACHE_SLOPPINESS=time_macros ccache"
```

```sh
CCACHE_DIR=$BDK_HOME/ccache
CCACHE_MAXSIZE=50G
CCACHE_COMPILERCHECK=content   # each checkout downloads its own clang; the default mtime check would miss
CCACHE_NOHASHDIR=1
```

What is verified and what is not:

- Verified on a toy C++ file: two checkouts at different paths share cache hits when the base directory is set per checkout and directory hashing is off, and the objects are byte-identical. Without those settings there are no hits.
- Not verified: hit rates on a real Chromium build, Objective-C++ and module behaviour, and sccache on macOS. Chromium compiles with paths relative to the output directory and sets `-ffile-compilation-dir=.`, which should make cross-checkout hits work without `CCACHE_BASEDIR`. Task T-CHR-06 measures this on the first real build.

Leave sccache until there is a reason to share a compiler cache between machines. Its local mode allows only one server process at a time.

## 8. Artifact manifest and provenance

Every runtime in the store and every published runtime carries `manifest.json`:

```json
{
  "schema": 1,
  "source_key": "e0fa7b2a276fe343",
  "bdk_abi": { "version": 1, "hash": "…" },
  "chromium": { "tag": "158.0.8127.37", "commit": "b8c2be85…", "deps_digest": "…" },
  "bdk": { "repo_commit": "…", "patches_digest": "…", "overlay_digest": "…", "gn_args_digest": "…" },
  "target": { "os": "macos", "arch": "arm64", "min_os": "13.0" },
  "builder": { "xcode": "27.2", "sdk": "27.0", "clang": "…", "os": "…", "depot_tools": "…", "ci_run": "…" },
  "files": { "BDKRuntime.framework/Versions/A/BDKRuntime": "sha256:…" },
  "attestation": "…"
}
```

Compatibility rules:

- The Swift package records the `bdk_abi` version and hash it was compiled against. Runtime start fails with a readable error on a mismatch, before Chromium initialises.
- A published runtime is accepted only if its manifest digest matches the value recorded in `chromium/pin.json` at that commit. Once runtimes are built in CI, the build attestation must verify as well.
- Source and prebuilt runtimes for the same source identity must expose the same ABI hash. The pre-release pass compares them.

GitHub artifact attestations can only be produced inside GitHub Actions. While runtimes are built on a developer's machine (section 10.1) there is no attestation: the `attestation` and `ci_run` fields are empty, and trust rests on the digest committed to `chromium/pin.json` through a reviewed pull request. Attestation returns when a CI build does.

### 8.1 Versioning and releases

BDK follows SemVer. There is one version for everything BDK ships: the Swift SDK, the C ABI headers, the `@bdk/*` packages, the CLI and the runtime. They are tied together by the ABI hash, so separate versions would only describe combinations nobody can use. SwiftPM also resolves versions from plain SemVer tags on the repository, so the tag is `vX.Y.Z` and not a per-package tag.

| Bump | Trigger |
|---|---|
| Major | A breaking change to the Swift API, the C ABI, the `@bdk/*` APIs, the CLI's contract, or the `bdk.toml` schema (a file that was valid stops validating or changes meaning) |
| Minor | Additive API or schema. A roll to a new Chromium milestone. |
| Patch | Fixes. A Chromium point or security release inside the same milestone. |

Before 1.0 the version is `0.y.z` and a breaking change bumps `y`. 1.0 is the release that passes the public v1 gates in the roadmap.

The Chromium version is not encoded in the BDK version. It is in the runtime manifest and in the release notes. Web-platform changes that arrive with a milestone are listed in the notes of that minor release.

How the existing version numbers relate to it:

| Number | Rule |
|---|---|
| `bdk.toml` schema | No number of its own. The schema is published with each release and follows the BDK version. Inside one major version every earlier valid fixture must still validate; CI enforces this with the fixture suite. |
| `bdk_abi_version()` | An integer that increases on any incompatible ABI change, which is also a major bump after 1.0. The hash is still checked for an exact match at start. |
| `manifest.json` and `bdk.resolved.json` `schema` | Integers for file formats that BDK both writes and reads. Bumped only on an incompatible change. |

Tooling now:

- Conventional Commits, checked on the pull-request title because merges are squashed, and by a Lefthook `commit-msg` hook. This costs little and leaves the history ready for release automation.
- Releases are cut by hand, following section 10.2.

Deferred, and not designed in detail:

- release-please in manifest mode with a single root component, maintaining a release pull request with the changelog and the next version.
- A `dev` branch that publishes beta prereleases (`X.Y.Z-beta.N`), with `main` carrying stable releases.
- An automated `release.yml`.

One ordering constraint: a SwiftPM binary target names the runtime archive's checksum inside `Package.swift`, and that file must be correct at the tagged commit, before the tag exists. By hand the order is: publish the archive, commit its checksum, then tag. Later automation has to keep that order, most likely by having the release pull request carry the checksum. SwiftPM also expects `Package.swift` at the repository root, so the manifest lives there (decided in M2, T-SWF-01) and its targets point at `packages/swift` and `packages/core`; T-PKG-04 adds the binary target to it.

## 9. Formatting, linting and hooks

| Language | Format | Lint | Notes |
|---|---|---|---|
| Swift | `swift format` (bundled with the Xcode toolchain) | SwiftLint | swift-format owns layout. SwiftLint keeps semantic rules only; its style rules that overlap are disabled in `.swiftlint.yml`. Both configurations are pinned in the repository. |
| C and C++ (BDK-owned) | `clang-format` | `clang-tidy` where a compilation database exists; `-Wall -Wextra -Werror` | `packages/core` uses the toolchain clang-format. `chromium/bdk` and patches use Chromium's pinned clang-format and style. Upstream files are never reformatted. |
| TypeScript, JavaScript, JSON | Biome | Biome, `tsc --noEmit` | Vitest for unit tests. |
| TOML | Taplo or Tombi | Same tool | Biome does not format TOML. Taplo's last release was in May 2025; Tombi is the fallback if it stalls. |
| `bdk.toml` | n/a | JSON Schema validation plus fixture tests | Valid and invalid fixtures live in `tests/config/`. Every schema change needs a compatibility fixture. |
| YAML, GitHub Actions | Biome (YAML) | actionlint | |
| Shell | shfmt | ShellCheck | Only for scripts that exist. |
| Python | none | none | No BDK-owned Python. Chromium's own Python is used as shipped. |

Lefthook, from the first commit:

```yaml
pre-commit:
  parallel: true
  commands:
    swift-format:   { glob: "*.swift", run: "swift format --in-place {staged_files}", stage_fixed: true }
    clang-format:   { glob: "{packages,chromium/bdk}/**/*.{h,cc,mm}", run: "clang-format -i {staged_files}", stage_fixed: true }
    biome:          { glob: "*.{ts,tsx,js,mjs,json,yml,yaml}", run: "pnpm exec biome check --write {staged_files}", stage_fixed: true }
    toml:           { glob: "*.toml", run: "taplo format {staged_files}", stage_fixed: true }
    shell:          { glob: "*.sh", run: "shfmt -w {staged_files} && shellcheck {staged_files}", stage_fixed: true }
    actionlint:     { glob: ".github/workflows/*.yml", run: "actionlint {staged_files}" }
    secrets:        { run: "gitleaks protect --staged --redact" }
commit-msg:
  commands:
    conventional:   { run: "pnpm exec commitlint --edit {1}" }
pre-push:
  commands:
    affected-lint:  { run: "pnpm exec nx affected -t lint" }
```

Hooks never start a Chromium build. CI repeats every check, so bypassing hooks gains nothing.

## 10. GitHub Actions

| Workflow | Trigger | Runner | Contents |
|---|---|---|---|
| `pr.yml` | Pull request | Hosted Linux for JS and lint; hosted macOS for Swift | `nx affected -t lint test build` with the stub runtime; patch-series apply check; `bdk.toml` schema fixtures; `cli:package` and a smoke test of the executable on both runners; actionlint; gitleaks |
| `integration.yml` | Pull request touching Swift, `packages/core`, `chromium/` or `tests/`; nightly | Hosted macOS | Downloads the runtime recorded in `chromium/pin.json`, verifies its digest, runs SDK integration tests, the MV3 fixture suite and the example apps' smoke tests. When a change alters the source key or the ABI hash and no matching runtime is published, it reports "runtime rebuild needed" and skips. |
| `chromium-roll.yml` | Daily schedule; manual | Hosted Linux | Detects a newer Chromium stable, bumps the pin on a branch, tries the patch series with no build, opens or updates the roll pull request (section 6.1) |
| `docs.yml` | Changes under `apps/docs` | Hosted Linux | Builds and publishes the Fumadocs site |

Rules for every workflow:

- External actions are pinned by commit SHA.
- Node is installed from `.node-version`, so CI, developers and the shipped CLI use the same release.
- Default `permissions` are `contents: read`. Jobs add what they need.
- No signing certificate or notarization key is stored in GitHub while releases are cut by hand. If an automated release workflow is added, they live only in a `release` environment that requires reviewers and is restricted to protected tags.
- Cloud storage is reached through OIDC, not stored keys.
- There is no self-hosted runner. If one is added later, it never runs jobs for pull requests from forks.

There is no workflow that builds Chromium, cuts a release or signs anything. Those are done by hand for now (sections 8.1 and 10.2).

Why CI does not build Chromium: hosted macOS runners have 3 vCPUs, 7 GB of memory and 14 GB of disk, with a six-hour job limit, and Chromium needs on the order of 100 GB of disk. A machine that can build it costs roughly $350 to $1,100 per month to rent. The product owner has decided against that for now. The runtime is built on a developer's Mac, and the complete suite runs there before a release.

### 10.1 What CI tests without a Chromium build

Standard GitHub-hosted runners are free for public repositories, including macOS, so CI costs nothing under this plan.

| Check | Needs | Runs on | Catches |
|---|---|---|---|
| Lint and format for every language, actionlint, gitleaks | Nothing | Hosted Linux | Style, secrets, workflow errors |
| Unit tests for Swift, TypeScript, the CLI and `packages/core` | Stub runtime | Hosted Linux and macOS | Logic errors above the ABI |
| The CLI packaged as an executable, then run with Node removed from the path | Nothing | Hosted Linux and macOS | A CLI that works under Node and fails as an executable |
| ABI conformance suite | Stub runtime | Hosted Linux and macOS | The SDK or the stub disagreeing with the ABI definition; headers that stop compiling as C |
| ABI hash check | The pin record | Hosted Linux | A header change that the published runtime does not implement |
| Code generation, `bdk.toml` schema fixtures, resolver fixtures | Nothing | Hosted Linux | Schema and binding drift |
| Patch-series apply | Sparse checkout of the pinned tag | Hosted Linux | A patch that no longer applies; a roll that conflicts |
| Format check of `chromium/bdk` with Chromium's clang-format | Nothing | Hosted Linux | Style only |
| SDK integration tests, MV3 fixture suite, example smoke tests | Last published runtime | Hosted macOS | Regressions in Swift, JavaScript, pages and tests against a real engine |
| Signing script with an ad hoc identity | Last published runtime | Hosted macOS | Re-signed helpers that no longer launch |
| Docs build | Nothing | Hosted Linux | Broken links and MDX |

What CI cannot see:

- Whether a change under `chromium/bdk` or `chromium/patches` compiles, links or behaves. Such a change alters the source key, no published runtime matches it, and the integration job says so and skips.
- Whether a Chromium roll works, beyond the patches applying.
- Notarization, and anything else that needs a real signing identity.

Two habits keep that blind spot small:

- Put logic that does not need Chromium in `packages/core`, where CI unit-tests it: route normalisation, capability tables, configuration resolution, handle and error plumbing. Keep `chromium/bdk` to code that has to touch Chromium.
- A pull request that changes `chromium/` merges only after its author has built it locally, run `pnpm test:release` and published the runtime. The integration job then has a runtime to run against.

Whether a Chromium-based runtime launches and renders on a hosted macOS runner, which has no real GPU, is unverified. If it does not, the integration and signing rows move to the local pass.

### 10.2 Before a release

Run on a developer's Mac, from a clean tree, against the source-built runtime:

1. `pnpm chromium:sync`, `pnpm chromium:build`, `pnpm chromium:package`.
2. `pnpm test:release`: the unit, integration, MV3 and end-to-end suites, the network audit, and the conformance suite against the real runtime.
3. `pnpm cli:package`; sign and notarize the `bdk` executable; place it in the Swift package's artifact bundle.
4. Sign and notarize the example application under a second team identifier.
5. `pnpm chromium:publish`, commit the pin record and the SwiftPM checksums, then tag.

Expected time, none of it measured:

| Step | Estimate |
|---|---|
| First checkout | Of the order of an hour and 100 GB |
| Full build, needed for every Chromium roll | About 90 minutes on an M2 Max in a 2023 report. Expect one to three hours on current high-end Apple Silicon and longer on a base machine. |
| Incremental build after a change in `chromium/bdk` or one patch | Minutes to tens of minutes |

Under section 6.1 a roll arrives with every new milestone, every two weeks, and with point releases between them. T-CHR-05 replaces these estimates with measurements.

When a `dev` branch starts producing beta releases, a build machine and a build workflow come back into the plan. Nothing here prevents that, because the build, package and publish targets are the ones a workflow would call.

## 11. Differences from the brief and the spec

| Source text | This plan | Why |
|---|---|---|
| Spec and brief: "GN/Ninja" | GN with `autoninja`, which uses Siso by default at M158 | Upstream changed the default executor. Ninja still works for an output directory that was first built with it. |
| Brief: use Nx caching for suitable packaged runtime artifacts | Nx caches the pointer; the artifact lives in the runtime store | Size, cache retention, and the stale-runtime hazard verified in E9. |
| Spec §5: generated declarations stay in Xcode derived data | True for downstream apps. Inside this monorepo, generated bindings are Nx outputs inside each package and are not committed, except those that a build which cannot run the generator compiles (amendments.md 2.14). | The spec text is about applications built with BDK. |
| Spec §5: `bdk doctor` checks toolchain, compatibility and signatures | Kept, with no disk reporting | Brief section 8. |
| Brief layout: `packages/core` is "native C++ bridge" | Split between `packages/core` (ABI, definitions, stub) and `chromium/bdk` (Chromium-linked code) | The Chromium-linked code cannot build outside a Chromium checkout. |
| Spec §5: `bdk.toml` is parsed at build and at runtime | One parser, in the CLI. The build writes a validated `bdk.resolved.json` into the app bundle and the engine reads only that. | Avoids a second TOML implementation inside the engine and keeps validation in one place. |

## 12. What is verified and what is not

| Claim | Status |
|---|---|
| Nx 24 runs C++, Swift and TypeScript targets from root scripts with correct ordering | Verified (E8) |
| Nx caching restores native outputs and invalidates on source, interface and toolchain changes | Verified (E8) |
| Nx does not infer native dependencies | Verified (E8) |
| Two worktrees share one immutable runtime and cannot modify it | Verified with a dummy runtime (E9) |
| Conflicting patch sets get separate keys, output directories and runtimes | Verified with dummy builds (E9) |
| Exclusive checkout lock with owner reporting and release on exit | Verified on macOS (E9) |
| A cached resolve task hides a tampered or evicted runtime; an uncached or probed one does not | Verified (E9) |
| APFS clone cost for a checkout-like tree | Verified at small scale (E9) |
| ccache hits across checkout paths | Verified on a toy file only |
| Everything above on a real Chromium checkout and build | Unverified; tasks T-CHR-01 to T-CHR-07 |
| Nx behaviour with concurrent runs writing very large outputs | Unverified, and avoided by design |
| Linux and Windows equivalents of `lockf` and APFS clones | Unverified; `flock` and reflink copies are the expected equivalents |
| A Node single executable of the CLI runs from a Swift package artifact bundle and an Xcode build phase | Verified for the executable, the artifact bundle under SwiftPM, no Node on `PATH` and start-up time (2026-11-18, [sdk/cli.md](sdk/cli.md) section 2.3). Not verified in an Xcode run-script phase. |
