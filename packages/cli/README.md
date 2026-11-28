# @bdk/cli

The `bdk` command line tool, in TypeScript, shipped as one self-contained executable built with Node's own
`node --build-sea`. Design: [docs/planning/sdk/cli.md](../../docs/planning/sdk/cli.md) (sections 2 and 3).
Tasks: T-CLI-05 (the executable), T-CLI-04 (`bdk build`), T-CLI-01, T-CLI-02 and T-CLI-03 (`doctor`, `init`, `patch`).

## Commands

| Command | Status |
|---|---|
| `bdk init [<directory>] [flags]` | T-CLI-02, see "Init" below. Asks for each choice not given as a flag (only with a terminal on standard input; never with `--yes`, `--no-input` or without one) and writes the project from the embedded templates. |
| `bdk doctor [--bundle <App.app>]` | T-CLI-01, see "Doctor" below. Findings in the groups machine, project, compatibility and, with `--bundle`, bundle; exit 1 when one is an error. |
| `bdk patch status\|apply\|new <name>\|export\|check\|rebase <tag>` | T-CLI-03, see "Patch" below. Source mode only. |
| `bdk build config [--out <file>]` | Resolves `bdk.toml` against the embedded schema (`assets/bdk.schema.json`) and, on macOS inside Xcode, the `Info.plist` and entitlements Xcode names. Writes `bdk.resolved.json` next to `bdk.toml` (or to `--out`) only when there are no errors. In Xcode it also writes the build phases' `.xcfilelist` files (see "Xcode" below). The resolver itself is T-CFG-02 (`src/core/config/`). |
| `bdk build` | With no step, runs the steps that apply, in order: `config` always, `runtime` when there is a runtime to resolve (see "Runtime and embed"), `codegen` always (it does nothing without `Pages/` or `Automation/`), `pages`, `automation`, `devtools` and `extensions` when a bundle is named (`--bundle`, or an Xcode build), `embed` when there is also a runtime, `sign` in an Xcode build or with `--identity`. `--out` is a file for `config`, `automation`, `devtools` and `extensions` and a directory for `codegen`, so it is refused when `codegen` and another of them run. |
| `bdk build codegen [--out <dir>] [--commands <file>] [--resolved <file>]` | Writes the `@bdk` packages for editors, `tsc` and Vite (T-CLI-04, see "Codegen" below). |
| `bdk build pages [--bundle <App.app>] [--resolved <file>]` | Packages the bundled pages and the SDK into the application (T-CLI-04, see "Pages" below). Reads the `mode: "bundled"` routes of `bdk.resolved.json` (next to `bdk.toml`, or `--resolved`; missing means run `bdk build config` first) and writes each page to `<App.app>/Contents/Resources/<route path>`, replacing what was there. With `Pages/vite.config.*` it runs the project's own Vite once per page; otherwise it copies the page directory as it is. Refuses output the content security policy would block (`pages.csp`) and any file over 16 MiB (`pages.too_large`). Copies the `@bdk/internal` modules to `Contents/Resources/BDK/js/internal/`. In Xcode `--bundle` defaults to `CODESIGNING_FOLDER_PATH`, else `TARGET_BUILD_DIR/WRAPPER_NAME`. `--watch` is not built (T-CLI-06). |
| `bdk build sign --bundle <App.app> --identity <name\|-> [--team <id>] [--entitlements <file>]` | Re-signs the application and the runtime nested in it with the application's identity (T-PKG-03): rewrites each helper's `CFBundleIdentifier` to `<app id>.helper[.renderer]` (Alerts: `<app id>.framework.AlertNotificationService`), then signs inside-out (libraries, loose executables, helpers with the JIT entitlement for renderer and GPU, framework, application) and verifies with `codesign --verify --deep --strict`. Refuses an application entitlement on the deny list. Before signing, copies the runtime's `THIRD_PARTY_NOTICES.html` (from the framework's `Resources`) to the application's `Contents/Resources`, so the signature covers it; a runtime without one is the warning `sign.notices_missing` (T-REL-02). `BDK_SKIP_SIGNING=1` skips it. In Xcode the flags default to `CODESIGNING_FOLDER_PATH`, `EXPANDED_CODE_SIGN_IDENTITY`, `DEVELOPMENT_TEAM` and `CODE_SIGN_ENTITLEMENTS`. An ad hoc identity (`-`) gets no `--options`; see [sdk/cli.md](../../docs/planning/sdk/cli.md) section 5.1. |
| `bdk build automation --bundle <App.app> [--out <file>]` | Reads `bdk.resolved.json` (run `build config` first) and bundles each `automation.scripts.<name>` with Rolldown into one ES module with no imports at `<App.app>/Contents/Resources/BDK/Automation/<name>.js`, then adds `"bundle": "BDK/Automation/<name>.js"` and `"bundle_digest": "sha256:..."` to that script in the resolved file and recomputes `config_digest`. The automation host provides no modules, so `@bdk/automation`, `@bdk/automation/{client,commands,events}`, `@bdk/commands` and `@bdk/events` are inlined from the `@bdk/automation` package (the project's own copy, else `packages/js/automation` in the repository), and so is any other package that resolves inside the project. A Node built-in is `automation.import_not_allowed`, an import that does not resolve is `automation.import_unresolved`, a Rolldown error is `automation.bundle_failed`. Rolldown is loaded from the project's `node_modules` (the executable cannot carry its native binding), else from the CLI's own dependency; with neither the step fails with `automation.bundler_missing` (exit 3). Incremental: a script is rebuilt only when it, a module it imported or the output changed (`.bdk/automation-cache.json` in the project records the digests). With `[automation] enabled = false` or no scripts it does nothing. `--bundle` defaults to `CODESIGNING_FOLDER_PATH`. With `--watch` (T-AUT-07) it builds once, then watches `[automation] directory` (default `Automation`, at any depth) and rebuilds into the same bundle on each change, 100 ms after the last one, rewriting the digests as above. A build error is reported and watching goes on; Ctrl-C exits 0. Imports from outside that directory (a package in `node_modules`) are not watched. Adding or removing a script needs `bdk build config` first. `--watch` with any other step is a usage error. |
| `bdk build devtools --bundle <App.app> [--out <file>]` | Reads `devtools` from `bdk.resolved.json` (run `build config` first) and packages it in the application for the engine (javascript-api.md section 6). A path `frontend` is copied whole (replacing what was there) to `<App.app>/Contents/Resources/BDK/DevTools/Frontend/` and must hold `index.html` (`devtools.entry_missing`). With `frontend = "default"` and an `overlay` directory, `overlay.css` and `overlay.js` are copied to `.../BDK/DevTools/Overlay/`; neither present is `devtools.overlay_empty`, and any other file is not copied (one `devtools.overlay_files_ignored` warning lists them). An overlay beside a replacement or a disabled front end is `devtools.overlay_ignored` (warning). With neither, `Contents/Resources/BDK/DevTools` is removed and the step says nothing was packaged. A path outside the project is `config.path_escape`, a path that is not a directory is `devtools.not_directory`, and a value that looks like a URL is `devtools.url_not_allowed`: resources are packaged in the signed bundle, never loaded from a URL. Incremental: nothing is copied when the digests of the inputs and of the packaged tree match `.bdk/devtools-cache.json`. `--json` data is `{mode, packaged, unchanged, files}`. `--bundle` defaults to `CODESIGNING_FOLDER_PATH`. |
| `bdk build extensions --bundle <App.app> [--out <file>]` | Reads `extensions.bundled` from `bdk.resolved.json` (run `build config` first) and copies each extension's directory, as it is, to `<App.app>/Contents/Resources/<path>`, where the runtime loads it, so the application's signature covers it (extensions.md section 9, T-EXT-14). Replaces what was at that path; `.DS_Store` is not copied; a link inside the extension is copied as what it points at. Before anything is written it refuses a directory whose digest differs from the resolved file's (`extensions.resolved_stale`: it changed since `bdk build config`), a link that leaves the extension's directory or a path outside the project (`config.path_escape`), a link to nothing or back to a directory it is inside (`extensions.bad_link`), a path that is not a directory (`extensions.bundled_missing`) and an entry with no path or digest (`extensions.resolved_invalid`). A copy whose digest is not the resolved file's is removed and fails with `extensions.digest_mismatch` (exit 4). A copy of an extension the configuration no longer lists is removed only when `.bdk/extensions-cache.json` records that this step wrote it, into that bundle, with the digest it still has (and empty parent directories go with it); one that changed since is left, with an `extensions.stale_copy_kept` warning, and anything the step did not write is left. Prints each extension's declared `permissions`, `optional_permissions`, `host_permissions` and `optional_host_permissions`, so a reviewer sees what the product grants itself. Incremental: a copy whose digest already matches is not rewritten. `--json` data is `{copied, bundle, extensions: [{name, path, required, unchanged, permissions}], removed, kept}`. `--bundle` defaults to `CODESIGNING_FOLDER_PATH`. |

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
├── asset-sources.ts # where the SDK modules and packages come from (packages/js/*/dist) and the embedded asset index
├── core/            # platform-neutral: args, project discovery, diagnostics, steps, config seam, the `--watch` loop (watch.ts)
│   ├── doctor/      # bdk doctor: the groups' checks, the requirements, the embedded release facts
│   ├── init/        # bdk init: the choices, the prompts, the project generator
│   └── patch/       # bdk patch: source-mode settings and the subcommands, over tooling/src/chromium
└── platform/
    ├── platform.ts  # the interface the core sees
    ├── select.ts    # the one place a platform module is chosen
    ├── xcode/       # macOS: Xcode environment, Info.plist and entitlements through plutil; doctor.ts: toolchain and signature facts
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
   `import.meta.url` becomes a name beside the executable (`<bdk>.cjs`): the tooling modules the bundle includes
   run their command when `isMain(import.meta.url)` holds, which the executable's own path would make true. Tooling
   modules the CLI imports must not use top-level `await`, which a CommonJS bundle cannot hold.
2. Adds BDK's patch series (`chromium/patches/*`), the `bdk init` templates (`apps/templates/swift-browser/**` as
   `templates/swift-browser/`, `apps/templates/pages/**` as `templates/pages/`, without their README and
   `project.json`) and `bdk-release.json` (the pin and the ABI, from `chromium/pin.json` and `packages/core`), all
   listed in `src/asset-sources.ts`; unpackaged runs read the same files from the repository.
