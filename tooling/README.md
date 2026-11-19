# tooling

TypeScript and shell helpers called by Nx targets.

## Convention for running tooling TypeScript

Run source directly with tsx through pnpm; there is no build step for tooling commands:

```sh
pnpm exec tsx tooling/src/<file>.ts
```

Nx targets (for example `chromium:sync`) call `pnpm exec tsx tooling/src/chromium/<command>.ts` from the
workspace root. Import siblings with a `.js` extension (`./paths.js`), as NodeNext requires. The `build`
target only type-checks and emits `dist/` for consumers; nothing at runtime depends on it.

## Shared modules

- `src/paths.ts`: `BDK_HOME` resolution and the subpaths under it. Rejects paths with spaces.
- `src/toolchain.ts`: version checks used by `setup`.
- `toolchain-id.sh`: the `nativeToolchain` Nx input.

- `src/lock.ts`: checkout locks. `runUnderCheckoutLock(checkout, argv)` runs under `lockf -t 0 -k` (macOS)
  or `flock -n` (Linux) on `$BDK_HOME/chromium/locks/managed.lock` or `exp-<id>.lock`, with an owner
  record `*.owner.json`; a busy lock fails at once with `LockBusyError` (exit 75 from the commands).
- `src/git.ts`: process and git helpers.

## Chromium commands (`src/chromium/`)

All take `--experiment <id>` (default: the managed checkout) and re-execute themselves under the
checkout lock, so a whole sync, patch and build sequence is one lock acquisition. See
`chromium/patches/README.md`, `chromium/gclient/README.md`, `chromium/gn/README.md`.

- `chromium:sync [--force] [--no-patch] [--no-hooks]`: fetch the pinned tag at depth 1, gclient sync
  to the pinned SHA, verify, apply patches. Refuses to discard uncommitted work or commits BDK did not
  produce. If `src` is already the pin plus exactly the series, it is left untouched and only the
  dependencies are synced, so the next build has nothing to do; `--force` and `--no-patch` reset to
  the pin instead.
- `chromium:patch-apply [--force]`, `chromium:patch-export`, `chromium:patch-check`.
- `chromium:build [--config dev|release] [--targets a,b | targets...] [--ccache-maxsize 50G] [--no-patch]
  [--no-verify] [--force]`: apply patches, mount `//bdk` (content-diff copy of `chromium/bdk`, plus `packages/core/runtime`, `include/bdk` and `generated` under `//bdk/core`), `gn gen`, `autoninja`
  (default target `chrome`) with `CCACHE_DIR=$BDK_HOME/ccache`, `CCACHE_MAXSIZE` (flag, or
  `BDK_CCACHE_MAXSIZE`, default 50G), `CCACHE_COMPILERCHECK=content`, `CCACHE_NOHASHDIR=1`. Prints
  full or incremental timings and appends them to `out/<config>/bdk-build-timings.jsonl`. After a
  `chrome` build it runs `nm -gU` on the framework and fails if `_bdk_overlay_marker` is missing.
- `chromium:experiment create <id> [--from managed|<id>] [--method clone|worktree]`, `remove <id>`,
  `list [--json]`. `clone` is an APFS `cp -cR` of the source tree without `out/` (refused when the
  clone probe fails, i.e. not APFS or another volume); `worktree` runs Chromium's
  `tools/git/worktree.py --light`, after which `chromium:sync --experiment <id>` fetches dependencies.
  Creation holds the source's lock and the experiment's own lock.

### Updating the pin in place

A roll is `chromium/pin.json` changing, then the same three commands on the managed checkout:

```sh
pnpm exec nx run chromium:sync      # fetches only the new tag, moves src, re-applies the series
pnpm exec nx run chromium:build     # incremental: only what the roll changed, then a relink
pnpm exec nx run chromium:package   # a new runtime; the previous one stays in the store
```

Git rewrites only the files that differ between the two tags, so Siso recompiles those and their
dependants, and ccache serves anything it has compiled before. How much that is depends on the roll:

- **Within a release branch** the compiler is fixed, so the build is incremental, but its size
  depends on which dependencies moved. `158.0.8127.37` to `.65` (28 patch releases; V8, Skia, ICU,
  ANGLE, PDFium, Dawn and LevelDB moved) took 12,868 compiles, 12,785 of them new: 83 minutes on an
  M2 Max. Rolling back, with every object already in ccache: 12,867 hits, 8 minutes.
- **To a new milestone** (`158` to `159`) the compiler changes, and every compile misses: about three
  hours. Building the new branch's beta tag ahead of time in an experiment checkout would fill the
  cache so that the stable roll is incremental on the day; that is not automated yet.
- **Nothing changed**: a sync takes about 20 s and rewrites only hook outputs; the next build takes
  17 s.

