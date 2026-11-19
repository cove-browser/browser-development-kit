import type { ParsedArgs, StepFlag } from "../args.js";
import type { Context } from "../context.js";
import type { Diagnostic } from "../diagnostics.js";
import type { ExitCodeValue } from "../exit-codes.js";
import type { Project } from "../project.js";

export interface StepInput {
  project: Project;
  ctx: Context;
  args: ParsedArgs;
}

export interface StepResult {
  diagnostics: Diagnostic[];
  exitCode: ExitCodeValue;
  /** Step output for the `--json` document. */
  data?: Record<string, unknown>;
}

/** One `bdk build <step>`. Only steps that work are registered (steps/index.ts). */
export interface Step {
  name: string;
  /** Flags this step reads besides the global ones. */
  flags: readonly StepFlag[];
  run(input: StepInput): Promise<StepResult>;
}
