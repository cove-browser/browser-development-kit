/** The `--help` text. `steps` are the registered `bdk build` steps. */
export function helpText(steps: readonly string[]): string {
  return `bdk: the Browser Development Kit command line tool

Usage:
  bdk build config [--out <file>]
  bdk build automation --bundle <App.app> [--out <file>]
  bdk build sign --bundle <App.app> --identity <name|-> [--team <id>] [--entitlements <file>]
  bdk --help | --version

Commands:
  build         Run build steps. Steps: ${steps.join(", ")}

Options:
  --project <dir>         Use the project in <dir> instead of the nearest ancestor with bdk.toml
  --json                  Print one JSON document on standard output; diagnostics stay on standard error
  --offline               Never use the network (fail instead)
  --diagnostics <form>    Print diagnostics as "human" or "xcode" (file:line: error: message).
                          Default: "xcode" inside an Xcode build, otherwise "human"
  --out <file>            For \`build config\` and \`build automation\`: where bdk.resolved.json is written and read
  --bundle                For \`build automation\` and \`build sign\`: the application, <App.app>
  --identity, --team, --entitlements
                          For \`build sign\`. In an Xcode build they default to CODESIGNING_FOLDER_PATH,
                          EXPANDED_CODE_SIGN_IDENTITY, DEVELOPMENT_TEAM and CODE_SIGN_ENTITLEMENTS.
                          BDK_SKIP_SIGNING=1 skips the step
  -h, --help              Show this help
  -V, --version           Show the version

Exit codes:
  0  Success
  1  The check or build failed for a reason in the project
  2  Usage error
  3  The environment is unusable
  4  An integrity failure: a digest or signature did not match
`;
}
