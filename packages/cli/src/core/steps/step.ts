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
  /**
   * Whether `bdk build` with no step runs this one (sdk/cli.md section 5: "the steps that apply"). Absent
   * means always. A step named on the command line always runs.
   */
  applies?(input: Omit<StepInput, "project">): boolean;
  /** Flags this step reads besides the global ones. */
  flags: readonly StepFlag[];
  /**
   * The directory `--watch` watches, relative to the project; absent when the step has no `--watch`.
   * Registered only by steps whose rebuild is incremental.
   */
  watchDirectory?(project: Project): Promise<string>;
  run(input: StepInput): Promise<StepResult>;
}
