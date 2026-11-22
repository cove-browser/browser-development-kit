# BDK CLI and build integration

Status: proposal, 2026-11-15. Based on the spec's sections 5 and 10 and [../repository-and-tooling.md](../repository-and-tooling.md). Command, flag and file names are placeholders except `bdk init`, `bdk patch` and `bdk doctor`, which are the spec's. Nothing here has been built. Status labels are in [README.md](README.md) section 2. Tasks: T-CLI-01 to T-CLI-05, T-CFG-01, T-CFG-02, T-PKG-03.

This document is about the `bdk` tool that a browser developer uses. The `pnpm` scripts for people working on BDK itself are in the tooling document, section 4.

## 1. Commands

| Command | Who runs it | Purpose | Status |
|---|---|---|---|
| `bdk init` | A developer, once | Create an application project | Plan, M9, T-CLI-02 |
| `bdk doctor` | A developer; CI | Check the machine, the project and the built application | Plan, M9, T-CLI-01 |
| `bdk build` | The Xcode build; occasionally a developer | Resolve configuration, generate code, build pages and scripts, resolve or build the runtime, sign | Added (S8); the work is T-CLI-04 |
| `bdk patch` | A developer in source mode | Manage the application's Chromium patches | Plan, M9, T-CLI-03 |

The spec proposes three commands and asks for the CLI to stay lean. `bdk build` is a fourth because the Xcode build needs one stable entry point for work the spec already assigns to the build. It adds no new capability. There is no `bdk run`, `bdk test`, `bdk publish`, `bdk update` or `bdk config`: Xcode runs and tests, the application ships its own updates, and BDK never updates itself.

## 2. Installation, language and versions

| Use | Which `bdk` |
|---|---|
| `bdk init`, before a project exists | A standalone binary from the BDK release, or a package manager formula that installs it |
| Everything inside a project | The binary that ships with the BDK package the project resolved, so the CLI version always equals the SDK and runtime version. On macOS it is an executable in an artifact bundle of the Swift package (verified with SwiftPM on 2026-11-18, not yet with Xcode; section 2.3). |

A standalone `bdk` run inside a project re-executes the project's own copy. If it cannot find one, it says so and stops; it does not guess.

### 2.1 Language

The CLI is written in TypeScript and shipped as one self-contained executable per platform. This is the product owner's direction from the 2026-11-16 review. It replaces the earlier recommendation of Swift.

| Reason | Detail |
|---|---|
| Other platforms | Windows and Linux hosts are planned after v1 (roadmap section 4). A Swift CLI would tie the tool to the Swift toolchain on those platforms, or need a second CLI. |
| Nothing needs Swift | Every macOS-specific step runs an Apple tool as a subprocess or edits a text file (section 2.2). |
| Shared code | The code generator (`packages/js/codegen`), the Vite preset and the repository tooling are already TypeScript. The CLI imports them. A Swift CLI would have had to start a JavaScript process to generate declarations. |
| No Node for the developer | The executable carries its own JavaScript runtime. A developer who writes no JavaScript installs no Node, so the spec's promise holds. The earlier objection to TypeScript was only that a CLI run by Node would make Node a requirement of every Xcode build. |

### 2.2 Platform-neutral core and platform modules

The CLI has a platform-neutral core and one module per host platform. The core never imports a platform module directly; it calls the module for the current platform through one interface. A lint rule enforces that.

```text
packages/cli/src/
├── core/            # configuration, runtime store, code generation, pages, automation, patches, prompts
└── platform/
    ├── xcode/       # macOS: everything in the right-hand column below
    └── …            # a Windows module later, behind the same interface
```

| Work | Platform-neutral core | The `xcode` module, on macOS |
|---|---|---|
| `init` | Prompts, flags, `bdk.toml`, page templates | The Xcode project template: files copied with names substituted. No project-file library is needed. |
| `build config` | Parse and validate `bdk.toml`; write the resolved file | Read `Info.plist` and the entitlements. They are property lists, read with `plutil` or a parser. |
| `build runtime` | Download, verify the digest, store, lock | Which artifact to fetch; the lock primitive |
| `build codegen` | The generator | Ask the Swift toolchain to build and run the declaration extractor, then read its JSON ([javascript-api.md](javascript-api.md) section 7) |
| `build pages`, `build automation` | All of it | None |
| `build embed` | None | Copy the framework into the bundle with its symlinks |
| `build sign` | None | Rewrite helper identifiers; run `codesign` |
| `doctor` | Project and compatibility checks | Xcode and Swift versions, signatures, notarization: each an Apple tool run as a subprocess |
| `patch` | All of it: git and depot_tools | None |
| Diagnostics | Codes, messages, JSON output | The `file:line: error:` form Xcode shows inline |