Environment: `BDK_HOME`, `BDK_CHROMIUM_URL`, `BDK_DEPOT_TOOLS_URL`, `BDK_PATCH_CHECK_REMOTE`,
`BDK_CCACHE_MAXSIZE`, `BDK_NM`, `BDK_TAR` (the tar binary; GNU tar and bsdtar both work), and
`BDK_REPO_ROOT` (tests).

Tests use fixture repositories and a fake depot_tools (`src/chromium/test-support.ts`), never a real checkout.

## Chromium roll bot (`src/roll/`)

`pnpm exec nx run chromium:roll [-- --dry-run]` (or `pnpm exec tsx tooling/src/roll/roll.ts`) is the script behind
`.github/workflows/chromium-roll.yml` (repository and tooling plan, section 6.1). Steps:

1. Detect: the highest stable Mac version from the VersionHistory API, falling back to Chromium Dash.
   Versions are compared numerically. If it is not newer than `chromium/pin.json`, it prints "pin is up to
   date" and exits 0 without touching git or GitHub. Chromium Dash is also read for release dates.
2. Resolve: `git ls-remote <remote> refs/tags/<v> refs/tags/<v>^{}`, preferring the peeled commit. A tag the
   remote does not have yet is reported and the run ends with exit 0 (the next run retries).
3. Check: `checkPatchSeries` (the `chromium:patch-check` library) at the new tag.
4. Bump: branch `chromium-roll/<version>`, `tag` and `commit` rewritten, `runtime` reset to `null`, other
   fields kept; one commit with the PR title.
5. Pull request: title `feat(chromium): roll to <v>` when the milestone (first component) changes,
   `fix(chromium): roll to <v>` for a point release. The body links the Chrome Releases blog search for the
   version (not every build has a post) and the source log, gives days behind, and lists each failing patch
   with its conflicting files. A failing series makes it a draft labelled `needs-rebase`. If any open PR has a
   head starting with `chromium-roll/`, that PR is updated (title, body, draft state, label) and no second
   one is opened; its existing branch name is kept, since a head cannot be renamed. If that branch already
   pins the detected version it is not pushed again, so a runtime record a person pushed there survives.
6. Issue: if the pin has been behind more than `--stale-days` (default 14, or `BDK_ROLL_STALE_DAYS`) an issue
   `Chromium pin is N days behind stable`, labelled `chromium-pin-stale`, is opened or edited (risk R4).
   "Days behind" is counted from the release date of the oldest stable release newer than the pin, as
   Chromium Dash reports it; with no date the PR says "unknown" and no issue is raised.

Options: `--dry-run` (read-only: detects, resolves and runs the patch check, prints the plan and the PR or issue
text, creates no branch, pushes nothing, never calls `gh`), `--stale-days N`, `--remote R` (default the GitHub
mirror, or `BDK_PATCH_CHECK_REMOTE`), `--base B` (default `main`). Real runs need `gh` authenticated by
`GH_TOKEN` and a clean working tree. All GitHub and push operations go through the `GitHubClient` interface
(`github.ts`): `GhCliClient` for real, `DryRunClient` for `--dry-run`, and a fake in the tests, which use
fixture remotes and injected `fetch` only.

## `pnpm run setup`

pnpm has a built-in `setup` command (it edits shell profiles), and built-ins win over scripts, so
`pnpm setup` never reaches the script. Use `pnpm run setup` (or `pnpm exec nx run tooling:setup`).

## Runtime store, package, resolver and publish (`src/runtime/`, `src/chromium/{package,runtime,publish}.ts`)

Design: `docs/planning/repository-and-tooling.md` sections 5.2, 5.3, 7.1, 8 and 10.2.

### Commands

| Command | Does |
|---|---|
| `pnpm chromium:package [--config release] [--experiment <id>] [--reproducible-check]` | Under the checkout lock: copies the files listed in `chromium/runtime-files.json` from `out/<config>` of the managed (or experiment) checkout into a temp dir in the store, writes and verifies `manifest.json`, makes it read-only and renames it to `runtimes/<source-key>/`. Refuses a checkout that is not the pin plus the current patch series (clean), and refuses when `packages/core/generated/abi-hash.txt` is stale (`cmake -DMODE=check`). Idempotent for identical content, after re-verifying the existing store entry against its manifest; an error when the key exists with different content or the entry is damaged (it is never replaced silently: remove it as the message says). Symlinks must resolve inside the runtime through the real filesystem. A declared file that is missing fails packaging and is named. `--reproducible-check` packages twice into throwaway stores and compares. |
| `pnpm chromium:runtime [--config release] [--allow-source-build]` | Never cached. Computes the source key, re-verifies the manifest and every file digest in `runtimes/<key>`, checks the manifest digest and ABI hash against `pin.json` when it records a runtime for this key (not in `source` mode), writes `.bdk/runtime.json` and the `.bdk/runtime` symlink. If the key is missing and `pin.json` has a published runtime for it, downloads, checks the archive SHA-256 and then the manifest digest against `pin.json`, unpacks via temp and rename. Otherwise fails naming the key and the remedies. |
| `pnpm chromium:publish [--backend github\|file] [--dest file:///dir] [--repo owner/name] [--config release] [--dry-run]` | Clean tree only (`git status --porcelain` empty). Archives `runtimes/<key>`, uploads, writes the `runtime` record into `chromium/pin.json`. `github` uses `gh release upload` to release `runtime-<key>` of `origin` (created if absent, never overwrites). `file` copies into a directory. |
| `pnpm test:release` | Adds `chromium:test-release`: the runtime/resolver suite, then the conformance step, which skips with a message while no runtime is published or the suite is not written. |

