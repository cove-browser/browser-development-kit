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
} as const;

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
  if (values.out !== undefined) parsed.out = values.out;
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