Nothing in the list needs Swift or links an Apple framework. The one piece of Swift in the build, the declaration extractor, is compiled from the application's own module by the Swift toolchain; the CLI only starts it.

### 2.3 Producing the executable

Facts are from each tool's documentation, read on 2026-11-16, before anything was built. The first build's results are at the end of this section.

The JavaScript engine inside the CLI does not need to match Chromium's. The CLI is a separate build-time process that reads files and starts other tools. It never loads the runtime, and shares no code, bytecode or snapshot with it. Pages run in Chromium whatever built them, and the automation host's engine is a separate choice (T-AUT-01). The reasons to prefer one option below are about tooling, not about V8.

| Option | Targets | Cross-compiles | Notes |
|---|---|---|---|
| Node single executable application, `node --build-sea` | Wherever Node runs. Node's CI tests it on Windows, macOS and Linux other than Alpine. | Yes: the `executable` field names the Node binary of the target platform. Code cache and start-up snapshot must then be off. | Built into Node 26; no package needed. Marked "Stability 1.1, Active development". Takes one bundled script, CommonJS or ES module. Embeds named assets; a read-only file system over them exists from 26.9 and is marked "Early development". The script can load modules from disk with `module.createRequire()`. It cannot install packages. Signed with `codesign` on macOS. |
| Bun, `bun build --compile` | macOS arm64 and x64; Windows x64 and arm64; Linux x64 and arm64, glibc and musl | Yes, from any host. Windows icon and file metadata can be set only on a Windows host. | `--asset` embeds a directory tree. `--bytecode` moves parsing to build time. With `BUN_BE_BUN=1` the executable acts as the full `bun` tool, so it can install packages. Runs on JavaScriptCore, not V8. On macOS it must be signed with just-in-time entitlements. |
| Deno, `deno compile` | macOS arm64 and x64; Windows x64; Linux x64 and arm64 | Yes, from any host | Supports npm packages. Signed ad hoc on macOS by default. No Windows arm64 target in the documentation read. |

Packages that wrap the Node route, none of them needed with `--build-sea`:

| Package | What it is |
|---|---|
| `@yao-pkg/pkg` | The maintained fork of Vercel's `pkg`. Has a mode built on single executable applications (Node 22 or later) and the older patched-binary mode. Downloads prebuilt base binaries. |
| `postject` | The injection tool the earlier, manual flow needed. `--build-sea` replaces it. |
| `boxednode` | Builds Node from source with the script inside. Slow builds. Not evaluated. |
| `nexe`, `seabox`, `node2exe` | Other wrappers. Not evaluated. |

Decided by the product owner on 2026-11-16: Node's own `node --build-sea`, with no wrapper package. Reasons:

| Reason | Detail |
|---|---|
| One runtime | The repository already builds and tests on Node 26; the experiments in [../feasibility.md](../feasibility.md) ran on Node 26.9. The shipped CLI then runs on the runtime it was tested on. |
| The tools it drives | Vite and Rolldown are developed and tested against Node. |
| Fewer dependencies | The feature is part of Node; nothing third-party sits between the source and the release binary. |

What it costs, compared with Bun: the executable cannot install packages, so a project with Vite pages needs Node and a package manager installed. A developer writing React pages has both. A project with no JavaScript still needs neither.

Build steps: bundle `packages/cli` into one file (Rolldown, already in the plan), then run `node --build-sea` once per target on that target's CI runner, so code cache can stay on. The repository pins one Node 26 release in `.node-version` (read by fnm) and in `engines`; that release is also the runtime inside `bdk`. This is a requirement for building BDK, not for using it. v1 ships one executable, for macOS on Apple Silicon (decision D6); other targets are added with their platforms.

T-CLI-05 builds it in M2 and must show the following; the Vite check waits for pages, in T-CLI-04. Results of the first build, run on 2026-11-18, are in the table and below it.

| Test | Why |
|---|---|
| The executable works from a Swift package artifact bundle and from an Xcode run-script phase | That is how every build finds it (section 8) |
| Start-up time, measured | An Xcode build runs it three times |
| `bdk build pages` loads the project's own Vite from `node_modules` with the embedded Node | The page build must not need a second Node of a matching version |
| Templates and schemas are read from embedded assets on every target | The asset file system is new; plain named assets are the fallback |
| The binary is signed and notarized in the BDK release and passes Gatekeeper | A standalone download must open without a warning |

Results of the first build (`packages/cli`, `pnpm cli:package`), run on 2026-11-18 on macOS 27, Apple Silicon, Node 26.9.0, Rolldown 1.2.18:

