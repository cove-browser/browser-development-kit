# @bdk/cli

The `bdk` command line tool, in TypeScript, shipped as one self-contained executable built with Node's own
`node --build-sea`. Design: [docs/planning/sdk/cli.md](../../docs/planning/sdk/cli.md) (sections 2 and 3).
Task: T-CLI-05. This is the foundation; commands are added as their tasks land.

## Commands

| Command | Status |
|---|---|
| `bdk build config [--out <file>]` | Resolves `bdk.toml` against the embedded schema (`assets/bdk.schema.json`) and, on macOS inside Xcode, the `Info.plist` and entitlements Xcode names. Writes `bdk.resolved.json` next to `bdk.toml` (or to `--out`) only when there are no errors. The resolver itself is T-CFG-02 (`src/core/config/`). |
| `bdk build sign --bundle <App.app> --identity <name\|-> [--team <id>] [--entitlements <file>]` | Re-signs the application and the runtime nested in it with the application's identity (T-PKG-03): rewrites each helper's `CFBundleIdentifier` to `<app id>.helper[.renderer]` (Alerts: `<app id>.framework.AlertNotificationService`), then signs inside-out (libraries, loose executables, helpers with the JIT entitlement for renderer and GPU, framework, application) and verifies with `codesign --verify --deep --strict`. Refuses an application entitlement on the deny list. `BDK_SKIP_SIGNING=1` skips it. In Xcode the flags default to `CODESIGNING_FOLDER_PATH`, `EXPANDED_CODE_SIGN_IDENTITY`, `DEVELOPMENT_TEAM` and `CODE_SIGN_ENTITLEMENTS`. An ad hoc identity (`-`) gets no `--options`; see [sdk/cli.md](../../docs/planning/sdk/cli.md) section 5.1. |
| `bdk build automation --bundle <App.app> [--out <file>]` | Reads `bdk.resolved.json` (run `build config` first) and bundles each `automation.scripts.<name>` with Rolldown into one ES module with no imports at `<App.app>/Contents/Resources/BDK/Automation/<name>.js`, then adds `"bundle": "BDK/Automation/<name>.js"` and `"bundle_digest": "sha256:..."` to that script in the resolved file and recomputes `config_digest`. The automation host provides no modules, so `@bdk/automation`, `@bdk/automation/{client,commands,events}`, `@bdk/commands` and `@bdk/events` are inlined from the `@bdk/automation` package (the project's own copy, else `packages/js/automation` in the repository), and so is any other package that resolves inside the project. A Node built-in is `automation.import_not_allowed`, an import that does not resolve is `automation.import_unresolved`, a Rolldown error is `automation.bundle_failed`. Rolldown is loaded from the project's `node_modules` (the executable cannot carry its native binding), else from the CLI's own dependency; with neither the step fails with `automation.bundler_missing` (exit 3). Incremental: a script is rebuilt only when it, a module it imported or the output changed (`.bdk/automation-cache.json` in the project records the digests). With `[automation] enabled = false` or no scripts it does nothing. `--bundle` defaults to `CODESIGNING_FOLDER_PATH`. |
| `bdk build devtools --bundle <App.app> [--out <file>]` | Reads `devtools` from `bdk.resolved.json` (run `build config` first) and packages it in the application for the engine (javascript-api.md section 6). A path `frontend` is copied whole (replacing what was there) to `<App.app>/Contents/Resources/BDK/DevTools/Frontend/` and must hold `index.html` (`devtools.entry_missing`). With `frontend = "default"` and an `overlay` directory, `overlay.css` and `overlay.js` are copied to `.../BDK/DevTools/Overlay/`; neither present is `devtools.overlay_empty`, and any other file is not copied (one `devtools.overlay_files_ignored` warning lists them). An overlay beside a replacement or a disabled front end is `devtools.overlay_ignored` (warning). With neither, `Contents/Resources/BDK/DevTools` is removed and the step says nothing was packaged. A path outside the project is `config.path_escape`, a path that is not a directory is `devtools.not_directory`, and a value that looks like a URL is `devtools.url_not_allowed`: resources are packaged in the signed bundle, never loaded from a URL. Incremental: nothing is copied when the digests of the inputs and of the packaged tree match `.bdk/devtools-cache.json`. `--json` data is `{mode, packaged, unchanged, files}`. `--bundle` defaults to `CODESIGNING_FOLDER_PATH`. |

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

## Signing

The plan (`src/core/signing/`) is pure and unit-tested against a fake bundle tree with a fake `codesign` and `plutil` runner (`src/platform/xcode/tools.ts` is the only place either is started). Not built: `bdk build embed`.

### The end-to-end signing test (opt in)

`src/platform/xcode/sign.runtime.test.ts` re-signs a real packaged runtime. It is skipped, with a warning, unless `BDK_TEST_RUNTIME_APP` names a runtime's `Chromium.app`:

```sh
BDK_TEST_RUNTIME_APP=~/.bdk/runtimes/<key>/Chromium.app pnpm exec nx run cli:test
```

It clones the runtime with `ditto` (a copy-on-write clone on APFS, so it takes under a second and the shared original is never modified), makes the clone a second application (`com.second.Browser`), signs it ad hoc, then checks `codesign --verify --deep --strict`, the helper identifiers and entitlements, launches it with `--headless=new --dump-dom data:text/html,<p>hi</p>` and a fresh `--user-data-dir`, and records what `spctl --assess` says (a rejection is the expected result for an ad hoc signature). It takes about 10 seconds. It needs no Chromium build and no credentials. Notarization is not tested.

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
| Re-signing a packaged runtime as another application, verifying it and launching it headless | Pass, ad hoc (`sign.runtime.test.ts`; details in cli.md section 5.1). `spctl` rejects it, as expected. Notarization and a real Developer ID identity were not tested. |
| Gatekeeper with a signed and notarized binary | Not verified; release procedure (T-PKG-06) |
| Vite loaded from a project's `node_modules` with the embedded Node | Not verified; waits for pages (T-CLI-04) |
| The embedded asset file system (`node:sea` virtual file system) | Not used; plain named assets only, per risk R31 |
