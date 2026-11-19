# @bdk/cli

The `bdk` command line tool, in TypeScript, shipped as one self-contained executable built with Node's own
`node --build-sea`. Design: [docs/planning/sdk/cli.md](../../docs/planning/sdk/cli.md) (sections 2 and 3).
Task: T-CLI-05. This is the foundation; commands are added as their tasks land.

## Commands

| Command | Status |
|---|---|
| `bdk build config [--out <file>]` | Resolves `bdk.toml` against the embedded schema (`assets/bdk.schema.json`) and, on macOS inside Xcode, the `Info.plist` and entitlements Xcode names. Writes `bdk.resolved.json` next to `bdk.toml` (or to `--out`) only when there are no errors. The resolver itself is T-CFG-02 (`src/core/config/`). |

Global flags: `--help`, `--version`, `--json` (one JSON document on standard output; diagnostics stay on standard
error), `--project <dir>` (otherwise the nearest ancestor with `bdk.toml`), `--offline`, and
`--diagnostics human|xcode` to force a diagnostics form. Exit codes 0 to 4 are those in cli.md section 3. An unknown
command or step is a usage error (exit 2) that names what exists. Only commands that work are registered.

Diagnostics print as `file:line: error: [code] message` (with the fix as a `note:`) when `XCODE_VERSION_ACTUAL` is
set, as Xcode sets it for build phases, and in a human form otherwise. Colour only on a terminal, and not with
`NO_COLOR`.

## Layout

```text
src/
├── main.ts          # the entry point; the only file that wires the real process, assets and platform
├── assets.ts        # the one adapter for embedded assets (node:sea, or assets/ when unpackaged)
├── core/            # platform-neutral: args, project discovery, diagnostics, steps, config seam
└── platform/
    ├── platform.ts  # the interface the core sees
    ├── select.ts    # the one place a platform module is chosen
    ├── xcode/       # macOS: Xcode environment, Info.plist and entitlements through plutil
    └── generic/     # every other host, until it has a module
assets/              # files embedded in the executable (see its README)
scripts/             # package.ts: the packaging script run by cli:package
```

### The import rule

The core never imports a platform module. Biome's `noRestrictedImports`, set for `packages/cli/src/core/**` in
the root `biome.json`, rejects any import of `platform/**` there, except the interface file `platform/platform.js`.
`cli:lint` fails on a violation. `src/boundaries.test.ts` runs Biome with the repository's configuration on
fixtures to prove the rule fires, and checks that only `platform/select.ts` imports the platform modules and only
`assets.ts` imports `node:sea`. The core receives everything through `Context` (`src/core/context.ts`), which is
how tests supply fakes.

## Targets

| Command | What it does |
|---|---|
| `pnpm exec nx run cli:lint` | Biome, then `tsc --noEmit` for `src` and `scripts` |
| `pnpm exec nx run cli:test` | vitest. The packaged-executable tests run when `dist/bdk` exists and are reported as skipped, with the reason, when it does not |
| `pnpm exec nx run cli:build` | `tsc` to `dist/` (type-checked JavaScript; not what ships) |
| `pnpm cli:package` | Produces `dist/bdk` (and, on macOS, `dist/bdk.artifactbundle/`) |

### How `cli:package` works

`scripts/package.ts`:

1. Bundles `src/main.ts` into one CommonJS file with Rolldown (`dist/sea/bdk.cjs`). The version from `package.json`
   is substituted for `__BDK_CLI_VERSION__` at this point; unpackaged runs read `package.json` instead.
2. Lists every file under `assets/` (except `assets/README.md`) and writes `dist/sea/sea-config.json` with them as
   named assets. A new asset needs no configuration change.
3. Runs `node --build-sea` with the Node that runs the script (the release pinned in `.node-version`), with code
   cache on. Code cache ties the executable to that Node build and architecture, so cross builds would need it off.
4. On macOS, signs ad hoc (`codesign --sign - --force`) and lays out `dist/bdk.artifactbundle/` with an `info.json`
   (schema version 1.0, artifact `bdk`, type `executable`, variant for `arm64-apple-macosx`).

Signing with a Developer ID identity and notarization are part of the release procedure (T-PKG-06), not done here.

## What was verified (2026-11-18, macOS 27, Apple Silicon, Node 26.9.0)

| Check | Result |
|---|---|
| `cli:lint`, `cli:test`, `cli:build`, `cli:package` | Pass |
| `bdk --version`, `--help`, `build config`, exit codes 1 and 2 with every directory that holds `node` removed from `PATH` | Pass (`src/packaged.test.ts`, and the `cli-smoke` job in `pr.yml`) |
| An embedded asset is read inside the executable | Pass, with a throwaway `assets/bdk.schema.json`: `bdk build config` reached the resolver instead of failing with `cli.asset_missing`. The file was removed again; the real schema comes with T-CFG-01. Without it the executable reports `cli.asset_missing` (exit 3), which the packaged test checks. |
| Start-up, `bdk --version`, 50 runs, packaged | Median 36 to 39 ms (min 34); `node -e 0` on the same machine: 33 ms. `--help` and `build config` on a tiny project: 41 to 49 ms median. |
| Executable size | 145 MB (a copy of the Node binary plus a 17 KB bundle) |
| The artifact bundle works as a SwiftPM binary target | Pass: a throwaway package with `.binaryTarget(path: "bdk.artifactbundle")` and a command plugin ran `bdk --version` (printed `0.0.0`, status 0); a build-tool plugin ran it during `swift build`, inside SwiftPM's sandbox |
| Run from an Xcode run-script phase, or Xcode's own build of the package | Not verified. `xcodebuild` on this machine reports that CoreSimulator is out of date and found no scheme in the throwaway package. The Xcode environment is simulated in tests by setting `XCODE_VERSION_ACTUAL`. |
| Gatekeeper with a signed and notarized binary | Not verified; release procedure (T-PKG-06) |
| Vite loaded from a project's `node_modules` with the embedded Node | Not verified; waits for pages (T-CLI-04) |
| The embedded asset file system (`node:sea` virtual file system) | Not used; plain named assets only, per risk R31 |