| Test | Result |
|---|---|
| No Node on the machine | Passed. With every directory that holds `node` removed from `PATH`, `bdk --version`, `--help`, and `build config` ran and returned their exit codes (0, 0, 1 outside a project; 2 for an unknown command). The `cli-smoke` job in `pr.yml` repeats this on both runners. |
| Swift package artifact bundle | Passed for SwiftPM. `dist/bdk.artifactbundle/` (`info.json` schema version 1.0, artifact `bdk`, type `executable`, one variant for `arm64-apple-macosx`) was used as `.binaryTarget(path:)` in a throwaway package. A command plugin ran `bdk --version` and got `0.0.0`; a build-tool plugin ran it during `swift build` inside SwiftPM's sandbox. Not verified: an Xcode run-script phase, and Xcode's own build of the package. `xcodebuild` on the machine reported that CoreSimulator is out of date and found no scheme in the throwaway package, so Xcode's environment is only simulated (`XCODE_VERSION_ACTUAL`) in unit tests. |
| Start-up time | `bdk --version`: median 36 to 39 ms over 50 runs (minimum 34 ms), against 33 ms for `node -e 0` on the same machine. `--help` and `build config` on a one-line project: 41 to 49 ms median. Three runs per Xcode build are about 0.15 s. The executable is 145 MB. Code cache was on. |
| Embedded assets | Passed with plain named assets: every file under `packages/cli/assets/` is registered automatically, and `bdk build config` read the schema asset inside the executable (checked with a throwaway `bdk.schema.json`, removed again; the real schema is T-CFG-01). A missing asset gives `cli.asset_missing`, exit 3. The asset file system was not used. |
| Signing | Ad hoc signing (`codesign --sign - --force`) works and the binary runs; Developer ID signing, notarization and Gatekeeper are not tested. |
| Import rule | Passed: Biome `noRestrictedImports` for `src/core/**` rejects platform imports; a test proves it on fixtures. |

If one of these fails on a point that Bun passes, Bun is the fallback (risk R31). Rules that keep that open:

| Rule | Detail |
|---|---|
| Standard APIs | CLI code uses `node:` modules and web-standard APIs. Anything specific to the packaging (embedded assets) sits behind one adapter file. |
| Also runs unpackaged | The same code runs under plain Node from the repository, for development and tests. |
| One binary, no install step | The executable never downloads a runtime or packages for itself. Its templates and schemas are embedded. |

## 3. Global behaviour

| Topic | Rule |
|---|---|
| Project discovery | The nearest ancestor directory that contains `bdk.toml`, or `--project <dir>` |
| Output | Human-readable on a terminal. `--json` prints one JSON document on standard output, for CI and editors. Diagnostics go to standard error. |
| Diagnostics | Each has a stable code, a file and line where one exists, and a fix. In an Xcode build they are printed in the `file:line: error: message` form so Xcode shows them inline. |
| Colour and prompts | Off when not on a terminal. Only `bdk init` asks questions, and every question has a flag that answers it (section 4.2). No command prompts in CI: a choice with a default takes it, and a choice with no default fails and names the flag. |
| Network | Only `bdk init` (template and package resolution) and `bdk build runtime` (runtime download) use it to download, and both verify what they download against a digest. `bdk build sign` with a real identity also reaches Apple's timestamp service through `codesign --timestamp`. `--offline` makes all three fail instead (signing ad hoc needs no network and still works). |
| Telemetry | None |
| Disk | No command reports disk usage or manages storage (tooling document, section 7) |

Exit codes:

| Code | Meaning |
|---|---|
| 0 | Success |
| 1 | The check or build failed for a reason in the project: invalid configuration, a failing page build, a doctor finding at error level |
| 2 | Usage error |
| 3 | The environment is unusable: missing toolchain, no runtime for this source key, lock held by another build |
| 4 | An integrity failure: a digest or signature did not match |

## 4. `bdk init`

```text
bdk init [<directory>] [--name <display name>] [--bundle-id <identifier>] [--scheme <scheme>]
                       [--template swift-browser]
                       [--pages none|plain|vite] [--framework vanilla|react|vue|svelte|solid|preact|lit]
                       [--language ts|js] [--styling css|tailwind] [--starter-pages <route>,…]
                       [--package-manager pnpm|npm|yarn|bun] [--no-install]
                       [--search-engine none|<id>] [--automation] [--runtime prebuilt|source] [--no-git]
                       [--yes] [--no-input] [--force]
```

Run with no flags on a terminal, `bdk init` asks for each choice in turn. Every choice also has a flag, so the same project can be created with no questions.

