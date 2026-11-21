# template-swift-browser

Starter application used by `bdk init` (xcodebuild).

Status: skeleton. The Xcode project is not implemented until M9. `BuildPhases/` is: the scripts its run-script
phases call. Design: [sdk/cli.md section 8](../../../docs/planning/sdk/cli.md).

## Build phases

| Script | Target | Runs | Steps missing from this `bdk` today |
|---|---|---|---|
| `BuildPhases/bdk-runtime.sh` | `BDKRuntime` (aggregate, no sources) | `bdk build config runtime codegen` | `runtime` |
| `BuildPhases/bdk-bundle.sh` | The application, after Copy Bundle Resources | `bdk build pages automation embed` | `automation`, `embed` |
| `BuildPhases/bdk-sign.sh` | The application, the last phase | `bdk build sign` | none |

Each script sources `bdk-steps.sh`, which runs the steps the CLI has and prints an Xcode `warning:` for each one it
has not, so the phases keep working while the missing steps land. It fails, as an Xcode `error:`, when `BDK_CLI`
names no executable. With `runtime` and `embed` missing, an application built from this template has no runtime in
its bundle: it builds, signs and runs only against the stub.

### Adding the phases to an Xcode project

1. Define a build setting `BDK_CLI`: the path of the `bdk` executable from the resolved package's artifacts
   (`bdk.artifactbundle`). Define `BDK_COMMANDS_FILE` too if the application's declarations file
   (`bdk.commands.json`, written by `BDKDeclarations.write(to:)`) is not `$(DERIVED_FILE_DIR)/bdk.commands.json`.
2. Add an aggregate target `BDKRuntime` with a Run Script phase that runs `"$SRCROOT/BuildPhases/bdk-runtime.sh"`.
   Make the application depend on it.
3. In the application target, add a Run Script phase after "Copy Bundle Resources" that runs
   `bdk-bundle.sh`, and a last one that runs `bdk-sign.sh`.
4. For the bundle phase, tick "Based on dependency analysis" and set the input and output file lists to
   `$(DERIVED_FILE_DIR)/bdk/bundle.input.xcfilelist` and `$(DERIVED_FILE_DIR)/bdk/bundle.output.xcfilelist`.
   Leave the runtime phase unticked: it is incremental by digest and cheap (see below).
5. Set "User Script Sandboxing" (`ENABLE_USER_SCRIPT_SANDBOXING`) to `NO` for the targets that have these phases.

### Input and output file lists

`bdk build config`, when it runs inside Xcode (`DERIVED_FILE_DIR` is set), writes four lists in
`$(DERIVED_FILE_DIR)/bdk/`. One path per line, absolute.

| File | Lists |
|---|---|
| `runtime.input.xcfilelist` | `bdk.toml`, the `INFOPLIST_FILE` and `CODE_SIGN_ENTITLEMENTS` files when they exist, the declarations file when it exists |
| `runtime.output.xcfilelist` | `bdk.resolved.json`; with a Pages or Automation directory, `bdk/js/sdk.digest` and `bdk/js/@bdk/env.d.ts` under `DERIVED_FILE_DIR` |
| `bundle.input.xcfilelist` | `bdk.toml`, `bdk.resolved.json`, every file under the pages directory (not `node_modules`, not dot-files) and under the directory of a route an override moved elsewhere |
| `bundle.output.xcfilelist` | In the bundle's `Contents/Resources`: the directory of each bundled route, and `BDK/js/internal`. Empty with no bundled routes; the bundle lists are not written when Xcode names no bundle. |

Outputs are directories because a Vite build's file names are hashed; `bdk build pages` replaces each directory, so
its modification time moves. The files exist only after the first `bdk build config`, so a phase that reads them
must run after a phase that wrote them. That is why the runtime phase (which runs `config`) does not use its own
lists: on the first build they would not exist yet, and Xcode refuses a missing file list. The `BDKRuntime` target
runs first, so the bundle phase's lists exist by the time it is checked.

### Not verified

None of this has been run in Xcode: there is no Xcode project yet, and Xcode could not run on the machine that wrote
it. What is checked: the scripts with `shellcheck`, the CLI with the Xcode variables set by hand
(`src/platform/xcode/build.test.ts`), and the file lists' contents. Not checked:

- That Xcode accepts directories as outputs, and a missing input as "always run".
- That a build with user script sandboxing left on works. cli.md section 8 says it is to be tested. The phases read
  the project and `BDK_HOME` and write under `DERIVED_FILE_DIR` and the bundle; declaring all of that as inputs and
  outputs may be enough. Until it is shown, turn the sandbox off for these phases.
- That `bdk` finds `CODESIGNING_FOLDER_PATH`, `DERIVED_FILE_DIR` and the other settings in a real phase's
  environment, beyond what is documented.
