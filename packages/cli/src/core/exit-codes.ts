import type { Diagnostic } from "./diagnostics.js";

/** Exit codes from sdk/cli.md section 3. */
export const ExitCode = {
  Success: 0,
  /** The check or build failed for a reason in the project. */
  ProjectFailure: 1,
  Usage: 2,
  /** The environment is unusable: missing toolchain, a missing embedded asset, an internal fault. */
  Environment: 3,
  /** A digest or signature did not match. */
  Integrity: 4,
} as const;

export type ExitCodeValue = (typeof ExitCode)[keyof typeof ExitCode];

/** An error that ends the run with a diagnostic and a specific exit code. */
export class CliError extends Error {
  readonly diagnostic: Diagnostic;
  readonly exitCode: ExitCodeValue;

  constructor(diagnostic: Diagnostic, exitCode: ExitCodeValue) {
    super(diagnostic.message);
    this.name = "CliError";
    this.diagnostic = diagnostic;
    this.exitCode = exitCode;
  }
}

/** A usage error: exit code 2, code `cli.usage`. */
export function usageError(message: string, fix = "Run `bdk --help`."): CliError {
  return new CliError({ code: "cli.usage", severity: "error", message, fix }, ExitCode.Usage);
}
