/** The `--help` text. `steps` are the registered `bdk build` steps. */
export function helpText(steps: readonly string[]): string {
  return `bdk: the Browser Development Kit command line tool

Usage:
  bdk init [<directory>] [--name <display name>] [--bundle-id <identifier>] [--scheme <scheme>]
           [--template swift-browser] [--pages none|plain|vite]
           [--framework vanilla|react|preact] [--language ts|js] [--styling css|tailwind]
           [--starter-pages <route>,...] [--package-manager pnpm|npm|yarn|bun] [--no-install]
           [--automation] [--runtime prebuilt|source] [--no-git] [--bdk-package <url|path>]
           [--yes] [--no-input] [--force]
  bdk doctor [--bundle <App.app>]
  bdk patch status|apply|export|check
  bdk patch new <name>
  bdk patch rebase <tag>
  bdk build [<step>...]
  bdk build config [--out <file>]
  bdk build codegen [--out <dir>] [--commands <file>] [--resolved <file>]
  bdk build pages [--bundle <App.app>] [--resolved <file>]
  bdk build automation --bundle <App.app> [--out <file>] [--watch]
  bdk build devtools --bundle <App.app> [--out <file>]
  bdk build extensions --bundle <App.app> [--out <file>]
  bdk build sign --bundle <App.app> --identity <name|-> [--team <id>] [--entitlements <file>]
  bdk --help | --version

Commands:
  init          Create an application project. Asks for each choice not given as a flag, only on a
                terminal; --yes or --no-input takes the defaults, and a choice with no default must be
                a flag. Prints the command that makes the same project with no questions
  doctor        Check the machine, the project, compatibility and, with --bundle, a built application.
                Exits 1 when a finding is an error
  patch         Manage the application's Chromium patches in source mode: status, apply (BDK's series,
                then the application's, to the managed checkout), new, export (the commits on top of
                BDK's series into the project), check (against the pinned Chromium, no build), rebase
                (against another tag)
  build         Run build steps. Steps: ${steps.join(", ")}.
                With no step, runs those that apply, in that order: pages, automation, devtools and
                extensions need a bundle (--bundle, or an Xcode build); sign needs an Xcode build or
                --identity

Options:
  --project <dir>         Use the project in <dir> instead of the nearest ancestor with bdk.toml
  --json                  Print one JSON document on standard output; diagnostics stay on standard error
  --offline               Never use the network (fail instead)
  --diagnostics <form>    Print diagnostics as "human" or "xcode" (file:line: error: message).
                          Default: "xcode" inside an Xcode build, otherwise "human"
  --out <path>            For \`build config\`, \`build automation\`, \`build devtools\` and
                          \`build extensions\`: the file bdk.resolved.json is written to and read from.
                          For \`build codegen\`: the directory to write the @bdk packages to (Xcode:
                          $DERIVED_FILE_DIR/bdk/js, else <project>/.bdk/js). Not for codegen and another
                          of them in one run
  --commands <file>       For \`build codegen\`: the application's declarations, bdk.commands.json (Xcode:
                          $BDK_COMMANDS_FILE, else $DERIVED_FILE_DIR/bdk.commands.json; else
                          <project>/.bdk/bdk.commands.json). A missing file means no app. declarations
  --bundle, --resolved    For \`build pages\`: the application to write the pages into (in an Xcode build,
                          CODESIGNING_FOLDER_PATH, else TARGET_BUILD_DIR/WRAPPER_NAME) and the resolved
                          configuration to read (default: bdk.resolved.json next to bdk.toml; \`build codegen\`
                          reads the same file). --bundle is also the application for \`build automation\`,
                          \`build devtools\`, \`build extensions\`, \`build sign\` and \`doctor\`
  --identity, --team, --entitlements
                          For \`build sign\`. In an Xcode build they default to CODESIGNING_FOLDER_PATH,
                          EXPANDED_CODE_SIGN_IDENTITY, DEVELOPMENT_TEAM and CODE_SIGN_ENTITLEMENTS.
                          BDK_SKIP_SIGNING=1 skips the step
  --watch                 For \`build automation\`: build once, then rebuild when a script changes, until Ctrl-C
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