### 4.1 What it creates

```text
<directory>/
├── <Name>.xcodeproj
├── bdk.toml                   # [browser] name and an explicit internal_scheme; [storage] set explicitly
├── Package.resolved           # BDK pinned to the version of the CLI that ran
├── Sources/
│   ├── AppDelegate.swift      # the example in swift-api.md section 18
│   └── BrowserWindow.swift    # one window, one page, an address field
├── Pages/                     # unless --pages none: the chosen starter pages, with no capabilities beyond what each needs
├── Automation/                # with --automation
├── Resources/
│   ├── Info.plist             # NSPrincipalClass, usage strings, URL and document types
│   ├── Assets.xcassets        # an empty AppIcon set; the application's icon goes here
│   └── App.entitlements
├── Tests/                     # one unit test against the stub runtime
└── .gitignore                 # includes node_modules and .bdk
```

### 4.2 Choices, prompts and flags

Asked in this order. A question is skipped when its flag was given or when an earlier answer makes it irrelevant.

| Prompt | Flag | Default | Asked when |
|---|---|---|---|
| Display name | `--name` | None. Required. | Always |
| Directory | `<directory>` | The name, without spaces | Always |
| Bundle identifier | `--bundle-id` | None. Required. | Always |
| Internal scheme | `--scheme` | Derived from the name | Always |
| Internal pages | `--pages` | `none` | Always. `none`: Chromium's built-in pages only. `plain`: HTML, CSS and ES modules with no build step. `vite`: built with Vite and BDK's preset. |
| Framework | `--framework` | `vanilla` | `--pages vite` |
| Language | `--language` | `ts` | `--pages vite`. Plain pages are JavaScript with generated type declarations. |
| Styling | `--styling` | `css` | `--pages vite` |
| Starter pages | `--starter-pages` | `new-tab` | Pages are not `none`. A multiple choice of `new-tab`, `settings`, `history`, `bookmarks`, `downloads`. Each one chosen replaces the built-in page for that route. |
| Package manager | `--package-manager` | The one that invoked `bdk`, else the first found of pnpm, npm, yarn, bun | `--pages vite` |
| Install packages now | `--no-install` | Install | `--pages vite` |
| Default search engine | `--search-engine` | `none` | Always. An identifier from the short list the template ships, written as a `[search]` table the developer can edit. BDK compiles no engine in. Depends on addition S1. |
| Automation host | `--automation` | Off | Always |
| Runtime | `--runtime` | `prebuilt` | Always. `source` only writes `runtime.mode`; it never starts a Chromium build. |
| Git repository | `--no-git` | Create one | Always |

| Rule | Detail |
|---|---|
| Flags win | A choice given as a flag is not asked. An invalid value is a usage error (exit code 2) that lists the valid ones. |
| `--yes` | Accept the default for every choice not given as a flag. Choices with no default must then be flags. |
| Not on a terminal, or `--no-input` | The same as `--yes`, and nothing is ever asked. A missing required choice fails and names its flag. |
| Dependent flags | A flag whose question would not have been asked is a usage error, for example `--framework react` with `--pages plain`. |
| Replay | At the end, `bdk init` prints the full command that reproduces the project without prompts. `--json` prints the answers as one document. |
| Nothing hidden | Every answer ends up in a file the developer can read and change: `bdk.toml`, `Pages/package.json`, `Pages/vite.config.ts`. `bdk init` keeps no state of its own. |

### 4.3 Page templates

| Topic | Detail |
|---|---|
| What a template is | A directory of files for one combination of framework, language and styling, kept in `apps/templates/pages/` and shipped in the BDK release. `bdk init` copies it; it runs no third-party generator such as `create-vite`. |
| Versions | Each template pins its framework and Vite versions. The same BDK version always produces the same files. |
| Frameworks | Only frameworks whose output BDK tests against the internal-page content security policy are offered. The list above is the proposal. Each one needs a passing fixture before it ships, because the policy forbids inline script and `eval` and enforces Trusted Types ([javascript-api.md](javascript-api.md) 2.2). Unverified for every framework. |
| Other frameworks | A developer can use any framework by choosing `vanilla` and adding it. BDK's Vite preset then fails the build on output the policy would block. |
| Project shape | One Vite project in `Pages/` with one entry per page directory. Each page is its own origin, so a React application is one per page, not one application routed across pages. Pages share components through `Pages/shared/` at build time and share state only through `@bdk/internal/*`. |
| Routing inside a page | Works. Sub-paths of a page fall back to its `index.html` ([javascript-api.md](javascript-api.md) section 2), so a client-side router such as React Router or TanStack Router can be added as a library. |
| Frameworks that need a server | Not offered. An internal page is static files served from the application bundle. There is no server, so no server rendering, server components or API routes. Next.js, React Router in framework mode and TanStack Start are usable only if they produce a client-only static build with no inline script. None is a template and none is tested. |
| No dev server | Pages are never loaded from `http://localhost`, so there is no hot module replacement. `bdk build pages --watch` rebuilds and the running application reloads the page ([javascript-api.md](javascript-api.md) section 8). |
| Starter pages | Each starter page is a small working page that uses `@bdk/internal/*` and comes with the `[pages.capabilities.<route>]` table it needs, so the grants are visible in `bdk.toml` from the first commit. |
| After `init` | Framework and page configuration belong to the developer. Later BDK versions do not rewrite them. `bdk doctor` reports a template-pinned dependency that the current Vite preset no longer supports. |

