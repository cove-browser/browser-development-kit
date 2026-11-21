# @bdk/cli

The `bdk` command line tool, in TypeScript, shipped as one self-contained executable built with Node's own
`node --build-sea`. Design: [docs/planning/sdk/cli.md](../../docs/planning/sdk/cli.md) (sections 2 and 3).
Task: T-CLI-05. This is the foundation; commands are added as their tasks land.

## Commands

| Command | Status |
|---|---|
| `bdk build config [--out <file>]` | Resolves `bdk.toml` against the embedded schema (`assets/bdk.schema.json`) and, on macOS inside Xcode, the `Info.plist` and entitlements Xcode names. Writes `bdk.resolved.json` next to `bdk.toml` (or to `--out`) only when there are no errors. The resolver itself is T-CFG-02 (`src/core/config/`). |
| `bdk build pages [--bundle <App.app>] [--resolved <file>]` | Packages the bundled pages and the SDK into the application (T-CLI-04, see "Pages" below). Reads the `mode: "bundled"` routes of `bdk.resolved.json` (next to `bdk.toml`, or `--resolved`; missing means run `bdk build config` first) and writes each page to `<App.app>/Contents/Resources/<route path>`, replacing what was there. With `Pages/vite.config.*` it runs the project's own Vite once per page; otherwise it copies the page directory as it is. Refuses output the content security policy would block (`pages.csp`) and any file over 16 MiB (`pages.too_large`). Copies the `@bdk/internal` modules to `Contents/Resources/BDK/js/internal/`. In Xcode `--bundle` defaults to `CODESIGNING_FOLDER_PATH`, else `TARGET_BUILD_DIR/WRAPPER_NAME`. `--watch` is not built. |
| `bdk build sign --bundle <App.app> --identity <name\|-> [--team <id>] [--entitlements <file>]` | Re-signs the application and the runtime nested in it with the application's identity (T-PKG-03): rewrites each helper's `CFBundleIdentifier` to `<app id>.helper[.renderer]` (Alerts: `<app id>.framework.AlertNotificationService`), then signs inside-out (libraries, loose executables, helpers with the JIT entitlement for renderer and GPU, framework, application) and verifies with `codesign --verify --deep --strict`. Refuses an application entitlement on the deny list. `BDK_SKIP_SIGNING=1` skips it. In Xcode the flags default to `CODESIGNING_FOLDER_PATH`, `EXPANDED_CODE_SIGN_IDENTITY`, `DEVELOPMENT_TEAM` and `CODE_SIGN_ENTITLEMENTS`. An ad hoc identity (`-`) gets no `--options`; see [sdk/cli.md](../../docs/planning/sdk/cli.md) section 5.1. |

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
├── asset-sources.ts # where the SDK modules come from (packages/js/internal/dist) and the embedded asset index
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
2. Lists every file under `assets/` (except `assets/README.md`), adds the built `@bdk/internal` modules
   (`packages/js/internal/dist/*.js`) as `js/internal/<module>.js`, and writes an `asset-index.json` of every name,
   because `node:sea` cannot list assets. It writes `dist/sea/sea-config.json` with them all as named assets. A new
   asset in `assets/` needs no configuration change. Packaging fails, naming the fix, when the SDK is not built;
   `cli:package` builds it first (`js-internal:build`).
3. Runs `node --build-sea` with the Node that runs the script (the release pinned in `.node-version`), with code
   cache on. Code cache ties the executable to that Node build and architecture, so cross builds would need it off.
4. On macOS, signs ad hoc (`codesign --sign - --force`) and lays out `dist/bdk.artifactbundle/` with an `info.json`
   (schema version 1.0, artifact `bdk`, type `executable`, variant for `arm64-apple-macosx`).

Signing with a Developer ID identity and notarization are part of the release procedure (T-PKG-06), not done here.

## Pages

`bdk build pages` (`src/core/steps/pages.ts`, helpers in `src/core/pages/`). The layout it writes is the one the
runtime serves: a bundled route's files at `Contents/Resources/<path>` (the route's `path` in `bdk.resolved.json`,
for example `Pages/Settings`), and the SDK at `Contents/Resources/BDK/js/internal/<module>.js`, which the runtime
serves to every bundled page as `/@bdk/internal/<module>.js`.

| Part | Behaviour |
|---|---|
| Pages directory | `[pages] directory` of `bdk.toml`, default `Pages`. Where `vite.config.{ts,mts,js,mjs}` is looked for. |
| Plain page | The directory is copied as it is, skipping dot-files and dot-directories. Links are followed (the resolver already refused links that leave the project). |
| Vite project | Vite is loaded from the pages directory's own `node_modules` (`module.createRequire`) and run in this process with the Node that runs the CLI, so no second Node is needed. It runs once per page that is a direct child of the pages directory, with `BDK_PAGE=<directory>` for the `@bdk/vite` preset ([packages/js/vite](../js/vite/README.md)), so each page's output is self-contained, with `index.html` and `assets/` at its top. A route that an override points elsewhere is copied as it is. Vite's output goes to a temporary directory; nothing is written into the project. |
| Checks | Every HTML file is scanned with `cspIssues` from `@bdk/vite`; every file is checked against the size limit. All files of all pages are checked before anything is written, so a failure leaves the bundle as it was. |
| Size limit | 16 MiB per file (`MAX_PAGE_FILE_BYTES` in `src/core/pages/files.ts`). The runtime's data source is to enforce the same limit. |
| SDK | The `*.js` files of `packages/js/internal/dist`, embedded in the executable (read from the repository when unpackaged). Missing: `cli.asset_missing`, exit 3. |

Diagnostics: `pages.resolved_missing`, `pages.resolved_invalid`, `pages.vite_missing`, `pages.vite_failed`,
`pages.csp` (with the file and line), `pages.too_large` and `cli.asset_missing`. Every `pages.` code exits 1;
`cli.asset_missing` exits 3. `--json` reports the pages written with their file
counts and the number of SDK modules.

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
| `bdk build pages` on a plain page project, packaged, with no `node` on `PATH` | Pass (`src/packaged.test.ts`): page and 23 SDK modules written; the SDK list comes from the embedded `asset-index.json` |
| Vite loaded from a project's `node_modules` and run in the packaged executable with no `node` on `PATH` | Pass with Vite 8.3.8 (`src/packaged.test.ts`). `import()` of a file is not available in a single executable (it reports "No such built-in module"), so Vite is loaded with `require`, which Node 26 allows for an ES module. A Vite that uses top-level `await` in its entry would not load this way; 8.3.8 does not. |
| `bdk build pages` unit tests (plain copy, Vite fixture with the workspace's Vite linked in, CSP, size limit, missing and invalid resolved file, stale files, SDK, Xcode defaults) | Pass (`src/core/steps/pages.test.ts`) |
| `bdk build pages --watch` | Not built (recorded gap) |
| `bdk build pages` inside an Xcode run-script phase | Not verified; the Xcode variables are simulated in tests |
| The embedded asset file system (`node:sea` virtual file system) | Not used; plain named assets only, per risk R31 |
