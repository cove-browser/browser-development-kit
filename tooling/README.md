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

- `chromium:sync [--force] [--no-patch] [--no-hooks]`: gclient sync to the pin, verify, apply patches.
- `chromium:patch-apply [--force]`, `chromium:patch-export`, `chromium:patch-check`.
- `chromium:build [--config dev|release] [--targets a,b | targets...] [--ccache-maxsize 50G] [--no-patch]
  [--no-verify] [--force]`: apply patches, mount `//bdk` (content-diff copy), `gn gen`, `autoninja`
  (default target `chrome`) with `CCACHE_DIR=$BDK_HOME/ccache`, `CCACHE_MAXSIZE` (flag, or
  `BDK_CCACHE_MAXSIZE`, default 50G), `CCACHE_COMPILERCHECK=content`, `CCACHE_NOHASHDIR=1`. Prints
  full or incremental timings and appends them to `out/<config>/bdk-build-timings.jsonl`. After a
  `chrome` build it runs `nm -gU` on the framework and fails if `_bdk_overlay_marker` is missing.
- `chromium:experiment create <id> [--from managed|<id>] [--method clone|worktree]`, `remove <id>`,
  `list [--json]`. `clone` is an APFS `cp -cR` of the source tree without `out/` (refused when the
  clone probe fails, i.e. not APFS or another volume); `worktree` runs Chromium's
  `tools/git/worktree.py --light`, after which `chromium:sync --experiment <id>` fetches dependencies.
  Creation holds the source's lock and the experiment's own lock.

Environment: `BDK_HOME`, `BDK_CHROMIUM_URL`, `BDK_DEPOT_TOOLS_URL`, `BDK_PATCH_CHECK_REMOTE`,
`BDK_CCACHE_MAXSIZE`, `BDK_NM`, and `BDK_REPO_ROOT` (tests).

Tests use fixture repositories and a fake depot_tools (`src/chromium/test-support.ts`), never a real checkout.

## `pnpm run setup`

pnpm has a built-in `setup` command (it edits shell profiles), and built-ins win over scripts, so
`pnpm setup` never reaches the script. Use `pnpm run setup` (or `pnpm exec nx run tooling:setup`).

## Runtime store, package, resolver and publish (`src/runtime/`, `src/chromium/{package,runtime,publish}.ts`)

Design: `docs/planning/repository-and-tooling.md` sections 5.2, 5.3, 7.1, 8 and 10.2.

### Commands

| Command | Does |
|---|---|
| `pnpm chromium:package [--config release] [--experiment <id>] [--reproducible-check]` | Under the checkout lock: copies the files listed in `chromium/runtime-files.json` from `out/<config>` of the managed (or experiment) checkout into a temp dir in the store, writes and verifies `manifest.json`, makes it read-only and renames it to `runtimes/<source-key>/`. Idempotent for identical content; an error when the key exists with different content. A declared file that is missing fails packaging and is named. `--reproducible-check` packages twice into throwaway stores and compares. |
| `pnpm chromium:runtime [--config release] [--allow-source-build]` | Never cached. Computes the source key, re-verifies the manifest and every file digest in `runtimes/<key>`, writes `.bdk/runtime.json` and the `.bdk/runtime` symlink. If the key is missing and `pin.json` has a published runtime for it, downloads, checks the archive SHA-256 and then the manifest digest against `pin.json`, unpacks via temp and rename. Otherwise fails naming the key and the remedies. |
| `pnpm chromium:publish [--backend github\|file] [--dest file:///dir] [--repo owner/name] [--config release] [--dry-run]` | Clean tree only (`git status --porcelain` empty). Archives `runtimes/<key>`, uploads, writes the `runtime` record into `chromium/pin.json`. `github` uses `gh release upload` to release `runtime-<key>` of `origin` (created if absent, never overwrites). `file` copies into a directory. |
| `pnpm test:release` | Adds `chromium:test-release`: the runtime/resolver suite, then the conformance step, which skips with a message while no runtime is published or the suite is not written. |

`BDK_RUNTIME_MODE=source` makes the resolver use only a locally packaged runtime (no download).
`--allow-source-build` only changes the failure message when `pin.json` has no published runtime: the source
build is the remedy. A runtime already in the store for the computed key is always accepted, because equal keys
mean equal inputs.

### Source key (what names `runtimes/<key>`)

First 16 hex digits of SHA-256 over the canonical JSON (sorted keys, no whitespace) of: Chromium tag and commit;
BDK ABI version and ABI hash; SHA-256 digest of every file in `chromium/patches` and in `chromium/bdk`
(path-sorted, relative path hashed in; `README.md` and `.DS_Store` at the top level are ignored so documentation
edits do not force a rebuild); digest of `chromium/gn/<config>.gn` (`absent` if the preset file does not exist,
which packaging refuses); target OS and architecture. The ABI hash is in the key as well as the version: the
runtime must report exactly the hash the SDK was compiled against, so a header change is a new key.
Version and hash come from `src/runtime/abi.ts` (header `BDK_ABI_VERSION` and `packages/core/generated/abi-hash.txt`).

The build identity (Xcode, SDK, clang, OS, depot_tools revision, timestamp, CI run) is in the manifest only, so a
toolchain change leaves the key alone and changes the manifest digest.

### The deps digest

Decision: `deps_digest` is NOT in the key, and is recorded in the manifest at package time (SHA-256 of the
checkout's `src/DEPS`). Reason: the prebuilt path must compute the key with no Chromium checkout, and the pinned
commit already determines `DEPS` byte for byte, so adding a digest of it to the key would add no information.
The alternative (storing it in `pin.json` when the pin changes) would add a second field that has to be kept in
step with `commit`, and nothing can verify it without a checkout. Packaging refuses a checkout whose `HEAD` is not
the pinned commit, so the manifest value always belongs to the pin. Local edits to `DEPS` or to `.gclient` do not
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