### 4.4 Rules

| Rule | Detail |
|---|---|
| The result runs | `bdk init`, open in Xcode, Run: a window with a working page. That is the acceptance test in the roadmap, M9. It holds for every combination of choices, which makes the template matrix a CI fixture. |
| Scheme | Derived from the name and then written explicitly, so a later rename does not change it |
| Storage identifiers | Written explicitly for the same reason |
| Defaults | Every browser-owned surface in default mode; extensions on; no search engine; automation off |
| Non-empty directory | Refused unless `--force`, which never overwrites an existing file |

## 5. `bdk build`

Added (S8). Called by Xcode. Each step can also be run alone. Built so far: `config`, `codegen` and `pages` (M4, T-CLI-04), `automation` and `devtools` (M7) and `sign` (M2); `runtime` and `embed` are not built. With no step, `pages`, `automation` and `devtools` run only with a bundle (`--bundle`, or an Xcode build). `--out` is the resolved file for `config`, `automation` and `devtools` and a directory for `codegen`, so one run cannot give it to `codegen` and another of them. `--watch` is not built (T-CLI-06, M5). Run from the packaged executable with no Node on the path; not run from an Xcode build.

`bdk build` is not a second way to build or run the application. The developer builds and runs in Xcode, and Xcode's build phases call `bdk build` for the BDK-specific steps (section 8). `bdk build runtime` is one of seven steps of this one command, not a separate command. A developer types `bdk build` by hand in two cases only: `bdk build pages --watch` while editing internal pages, and `bdk build runtime --allow-source-build` in source mode, because a Chromium build must never start as a side effect of pressing Run.

```text
bdk build [<step>…] [--configuration Debug|Release] [--derived <dir>] [--bundle <App.app>] [--watch]
```

With no step, runs the steps that apply, in this order:

| Step | Reads | Writes | Fails when |
|---|---|---|---|
| `config` | `bdk.toml`, `Info.plist`, entitlements, `Pages/` and `Automation/` directory names | `bdk.resolved.json` | Any validation error in [configuration.md](configuration.md) section 9 |
| `runtime` | The resolved BDK version, `runtime.mode`, the source identity | A verified runtime in the store under `BDK_HOME`; `.bdk/runtime.json` in the project | Prebuilt: no published runtime for this version, or a digest mismatch. Source: see section 7. |
| `codegen` | BDK's interface definitions, the application's Swift command declarations, the resolved file | `@bdk/*` packages in derived data; the `node_modules/@bdk` links | A declaration that is not `Codable`, a name that does not start with `app.`, a grant for an `app.` capability that no command or event declares |
| `pages` | `Pages/` | Packaged pages in the bundle's resources; digests in the resolved file | A Vite failure; output that the content security policy would block; a page over the size the data source accepts |
| `automation` | `Automation/` | Bundled scripts in the bundle's resources; digests in the resolved file | A Rolldown failure; an import that is not `@bdk/*` or bundled. Built in M7: `Contents/Resources/BDK/Automation/<name>.js`, `@bdk/*` inlined, Rolldown from the project; `--watch` is T-AUT-07 |
| `devtools` | `[devtools]` frontend and overlay | `Contents/Resources/BDK/DevTools/{Frontend,Overlay}` | Added in M7: a front end without `index.html`, an empty overlay, a URL |
| `embed` | The runtime from the store | `Contents/Frameworks/BDKRuntime.framework`, copied with its symlinks | A copy that does not match the manifest |
| `sign` | The built bundle, the Xcode signing identity and team | Helper bundle identifiers rewritten to the application's; everything signed inside-out: libraries, helpers, framework, then the application | A signing failure; a helper with the wrong entitlements; an application entitlement on the deny list |