3. Lists every file under `assets/` (except `assets/README.md`), adds the built `@bdk/internal` modules
   (`packages/js/internal/dist/*.js`) as `js/internal/<module>.js`, and the `.js` and `.d.ts` files of the
   `@bdk/internal`, `@bdk/protocol`, `@bdk/automation` and `@bdk/vite` builds as `js/sdk/@bdk/<package>/<file>`, and writes an `asset-index.json` of every name,
   because `node:sea` cannot list assets. It writes `dist/sea/sea-config.json` with them all as named assets. A new
   asset in `assets/` needs no configuration change. Packaging fails, naming the fix, when the SDK is not built;
   `cli:package` builds it first (`js-internal`, `js-protocol`, `js-vite`, `js-codegen`).
4. Runs `node --build-sea` with the Node that runs the script (the release pinned in `.node-version`), with code
   cache on. Code cache ties the executable to that Node build and architecture, so cross builds would need it off.
5. On macOS, signs ad hoc (`codesign --sign - --force`) and lays out `dist/bdk.artifactbundle/` with an `info.json`
   (schema version 1.0, artifact `bdk`, type `executable`, variant for `arm64-apple-macosx`).

Signing with a Developer ID identity and notarization are part of the release procedure (T-PKG-06), not done here.

## Init

