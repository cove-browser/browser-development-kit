import { parseArgs } from "node:util";
import type { DiagnosticFormat } from "./diagnostics-format.js";
import { usageError } from "./exit-codes.js";

export interface ParsedArgs {
  help: boolean;
  version: boolean;
  json: boolean;
  offline: boolean;
  /** `--watch`: rebuild when the step's sources change, until interrupted. */
  watch: boolean;
  project?: string;
  /** Forced diagnostics form; undefined means detect from the environment. */
  diagnostics?: DiagnosticFormat;
  /** `--out <path>`: the resolved file for `build config`, `build automation`, `build devtools` and `build extensions`; the output directory for `build codegen`. */
  out?: string;
  /** `--bundle <App.app>`, for `build pages`, `build automation`, `build devtools`, `build extensions` and `build sign`. */
  bundle?: string;
  /** `--identity <name|->`: the signing identity, `-` for ad hoc. */
  identity?: string;
  /** `--team <id>`: the Apple team the signature must carry. */
  team?: string;
  /** `--entitlements <file>`: the application's entitlements, for `build sign`. */
  entitlements?: string;
  /** `--resolved <file>`: the resolved configuration, for `build pages`. */
  resolved?: string;
  /** `--commands <file>`: the application's declarations (`bdk.commands.json`), for `build codegen`. */
  commands?: string;
  /** The choices given as flags to `bdk init` (sdk/cli.md section 4.2); absent when there are none. */
  init?: InitFlags;
  positionals: string[];
}

/** `bdk init` flags that take a value. */
export const INIT_VALUE_FLAGS = [
  "name",
  "bundle-id",
  "scheme",
  "template",
  "pages",
  "framework",
  "language",
  "styling",
  "starter-pages",
  "package-manager",
  "runtime",
  "bdk-package",
] as const;
/** `bdk init` flags that are switches. */
export const INIT_SWITCH_FLAGS = [
  "no-install",
  "automation",
  "no-git",
  "yes",
  "no-input",
  "force",
] as const;
export type InitValueFlag = (typeof INIT_VALUE_FLAGS)[number];
export type InitSwitchFlag = (typeof INIT_SWITCH_FLAGS)[number];
export type InitFlags = { [K in InitValueFlag]?: string } & { [K in InitSwitchFlag]?: true };

const INIT_OPTIONS = Object.fromEntries([
  ...INIT_VALUE_FLAGS.map((flag) => [flag, { type: "string" }]),
  ...INIT_SWITCH_FLAGS.map((flag) => [flag, { type: "boolean" }]),
]) as { [K in InitValueFlag]: { type: "string" } } & { [K in InitSwitchFlag]: { type: "boolean" } };

const OPTIONS = {
  help: { type: "boolean", short: "h" },
  version: { type: "boolean", short: "V" },
  json: { type: "boolean" },
  offline: { type: "boolean" },
  watch: { type: "boolean" },
  project: { type: "string" },
  diagnostics: { type: "string" },
  out: { type: "string" },
  bundle: { type: "string" },
  identity: { type: "string" },
  team: { type: "string" },
  entitlements: { type: "string" },
  resolved: { type: "string" },
  commands: { type: "string" },
  ...INIT_OPTIONS,
} as const;

/**
 * The commands each option belongs to, besides the global ones. An option given to another command is
 * a usage error that names its owners.
 */
export const FLAG_COMMANDS: Readonly<Record<string, readonly string[]>> = {
  out: ["build"],
  bundle: ["build", "doctor"],
  identity: ["build"],
  team: ["build"],
  entitlements: ["build"],
  resolved: ["build"],
  commands: ["build"],
  watch: ["build"],
  ...Object.fromEntries(
    [...INIT_VALUE_FLAGS, ...INIT_SWITCH_FLAGS].map((flag) => [flag, ["init"]]),
  ),
};

/** The options of `args` that were given, by their command-line names. */
export function givenFlags(args: ParsedArgs): string[] {
  const given: string[] = [];
  for (const flag of STEP_FLAGS) if (args[flag] !== undefined) given.push(flag);
  if (args.watch) given.push("watch");
  for (const flag of Object.keys(args.init ?? {})) given.push(flag);
  return given;
}

/** Options that belong to a step rather than to every command. */
export const STEP_FLAGS = [
  "out",
  "bundle",
  "identity",
  "team",
  "entitlements",
  "resolved",
  "commands",
] as const;
export type StepFlag = (typeof STEP_FLAGS)[number];

/** Parses `argv` (without the executable and script). Throws a usage error (exit 2) on bad input. */
export function parseCliArgs(argv: readonly string[]): ParsedArgs {
  let values: ReturnType<typeof parseArgs<{ options: typeof OPTIONS }>>["values"];
  let positionals: string[];
  try {
    ({ values, positionals } = parseArgs({
      args: [...argv],
      options: OPTIONS,
      allowPositionals: true,
      strict: true,
    }));
  } catch (error) {
    throw usageError(error instanceof Error ? error.message : String(error));
  }
  const parsed: ParsedArgs = {
    help: values.help === true,
    version: values.version === true,
    json: values.json === true,
    offline: values.offline === true,
    watch: values.watch === true,
    positionals,
  };
  if (values.project !== undefined) parsed.project = values.project;
  for (const flag of STEP_FLAGS) {
    const value = values[flag];
    if (value !== undefined) parsed[flag] = value;
  }
  const init: InitFlags = {};
  for (const flag of INIT_VALUE_FLAGS) {
    const value = values[flag];
    if (value !== undefined) init[flag] = value;
  }
  for (const flag of INIT_SWITCH_FLAGS) {
    if (values[flag] === true) init[flag] = true;
  }
  if (Object.keys(init).length > 0) parsed.init = init;
  if (values.diagnostics !== undefined) {
    if (values.diagnostics !== "human" && values.diagnostics !== "xcode") {
      throw usageError(
        `--diagnostics takes "human" or "xcode", not "${values.diagnostics}".`,
        "Pass --diagnostics human or --diagnostics xcode, or leave it out to detect Xcode.",
      );
    }
    parsed.diagnostics = values.diagnostics;
  }
  return parsed;
}