Properties:

- Every step is incremental. Each records the digest of its inputs and does nothing when they are unchanged. Xcode's own input and output file lists are generated to match.
- `--watch` applies to `pages` and `automation`. It rebuilds into the last built bundle; a debug runtime reloads the affected internal page.
- `codegen` runs before the Swift compile only for the TypeScript side. Swift never depends on generated TypeScript.
- `sign` runs last and only in an Xcode build or with an explicit identity. `BDK_SKIP_SIGNING=1` skips it for pipelines that sign later; the result does not launch until signed.
- The steps do not compile Swift and do not invoke `xcodebuild`. Xcode stays the build system.

Why signing is BDK's step: Xcode's "Embed & Sign" is not expected to re-sign the helper applications nested in the framework with the right entitlements (feasibility section 7, unverified). Until task T-PKG-03 shows otherwise, the script does it.

### 5.1 `bdk build sign`, as built and run on 2026-11-18

`bdk build sign --bundle <App.app> --identity <name|-> [--team <id>] [--entitlements <file>]`. In an Xcode build the four values default to `CODESIGNING_FOLDER_PATH`, `EXPANDED_CODE_SIGN_IDENTITY`, `DEVELOPMENT_TEAM` and `CODE_SIGN_ENTITLEMENTS`. `BDK_SKIP_SIGNING=1` skips the step with a warning. The plan is in `packages/cli/src/core/signing/` (pure, tested without `codesign`); every `codesign` and `plutil` call is in the `xcode` module and goes through one runner (`platform/xcode/tools.ts`).

| Part | Identifier | `codesign --options` | Entitlements |
|---|---|---|---|
| Libraries (`Libraries/*.dylib`) | Derived by `codesign` | None | None |
| Loose executables in `Helpers/` | Derived; `<app id>.web_app_shortcut_copier` for that one | `restrict,library,runtime,kill` | None |
| Helper, Aperitif | `<app id>.helper` | Full | None |
| GPU, Aperitif GPU | `<app id>.helper` | `restrict,runtime,kill` | `allow-jit` only |
| Renderer, Aperitif Renderer | `<app id>.helper.renderer` | `restrict,runtime,kill` | `allow-jit` only |
| Alerts, Aperitif Alerts | `<app id>.framework.AlertNotificationService` | Full | None |
| Framework | `<app id>.framework` (its `Info.plist` is rewritten) | None | None |
| Application | `CFBundleIdentifier` | Full | The application's own file |

This follows `parts.py:60-295` at the 158.0.8127.37 pin, except where noted. Order: identifier rewrites, libraries, loose executables, helpers, framework, application, then `codesign --verify --deep --strict` (a failure is exit 4). With a real identity `--timestamp` is added (so `--offline` refuses a real identity, exit 2), and `--team` is checked against the signed application's `TeamIdentifier`. An application entitlement on the deny list (`app-sandbox` and the four `com.apple.security.cs.*` weakenings, in `core/signing/entitlements.ts`) is refused before anything is changed. The deny list is the implementation's choice from configuration.md section 6 and architecture.md section 16; it needs the product owner's confirmation.

Run against a packaged runtime (`~/.bdk/runtimes/a3c82d8de4cecdf1/Chromium.app`, Chromium 158.0.8127.37, cloned with `ditto` into scratch space, the shared original untouched), made into a second application with `CFBundleIdentifier` `com.second.Browser`:

| Observation | Result |
|---|---|
| Before signing, same clone | Launches headless and renders the page. |
| `bdk build sign --bundle Second.app --identity -` | Exit 0, 16 parts signed in about 2.5 s. `codesign --verify --deep --strict` passes. Identifiers are as in the table. The GPU helper carries `com.apple.security.cs.allow-jit`; the plain helper carries no entitlements. |
| `Second.app/Contents/MacOS/Chromium --headless=new --user-data-dir=<fresh> --dump-dom 'data:text/html,<p>hi</p>'` | Prints `<html><head></head><body><p>hi</p></body></html>`. With a page held open, the process list showed `gpu-process`, `renderer` and `utility` children, run from the re-signed `(Aperitif Renderer)` and plain `(Aperitif)` helpers. No process ran from a `(GPU)` or `(Aperitif GPU)` helper application, so the GPU entitlement path was not exercised at runtime. |
| `spctl --assess --type execute -vv Second.app` | `rejected`, exit 3. This is expected: an ad hoc signature has no Developer ID and no notarization ticket. |
| Application entitlement `com.apple.security.app-sandbox` | Refused with `sign.entitlement_denied`, exit 1; the bundle's signature was unchanged. |