`bdk init` (`src/core/init/`): `choices.ts` lists the choices, `prompts.ts` collects the answers and builds the
replayed command, `generate.ts` turns the answers and the embedded templates into a file map (pure, so tests
generate every combination), and `init.ts` writes it and runs `git init` and the package manager through
`Context.exec`. The questions come from `Context.prompter` (`src/main.ts`: readline on standard error, only when
standard input is a terminal).

| Part | Behaviour |
|---|---|
| Choices | sdk/cli.md 4.2, in that order except that "Automation host" comes before "Package manager": the package manager and "Install packages now" are asked for Vite pages or automation, because scripts are bundled with the project's own Rolldown. `--search-engine` is not offered (`[search]`, S1, is not accepted). `--bdk-package <url\|path>` (not a question) points the Xcode project at another BDK package: a URL, or a checkout as a local package. |
| Frameworks | `vanilla`, `react`, `preact`. Vue, Svelte, Solid and Lit assign template strings to `innerHTML`, which the internal-page policy blocks (`trusted-types` with no policy, `chromium/bdk/engine/pages/bundled_page_source.cc`), so they are not offered. |
| Templates | `apps/templates/swift-browser` (the Xcode project, `__PRODUCT__` in paths, `{{name}}`, `{{product}}`, `{{bundleId}}`, `{{bdkCli}}` and the package placeholders in contents, escaped for Swift, property lists and pbxproj) and `apps/templates/pages` (see its README). `bdk.toml` is written by code. |
| Writes | Into an empty or missing directory; with `--force` into any, never overwriting a file (`init.file_kept`). `BuildPhases/*.sh` are made executable. `BDK_CLI` is the packaged executable that ran; unpackaged it is left empty with the note `init.cli_unset`. |
| After | `git init` unless `--no-git`; `<package manager> install` in `Pages/` (Vite) and in the project (automation) unless `--no-install` or `--offline`. A failure is a warning (`init.git_failed`, `init.install_failed`), and the project stays. Prints the command that makes the same project with no questions; `--json` gives the answers. |

