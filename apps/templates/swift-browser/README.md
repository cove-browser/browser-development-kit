# template-swift-browser

The starter application `bdk init` writes (task T-CLI-02). Design: [sdk/cli.md sections 4 and
8](../../../docs/planning/sdk/cli.md). The CLI embeds every file here except this README and `project.json`
(`packages/cli/src/asset-sources.ts`) and substitutes the names; the generator is
`packages/cli/src/core/init/generate.ts`.

## Files

| Path | Becomes |
|---|---|
| `__PRODUCT__.xcodeproj/` | `<Product>.xcodeproj`, where the product is the display name with only letters and digits. Three targets: the application, its unit tests, and the aggregate `BDKRuntime`. File-system synchronised groups (`objectVersion = 77`, Xcode 16 or later), so files added to `Sources/`, `Resources/` and `Tests/` join their target with no project edit. |
| `Sources/` | `main.swift`, `AppDelegate.swift`, `BrowserWindow.swift` (one window, one page, an address field), `AddressInput.swift`, and `RuntimeSource.swift`, the one file that knows the runtime (see below) |
| `Resources/` | `Info.plist` (`NSPrincipalClass = BDKApplication`, usage strings, http and https URL types), `App.entitlements`, `Assets.xcassets` with an empty `AppIcon` |
| `Tests/__PRODUCT__Tests.swift` | One unit test against the stub runtime, in the test process |
| `BuildPhases/` | The scripts the run-script phases call (below) |
| `Automation/`, `package.json` | With `--automation` only: an example script, and Rolldown pinned to the CLI's version, which `bdk build automation` loads from the project |
| `gitignore` | `.gitignore` |

Placeholders in contents: `{{name}}`, `{{product}}`, `{{bundleId}}`, `{{scheme}}`, `{{bdkCli}}`, `{{packageName}}`, and in
the pbxproj `{{packageSection}}`, `{{packageComment}}` and `{{packageFields}}` for the BDK package reference: by URL
at the CLI's exact version, or a local package with `bdk init --bdk-package <path>`. Values are escaped for the
file they land in (Swift strings, property lists, pbxproj strings).

The application links `BDK` and `BDKTestSupport` and starts the stub runtime: `bdk build runtime` and `bdk build
embed` put the Chromium runtime in the bundle, but no prebuilt runtime is published for them to resolve, so the
build phases leave them out with a warning (T-CLI-04). `RuntimeSource.swift` says what to change when one is.

## Build phases

| Script | Target | Runs | Steps missing from this `bdk` today |
|---|---|---|---|
| `BuildPhases/bdk-runtime.sh` | `BDKRuntime` (aggregate, no sources) | `bdk build config runtime codegen`; when the release publishes no runtime, a warning instead of `runtime` | none |
| `BuildPhases/bdk-bundle.sh` | The application, after Copy Bundle Resources | `bdk build pages automation extensions embed`, `embed` only when a runtime is resolved (`.bdk/runtime.json`), then copies `bdk.resolved.json` into `Contents/Resources` | none |
| `BuildPhases/bdk-sign.sh` | The application, the last phase | `bdk build sign`, only when a runtime framework is embedded | none |

Each script sources `bdk-steps.sh`, which runs the steps the CLI has and prints an Xcode `warning:` for each one it
has not, so the phases keep working while the missing steps land. It fails, as an Xcode `error:`, when `BDK_CLI`
names no executable. The template sets `BDK_CLI` to the packaged `bdk` that ran `bdk init`, user script sandboxing
off, and every phase to run on every build (`alwaysOutOfDate`): each step is incremental by digest.

### Adding the phases to an existing Xcode project

1. Define a build setting `BDK_CLI`: the path of the `bdk` executable. Define `BDK_COMMANDS_FILE` too if the
   application's declarations file (`bdk.commands.json`, written by `BDKDeclarations.write(to:)`) is not
   `$(DERIVED_FILE_DIR)/bdk.commands.json`.
2. Add an aggregate target `BDKRuntime` with a Run Script phase that runs `"$SRCROOT/BuildPhases/bdk-runtime.sh"`.
   Make the application depend on it.
3. In the application target, add a Run Script phase after "Copy Bundle Resources" that runs `bdk-bundle.sh`, and a
   last one that runs `bdk-sign.sh`.
4. Set "User Script Sandboxing" (`ENABLE_USER_SCRIPT_SANDBOXING`) to `NO` for the targets that have these phases.

### Input and output file lists

`bdk build config`, when it runs inside Xcode (`DERIVED_FILE_DIR` is set), writes four lists in
`$(DERIVED_FILE_DIR)/bdk/`. One path per line, absolute.

| File | Lists |
|---|---|
| `runtime.input.xcfilelist` | `bdk.toml`, the `INFOPLIST_FILE` and `CODE_SIGN_ENTITLEMENTS` files when they exist, the declarations file when it exists |
| `runtime.output.xcfilelist` | `bdk.resolved.json`; with a Pages or Automation directory, `bdk/js/sdk.digest` and `bdk/js/@bdk/env.d.ts` under `DERIVED_FILE_DIR` |
| `bundle.input.xcfilelist` | `bdk.toml`, `bdk.resolved.json`, every file under the pages directory (not `node_modules`, not dot-files) and under the directory of a route an override moved elsewhere |
| `bundle.output.xcfilelist` | In the bundle's `Contents/Resources`: the directory of each bundled route, and `BDK/js/internal`. Empty with no bundled routes; the bundle lists are not written when Xcode names no bundle. |

The template does not use them: `DERIVED_FILE_DIR` is per target, so the lists `BDKRuntime` writes are not where
the application target's bundle phase would look for its own.

## Targets

| Command | What it does |
|---|---|
| `pnpm exec nx run template-swift-browser:build` | Packages the CLI, then `scripts/build-generated.sh`: `bdk init` with plain starter pages and automation into a temporary directory (Rolldown linked from the repository, so no network), `xcodebuild build` and `test` against this repository's package, then checks the bundle's resolved file, pages and script and its signature |
| `pnpm exec nx run template-swift-browser:lint` | `swift format lint`, SwiftLint, `shellcheck`, `shfmt -d`, `plutil -lint` |

## Verified (2026-11-25, macOS 27.2, Xcode 27.2, Apple Silicon)

The build target passes: the generated application builds, its unit test passes, and the bundle phase packaged
the pages and the automation script. Launched by hand, the application stayed up on the stub runtime. Not
verified: Run from the Xcode GUI, a build with user script sandboxing on, and anything with the Chromium runtime
embedded (it cannot be yet).