Finding that changed the design: Chromium's options cannot be used with an ad hoc identity. The first run, with the options of the table, did not start: `dyld: Library not loaded … code signature … not valid for use in process: mapping process and mapped file (non-platform) have different Team IDs`. The hardened runtime implies library validation, and an ad hoc signature has no team. Dropping only `library` and `runtime` still failed with `security policy does not allow @ path expansion`, caused by `restrict`. Ad hoc signatures therefore get no options. The signatures with a real identity keep Chromium's options, and have not been run: no Developer ID identity was available.

Not verified:

| Item | Why |
|---|---|
| Notarization of an application with the re-signed runtime, and Gatekeeper accepting it | No credentials, and none were sought. Needs a Developer ID identity and a notarization profile. |
| Signing with a real identity, and the hardened options and library validation with a real team | As above. Only the planned `codesign` arguments are unit-tested. |
| Xcode's "Embed & Sign" not re-signing nested helpers (the premise of this step) | Xcode was not run (see section 2.3). |
| `--team` on a real signature | Unit-tested with a fake `codesign` only. |
| `bdk build embed` | Not built. The test copies the runtime with `ditto`. |

The opt-in end-to-end test is `packages/cli/src/platform/xcode/sign.runtime.test.ts`; see `packages/cli/README.md`.

## 6. `bdk doctor`

```text
bdk doctor [--json] [--bundle <App.app>] [--fix]
```

Reports findings in four groups. It reports toolchain, compatibility and signatures. It does not inspect disk space.

| Group | Checks |
|---|---|
| Machine | macOS version against the runtime's minimum. Xcode and Swift versions against the range the BDK version was built for. Apple Silicon (decision D6). Node and a package manager, only if the project uses Vite or Rolldown. In source mode: depot_tools, a writable `BDK_HOME` with no spaces in its path, the managed checkout at the pinned revision, ccache. |
| Project | `bdk.toml` validates. `Info.plist` and entitlements meet [configuration.md](configuration.md) section 6. The BDK version in `Package.resolved` has a published runtime. The runtime in the store matches its manifest. Feature overrides are listed as "outside the tested configuration". Capabilities granted to each page are listed, so a reviewer sees them in one place. |
| Compatibility | ABI version and hash of the Swift package against the runtime. In source mode: the application's patches apply to the pinned revision; the source-built runtime reports the same ABI hash as the prebuilt one for that BDK version. Whether the pinned Chromium is behind the newest BDK release's, with the number of days, because that is a security exposure (risk R4). |
| Built application, with `--bundle` | The resolved file matches `bdk.toml`. Page and script digests match the bundle. Every helper is signed with the application's team and carries only the expected entitlements. The framework's files match the runtime manifest. The signature verifies and, if notarized, the ticket is stapled. |

| Level | Meaning | Exit code |
|---|---|---|
| Error | The application will not build or will not start | 1 |
| Warning | It will run, with a risk the developer should know about: an old engine, feature overrides, a remote provider with no privacy URL | 0 |
| Note | Information | 0 |

`--fix` applies only changes that are mechanical and local: refreshing generated links, re-resolving the runtime. It never edits `bdk.toml`, the Xcode project or signing settings.

Each finding has a fixture and a message (task T-CLI-01).

## 7. Source mode and `bdk patch`

Source mode is for applications that need to change Chromium: `runtime.mode = "source"` in `bdk.toml`. The application's patches are applied on top of BDK's own series, and the result is a runtime with its own source key.

```text
BrowserApp/
└── Chromium/
    ├── patches/
    │   ├── series             # one patch file name per line, in order
    │   └── 0001-…patch
    └── args.gn                # optional; the same content as [chromium] gn_args
```

```text
bdk patch status               # which patches apply cleanly to the pinned revision; which files each touches
bdk patch apply                # apply BDK's series, then the application's, to the managed checkout
bdk patch new <name>           # start a patch: records the current checkout state as the base
bdk patch export               # regenerate patch files from the commits made on top of the base
bdk patch check                # apply to a sparse checkout with no build; for CI
bdk patch rebase <tag>         # try the series against another Chromium tag and report conflicts
```

`bdk build runtime` in source mode:

| Step | Detail |
|---|---|
| 1. Identity | Compute the source key from the Chromium revision, BDK's patches and overlay, the application's patches, the GN arguments, the target and the ABI version (tooling document 5.3) |
| 2. Store | If a runtime with that key is in the store, verify it and stop |
| 3. Lock | Take the checkout lock. If another build holds it, fail at once and name the holder. |
| 4. Sync | `gclient sync` to the pinned revision |
| 5. Patch | Apply both series with `git am --3way`. A conflict names the patch and the file. |
| 6. Build | `gn gen` with BDK's preset plus the application's arguments, then `autoninja` |
| 7. Package | Assemble the framework, write the manifest, move it into the store, make it read-only |