Tests: `src/core/init/init.test.ts` generates a project for every combination of pages, framework, language, styling,
automation and runtime (56), with every starter page, with a prompter that records questions (none are asked),
then checks the files, `plutil -lint` on the pbxproj and `Info.plist`, the content security policy of each page's
HTML, the pinned versions, and that the generated `bdk.toml` resolves with each starter's grants.

## Doctor

`bdk doctor` (`src/core/doctor/`). Every finding is a diagnostic with a code, a message and a fix; `--json`
gives `{mode, toolchain, groups: {<group>: {errors, warnings, notes, codes}}}`. Requirements are in
`requirements.ts` (macOS 14, Xcode 26, Swift 6, arm64), each with its source.

| Group | Codes |
|---|---|
| machine | `doctor.host_unsupported`, `doctor.not_apple_silicon`, `doctor.macos_unknown`, `doctor.macos_too_old` (against the resolved runtime's `min_os` too), `doctor.xcode_missing`, `doctor.xcode_too_old`, `doctor.swift_missing`, `doctor.swift_too_old`, `doctor.node_missing` and `doctor.package_manager_missing` (with `Pages/vite.config.*` or automation scripts), and in source mode `doctor.bdk_home_invalid`, `doctor.bdk_home_not_writable`, `doctor.depot_tools_missing`, `doctor.checkout_missing`, `doctor.checkout_not_at_pin`, `doctor.ccache_missing` |
| project | Every diagnostic of the configuration resolver, with `Info.plist` and entitlements read from Xcode's settings or, outside Xcode, the template's `Resources/`; `doctor.page_capabilities` (a note per bundled page), `doctor.package_unresolved`, `doctor.bdk_version_mismatch`, `doctor.runtime_unpublished`, `doctor.runtime_missing`, `doctor.runtime_modified` (`.bdk/runtime.json` verified with the tooling's `verifyRuntimeDir`), `doctor.bdk_home_invalid` |
| compatibility | `doctor.abi_mismatch`, `doctor.runtime_other_chromium`, `doctor.vite_dependency_unsupported` (a dependency in the pages' `package.json` outside the range `@bdk/vite` declares in `SUPPORTED_DEPENDENCIES`), and in source mode `doctor.patch_invalid`, `doctor.patch_unlisted`, `doctor.application_patches`, `doctor.source_abi_differs` (the resolved runtime's ABI hash against the published prebuilt runtime's, `runtime.abi_hash` in `bdk-release.json`) or, with no prebuilt runtime published, `doctor.prebuilt_abi_unknown` (a note) |
| bundle | `doctor.bundle_missing` (exit 2), `doctor.bundle_config_missing`, `doctor.bundle_config_stale` (the scripts' `bundle` and `bundle_digest`, which `bdk build automation` adds, are taken out first), `doctor.page_missing`, `doctor.page_modified` (a page `bdk build pages` copies, file by file against its source, when the route's digest shows the source is the one built), `doctor.page_unverified` (a note: a page Vite builds, whose output has no recorded digest), `doctor.script_not_bundled`, `doctor.script_missing`, `doctor.script_modified` (against `bundle_digest`), `doctor.bundled_extension_missing` and `doctor.bundled_extension_modified` (each `extensions.bundled` entry of the application's resolved file against its copy in `Contents/Resources`), `doctor.runtime_not_embedded` (a note), `doctor.framework_modified` (`Contents/Frameworks/<entry>` against the resolved runtime's manifest; executables, `.dylib` files and `Info.plist` files by kind and executable bit only, `_CodeSignature` ignored, because signing rewrites them), `doctor.signature_invalid`, `doctor.ad_hoc`, `doctor.helper_team_mismatch`, `doctor.not_notarized`, `doctor.not_developer_id` (a team signature whose certificate is not `Developer ID Application:`), and on a Developer ID signature `doctor.gatekeeper_rejected` or `doctor.gatekeeper_accepted` (a note), from `spctl --assess` |

The project group is the hook for diagnostics added to the resolver (`src/core/config/`): whatever `bdk build
config` reports, `bdk doctor` lists, with its own severity, so `web_apps.push_unconfigured` and `services.push_google`
(T-PWA-02) need no change here. The platform supplies the facts through `Platform.toolchain` and
`Platform.inspectSignature` (`src/platform/xcode/doctor.ts`: `sw_vers`, `sysctl`, `xcodebuild -version`, `xcrun swift
--version`, `codesign -dvv`, `xcrun stapler`, `spctl --assess --type execute -vv`). Tests: one fixture and message
per code in `src/core/doctor/doctor.test.ts`, with a fake platform; the parsers in `src/platform/xcode/doctor.test.ts`
with a fake runner. The pages, scripts and runtime checks are in `contents.ts`, the Vite pins in `dependencies.ts`.
On 2026-11-25 the signature checks were run for real on a third-party application from `/Applications` that is signed
with Developer ID and notarized but not stapled: `doctor.not_notarized` and `doctor.gatekeeper_accepted` ("Notarized
Developer ID"). Not run: on a BDK application signed with Developer ID and notarized (no signing identity), and on a
Developer ID signature that Gatekeeper rejects. Not built: `--fix`, whether the pin is behind the newest BDK release
(needs a release feed), feature overrides, and a digest of Vite's output for each page (`bdk build pages` records
none).

## Patch

`bdk patch` (`src/core/patch/`) works on the managed checkout `$BDK_HOME/chromium/managed/src` with the tooling's
own code: the series format, `applySeries` with an application series after BDK's (`appPatchesDir`), the
deterministic `exportSeries`, `checkPatchSeries` and the checkout lock. BDK's series and the pin come from the
embedded assets. The application's series is `[chromium] patches` (a directory with `series`, default
`Chromium/patches`, or a list of files, copied to a temporary series), and is named `app/<file>` in reports.

| Command | Behaviour |
|---|---|
| `status` | BDK's and the application's patches with the files each touches, and the checkout's state. No lock. |
| `apply` | Resets to the pin and applies both series, or does nothing when they are applied; refuses uncommitted work or commits that are not in the series (`patch.checkout_unsafe`). A conflict is `patch.conflict`, naming the patch, its file in the project and every conflicting file. |
| `new <name>` | As `apply`, then says where to edit and how to commit. |
| `export` | The commits after BDK's series (`patch.base_unknown` when the checkout does not start with it) into the patch directory; refuses the list form (`patch.list_not_exportable`). |
| `check`, `rebase <tag>` | The sparse check against the pin (its commit must match) or another tag, with no build. `BDK_PATCH_CHECK_REMOTE` names the remote. `--offline` refuses (`patch.offline`, exit 3). |

`apply`, `new` and `export` run again as a child holding the checkout lock (`Context.underCheckoutLock`, with
`BDK_CHECKOUT_LOCKED=1`); a held lock is `patch.checkout_locked`, exit 3, naming the holder. Not in source mode:
`patch.not_source_mode`; no checkout: `patch.checkout_missing`, exit 3. A runtime built with the application's
patches records them in its manifest (`application.patches` and `patches_digest`, `tooling/src/runtime/`);
`bdk build runtime --allow-source-build` builds one (see "Runtime and embed"). Tests: `src/core/patch/patch.test.ts` on the tooling's fake
upstream (`tooling/src/chromium/test-support.ts`), never a real checkout.

## Runtime and embed

`bdk build runtime` (`src/core/steps/runtime.ts`) resolves the runtime the project runs on into the store under
`BDK_HOME` and records it in `.bdk/runtime.json` (`source_key`, `manifest_sha256`; `src/core/runtime/project-runtime.ts`).
`bdk build embed` (`src/core/steps/embed.ts`) copies it into the application.

| Mode | What `runtime` does |
|---|---|
| Prebuilt (default) | The runtime of the embedded release facts (`bdk-release.json`, `runtime`). In the store: verified against its manifest and the release's manifest digest. Not in the store: downloaded with the tooling's `fetchRuntime`, which checks the archive's SHA-256, the manifest and the ABI hash before installing anything. `--offline` refuses to download (`runtime.offline`, exit 3). A release with no runtime: `runtime.unpublished`. |
| Source (`runtime.mode = "source"`, or `BDK_RUNTIME_MODE=source`) | The source key of the BDK checkout named by `BDK_SOURCE_ROOT` plus the application's patches (`[chromium] patches`). In the store: verified and used. Not in the store: only with `--allow-source-build` or `BDK_RUNTIME_MODE=source` (else `runtime.source_build_not_allowed`), it runs `chromium:build` and `chromium:package` in that checkout with `--app-patches`, which take the checkout lock and apply both series. The checkout must have been synced (`pnpm chromium:sync`). |

`embed` copies each entry of the runtime's `Chromium.app/Contents/Frameworks` to the bundle's `Contents/Frameworks`,
symlinks as they are, makes the copy writable (the store is read-only) and checks it against the manifest. A copy
that `.bdk/embedded.json` records as this runtime's (bundle path → manifest digest) and that still matches, allowing
for what signing rewrites, is left alone, so a signed bundle is not unsigned by the next build. The record is needed
because that comparison ignores the content of executables and libraries, where two runtimes' engines differ. Other frameworks in the bundle are not touched. With no step, `bdk build` runs `runtime` when there is a
runtime to resolve, and `embed` when there is also a bundle; a release with no runtime builds for the stub.

Tests: `src/core/steps/runtime.test.ts` and `embed.test.ts`, on fixture runtimes in a temporary `BDK_HOME`; the
download test unpacks a real archive made with the tooling's `createArchive`. The source build is a fake `exec`.

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

## Codegen

`bdk build codegen` (`src/core/steps/codegen.ts`, helpers in `src/core/codegen/`). Design:
[javascript-api.md section 7](../../docs/planning/sdk/javascript-api.md). It never runs a package manager or Swift.

| Part | Behaviour |
|---|---|
| Applies when | The pages directory (`[pages] directory`, default `Pages`) or the automation directory (`[automation] directory`, default `Automation`) exists. With neither it writes nothing (javascript-api.md section 7, "No JavaScript"). |
| Inputs | `bdk.resolved.json` (`--resolved`, else next to `bdk.toml`; missing is `codegen.resolved_missing`) and the application's declarations, `bdk.commands.json`, written by `BDKDeclarations.write(to:)`. |
| Declarations file | `--commands <file>`; in Xcode `$BDK_COMMANDS_FILE` (relative to `SRCROOT`), else `$DERIVED_FILE_DIR/bdk.commands.json`; otherwise `<project>/.bdk/bdk.commands.json`. A missing file is not an error: it generates with no `app.` declarations and reports a `note`, `codegen.no_declarations`. |
| Output | `--out <dir>`; in Xcode `$DERIVED_FILE_DIR/bdk/js`; otherwise `<project>/.bdk/js`. It holds `@bdk/internal/` (the built `.js` and `.d.ts`, `package.json`, and the generated `app.d.ts`), `@bdk/protocol/` (its `.d.ts`, which the internal ones import for types), `@bdk/vite/` (the preset, so `import { bdk } from "@bdk/vite"` resolves in `Pages/vite.config.ts`), `@bdk/automation/` (the script API, with `app.d.ts` merging the application's commands and events into it) with `@bdk/commands/` and `@bdk/events/` (re-exports of its `commands` and `events` entries, written when the SDK has `@bdk/automation`), `@bdk/env.d.ts`, `sdk.digest`, and `node_modules/@bdk`, a link to `@bdk` so that the packages find each other once a tool follows a link to its real path. Each `package.json` has `exports` for every module that has a `.d.ts`. |
| Incremental | `app.d.ts` and `env.d.ts` record the digest of the declarations and of the resolved file; `sdk.digest` records the digest of the embedded packages. When all three match the inputs nothing is written. Otherwise `@bdk/` is rewritten whole and `sdk.digest` goes last. |
| Links | `<pages directory>/node_modules/@bdk` and `<automation directory>/node_modules/@bdk` (the ones whose directory exists) point at `<out>/@bdk`. A link that points elsewhere is replaced. A real file or directory is never replaced: `codegen.link_blocked`, a warning. A `node_modules` the step creates gets a `.gitignore` with `*`. |
| Source | The packages are embedded in the executable as `js/sdk/@bdk/<package>/<file>` (`src/asset-sources.ts`; read from `packages/js/<package>/dist` when unpackaged). |

Diagnostics: `codegen.resolved_missing` (exit 1), `codegen.resolved_invalid` and `codegen.declarations_invalid` (exit 1,
one per issue of the generator, `$.path: message`, at the file that holds it), `codegen.no_declarations` (note),
`codegen.link_blocked` (warning) and `cli.asset_missing` (exit 3). A "note" is a third severity, printed as `note:`.
Not built: the Swift declaration extractor (the CLI reads the file it writes), and a check that a Vite page was built
against current declarations. The types of `@bdk/vite` import `vite`, which does not resolve from the generated
directory: set `skipLibCheck` (usual) or accept `any` for the preset's options.

## Xcode

cli.md section 8. The CLI knows Xcode only through `src/platform/xcode/`, which supplies `buildLocations` (from
`DERIVED_FILE_DIR`: the declarations file, the codegen output and the file-list directory) and `writeFileLists`.
`bdk build config` computes the lists (`src/core/filelists.ts`) and, only when the platform has locations, has the
platform write four files, `$DERIVED_FILE_DIR/bdk/{runtime,bundle}.{input,output}.xcfilelist`, whose contents
are described in [apps/templates/swift-browser/README.md](../../apps/templates/swift-browser/README.md), with the build
phase scripts. Not verified in Xcode itself.

## Signing

The plan (`src/core/signing/`) is pure and unit-tested against a fake bundle tree with a fake `codesign` and `plutil` runner (`src/platform/xcode/tools.ts` is the only place either is started).

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
| `bdk build pages --watch` | Not built (recorded gap, T-CLI-06) |
| `bdk build automation --watch` unit tests (rebuild into the same bundle, digests, build error, stop, usage errors), with a fake watcher | Pass (`src/core/steps/automation.test.ts`, `src/core/watch.test.ts`) |
| `bdk build automation --watch` with the real `fs.watch` and a real Ctrl-C | Not verified; the watcher and the signal are injected in tests |
| `bdk build pages` inside an Xcode run-script phase | Not verified; the Xcode variables are simulated in tests |
| The embedded asset file system (`node:sea` virtual file system) | Not used; plain named assets only, per risk R31 |
| `bdk build config codegen` on a project with `Pages/`, packaged, with no `node` on `PATH` | Pass (`src/packaged.test.ts`): the packages and `app.d.ts` written, `Pages/node_modules/@bdk` linked |
| `tsc` (module resolution `bundler`) on a file importing `@bdk/internal/page`, `@bdk/internal/settings` and `@bdk/vite` through the link | Pass for `@bdk/internal` and `@bdk/protocol`; `@bdk/vite`'s own `import "vite"` does not resolve unless `skipLibCheck` is set (checked once by hand on `apps/examples/minimal-browser`) |
| `bdk build codegen`, the file lists and the Xcode defaults in an Xcode run-script phase, with user script sandboxing on or off | Not verified; the Xcode variables are simulated in tests (`src/core/steps/codegen.test.ts`, `src/platform/xcode/build.test.ts`) |
| (2026-11-25, Xcode 27.2) A project from the packaged `bdk init` (plain starter pages, automation) built and tested with `xcodebuild` against this repository's package and the stub runtime | Pass (`apps/templates/swift-browser/scripts/build-generated.sh`, the `template-swift-browser:build` target): the run-script phases ran `bdk build config codegen`, `pages automation` and skipped `sign` (no runtime embedded); the bundle carries `bdk.resolved.json`, the pages, `BDK/js/internal` and `BDK/Automation/hello.js`; `codesign --verify --deep --strict` passes; the unit test passes. Launched by hand, the application stayed up on the stub runtime with no error. The Xcode GUI's Run was not used. |
| (2026-11-25) Vite page templates installed from the registry and built by the packaged `bdk build pages`: React + TypeScript + Tailwind, Preact + TypeScript + CSS, vanilla + JavaScript | Pass, by hand: each page built and passed the policy check; `tsc --noEmit` passed in the React and Preact projects. Each framework's output at run time under Trusted Types in Chromium is not verified. |
| `bdk build automation` in the packaged executable | Pass (`src/packaged.test.ts`) after loading Rolldown with `require`: `import()` of a file is not available in a single executable, which made the packaged step fail with "No such built-in module" before. |
