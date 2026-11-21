import { parseArgs } from "node:util";
import type { DiagnosticFormat } from "./diagnostics-format.js";
import { usageError } from "./exit-codes.js";

export interface ParsedArgs {
  help: boolean;
  version: boolean;
  json: boolean;
  offline: boolean;
  project?: string;
  /** Forced diagnostics form; undefined means detect from the environment. */
  diagnostics?: DiagnosticFormat;
  /** `--out <file>`, for `build config`. */
  out?: string;
  /** `--bundle <App.app>`, for `build sign`. */
  bundle?: string;
  /** `--identity <name|->`: the signing identity, `-` for ad hoc. */
  identity?: string;
  /** `--team <id>`: the Apple team the signature must carry. */
  team?: string;
  /** `--entitlements <file>`: the application's entitlements, for `build sign`. */
  entitlements?: string;
  /** `--resolved <file>`: the resolved configuration, for `build pages`. */
  resolved?: string;
  positionals: string[];
}

const OPTIONS = {
  help: { type: "boolean", short: "h" },
  version: { type: "boolean", short: "V" },
  json: { type: "boolean" },
  offline: { type: "boolean" },
  project: { type: "string" },
  diagnostics: { type: "string" },
  out: { type: "string" },
  bundle: { type: "string" },
  identity: { type: "string" },
  team: { type: "string" },
  entitlements: { type: "string" },
  resolved: { type: "string" },
} as const;

/** Options that belong to a step rather than to every command. */
export const STEP_FLAGS = [
  "out",
  "bundle",
  "identity",
  "team",
  "entitlements",
  "resolved",
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
    positionals,
  };
  if (values.project !== undefined) parsed.project = values.project;
  for (const flag of STEP_FLAGS) {
    const value = values[flag];
    if (value !== undefined) parsed[flag] = value;
  }
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