Rules:

| Rule | Detail |
|---|---|
| Opt-in | A Chromium build never starts from an ordinary Xcode build unless `runtime.mode = "source"` and the developer passed `--allow-source-build` or set `BDK_RUNTIME_MODE=source`. Otherwise the step fails and says what to run. |
| Same contract | A source-built runtime must report the same ABI hash as the prebuilt runtime of that BDK version. A patch that changes the ABI is refused by `bdk doctor` and at start. |
| Deny list | GN arguments and patches cannot be checked for intent, but arguments on the deny list in [configuration.md](configuration.md) 3.4.1 are refused, and the runtime manifest records the digest of the application's patches so a release can be audited. |
| Support | A source-built runtime is the application's responsibility to rebuild for each security release. `bdk doctor` warns when the pin is behind. |
| Time and space | A first checkout and build are of the order of 100 GB and hours. Unmeasured; task T-CHR-05. |

## 8. Xcode integration

The template project has two targets.

| Target | Kind | Contents |
|---|---|---|
| `BDKRuntime` | Aggregate | One run-script phase: `bdk build config runtime codegen`. It has no sources. |
| The application | Application | Depends on `BDKRuntime`. Links the `BDK` package product. Two run-script phases: `bdk build pages automation embed` after resources are copied, and `bdk build sign` as the last phase. |

| Topic | Detail |
|---|---|
| Finding the CLI | The phases call the executable from the resolved package's artifacts through a build setting the template defines. No global install is needed to build. |
| Incremental builds | The phases declare input and output file lists that `bdk build config` generates, so Xcode skips them when nothing changed |
| Sandboxed build scripts | Xcode's user-script sandboxing restricts scripts to declared inputs and outputs. The store under `BDK_HOME` is outside the project. The template turns script sandboxing off for these phases; whether it can be left on with declared paths is to be tested in T-CLI-04. |
| Previews and tests | Unit tests link `BDKTestSupport` and need no runtime. UI tests launch the built application. |
| Changing `bdk.toml` or a page | Picked up by the next build, without cleaning |
| Without the template | An existing Xcode project adopts BDK by adding the package, the aggregate target and the three phases. `bdk doctor` reports what is missing. |
| Swift Package Manager only | An application built with `swift build` and no Xcode project is not supported: a macOS application bundle with nested helpers needs the bundle structure and signing that Xcode drives |

## 9. A developer's day

```sh
bdk init                                # asks for each choice; or, with no questions:
bdk init Cove --name Cove --scheme cove --bundle-id com.example.cove \
              --pages vite --framework react --starter-pages new-tab,settings --yes
cd Cove && open Cove.xcodeproj          # Run: a browser window

bdk doctor                              # before filing a bug, and in CI
bdk build pages --watch                 # while editing Pages/Settings, with the application running

# Later, to change Chromium:
#   set runtime.mode = "source" in bdk.toml
bdk doctor                              # checks depot_tools, BDK_HOME, the checkout
bdk build runtime --allow-source-build  # hours, the first time
bdk patch new fix-something             # edit files under $BDK_HOME/chromium/managed/src
bdk patch export
bdk build runtime --allow-source-build  # incremental
```

CI for an application:

```sh
bdk doctor --json                       # machine and project
xcodebuild build …                      # runs bdk build through the phases
bdk doctor --bundle build/Release/Cove.app --json   # digests, signatures, entitlements
```

## 10. Differences from the spec

| Spec | Here | Why |
|---|---|---|
| Three commands: `init`, `patch`, `doctor` | A fourth, `build`, called by Xcode | The build needs a stable entry point (addition S8) |
| "Xcode's `BDKRuntime` target orchestrates tooling" | Kept: an aggregate target, plus two phases on the application target | Pages must be built after resources are copied, and signing must be last |
| `bdk.toml` parsed at build and at runtime | Parsed by `bdk build config` only | One parser (`architecture.md` section 17) |
| Generated declarations stay in derived data | Kept, with a git-ignored link so editors resolve them | Types that an editor cannot find are not usable |
| `bdk init` is described only as "create an application project" | Interactive prompts, a flag for every prompt, and page templates per JavaScript framework | Requested in the 2026-11-16 review |
| The CLI's language is not stated | TypeScript, compiled to a self-contained executable per platform, with the Xcode work in a platform module | Requested in the 2026-11-16 review, for Windows and Linux later |