`BDK_RUNTIME_MODE=source` makes the resolver use only a locally packaged runtime (no download).
`--allow-source-build` only changes the failure message when `pin.json` has no published runtime: the source
build is the remedy. A runtime already in the store for the computed key is always accepted, because equal keys
mean equal inputs.

### Source key (what names `runtimes/<key>`)

First 16 hex digits of SHA-256 over the canonical JSON (sorted keys, no whitespace) of: Chromium tag and commit;
BDK ABI version and ABI hash; SHA-256 digest of every file in `chromium/patches`, in `chromium/bdk` and in
`packages/core/runtime` (the runtime core the engine compiles; path-sorted, relative path hashed in; `README.md` and `.DS_Store` at the top level are ignored so documentation
edits do not force a rebuild); digest of `chromium/gn/<config>.gn` (`absent` if the preset file does not exist,
which packaging refuses); digest of `chromium/runtime-files.json` (its sorted paths and `target.min_os`, canonical
JSON: what is packaged decides what the key names); target OS and architecture. The key's own `key_schema` is 3.
The ABI hash is in the key as well as the version: the
runtime must report exactly the hash the SDK was compiled against, so a header change is a new key.
Version and hash come from `src/runtime/abi.ts` (header `BDK_ABI_VERSION` and `packages/core/generated/abi-hash.txt`).

The build identity (Xcode, SDK, clang, OS, depot_tools revision, timestamp, CI run) is in the manifest only, so a
toolchain change leaves the key alone and changes the manifest digest.

### The deps digest

Decision: `deps_digest` is NOT in the key, and is recorded in the manifest at package time (SHA-256 of the
checkout's `src/DEPS`). Reason: the prebuilt path must compute the key with no Chromium checkout, and the pinned
commit already determines `DEPS` byte for byte, so adding a digest of it to the key would add no information.
The alternative (storing it in `pin.json` when the pin changes) would add a second field that has to be kept in
step with `commit`, and nothing can verify it without a checkout. Packaging refuses a checkout that is not exactly
the pinned commit plus exactly the current `chromium/patches` series with no uncommitted tracked changes (a built
checkout's `HEAD` is the series tip, never the pin itself), so the manifest value always belongs to the pin. Local edits to `DEPS` or to `.gclient` do not
change the key; that is the managed checkout's job to prevent (`chromium:sync` resets to the pin).

### Manifest

`manifest.json` follows section 8 (`schema` 1, `source_key`, `bdk_abi`, `chromium`, `bdk`, `target`, `builder`,
`files`, `attestation` empty). `builder` also has `timestamp`. The manifest digest is the SHA-256 of the
canonical JSON of the parsed manifest, so indentation and key order in the file do not matter. `files` values:
`sha256:<hex>`, `sha256:<hex>:exec` for executables, `symlink:<target>` for symlinks (never followed; framework
`Versions/Current` links are preserved). Verification fails on missing, modified or unexpected entries.

### pin.json `runtime` record

```json
{ "source_key": "…16 hex…", "url": "https://… or file:///…", "archive_format": "tar.zst",
  "archive_sha256": "…", "manifest_sha256": "…", "abi_hash": "…", "size": 123 }
```

`null` until something is published. The record is not an input to the key. `archive_sha256` is checked before
unpacking, `manifest_sha256` after.

### Archives

`tar --format gnutar` over a sorted file list with owner 0:0, no extended attributes or ACLs, then `zstd -T1 -10`
(gzip `-n -9` when `zstd` is absent). Packaging sets every mtime to 2020-01-01 so the bytes depend only on content
and the manifest. Two packagings differ in their archives only through `builder.timestamp`.

### Why `pnpm build` does not need a runtime yet

`swift:build`, `swift:test` and the app `build` targets list `dependentTasksOutputFiles: "**/runtime.json"` and the
fileset `.bdk/runtime.json` as inputs, so once `chromium:runtime` has run, their cache keys include the key and
manifest digest. No `dependsOn` on `chromium:runtime` is declared: until a runtime is published, `pnpm build` and
`pnpm test` use the stub runtime and must not fail for lack of one. The M2 integration targets add
`dependsOn: chromium:runtime` when they exist (`swift:test-integration`, `test-e2e`).
