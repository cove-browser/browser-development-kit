import { join, relative } from "node:path";
import { type ParsedArgs, parseCliArgs, STEP_FLAGS } from "./args.js";
import type { Context } from "./context.js";
import type { Diagnostic } from "./diagnostics.js";
import { type DiagnosticFormat, formatDiagnostics } from "./diagnostics-format.js";
import { CliError, ExitCode, type ExitCodeValue, usageError } from "./exit-codes.js";
import { helpText } from "./help.js";
import { findProject, type Project } from "./project.js";
import { BUILD_STEPS } from "./steps/index.js";
import type { Step } from "./steps/step.js";
import { watchLoop } from "./watch.js";

function diagnosticFormat(ctx: Context, args: ParsedArgs | undefined): DiagnosticFormat {
  if (args?.diagnostics !== undefined) return args.diagnostics;
  return ctx.platform.isIdeBuild(ctx.env) ? "xcode" : "human";
}

/** Colour only on a terminal, and not when `NO_COLOR` is set. */
function useColor(ctx: Context): boolean {
  return ctx.stderrIsTTY && ctx.env.NO_COLOR === undefined && ctx.env.TERM !== "dumb";
}

interface Report {
  command: string;
  exitCode: ExitCodeValue;
  diagnostics: Diagnostic[];
  data?: Record<string, unknown>;
}

/** Diagnostics always go to standard error; `--json` adds one document on standard output. */
function report(ctx: Context, args: ParsedArgs | undefined, json: boolean, r: Report): number {
  ctx.stderr(
    formatDiagnostics(r.diagnostics, {
      format: diagnosticFormat(ctx, args),
      color: useColor(ctx),
    }),
  );
  if (json) {
    const document = {
      ok: r.exitCode === ExitCode.Success,
      exitCode: r.exitCode,
      command: r.command,
      diagnostics: r.diagnostics,
      ...(r.data === undefined ? {} : { result: r.data }),
    };
    ctx.stdout(`${JSON.stringify(document, null, 2)}\n`);
  }
  return r.exitCode;
}

function selectSteps(ctx: Context, args: ParsedArgs): Step[] {
  const names = args.positionals.slice(1);
  const known = BUILD_STEPS.map((s) => s.name).join(", ");
  const chosen =
    names.length === 0
      ? // With no step, the steps that apply, in order (sdk/cli.md section 5).
        BUILD_STEPS.filter((step) => step.applies?.({ ctx, args }) ?? true)
      : names.map((name) => {
          const step = BUILD_STEPS.find((s) => s.name === name);
          if (step === undefined)
            throw usageError(`Unknown build step "${name}". Steps: ${known}.`);
          return step;
        });
  for (const flag of STEP_FLAGS) {
    if (args[flag] !== undefined && !chosen.some((s) => s.flags.includes(flag))) {
      const owners = BUILD_STEPS.filter((s) => s.flags.includes(flag)).map((s) => s.name);
      throw usageError(`--${flag} applies to \`bdk build ${owners.join("`, `bdk build ")}\` only.`);
    }
  }
  // `--out` is the resolved file for `config`, `automation` and `devtools`, and a directory for
  // `codegen`: one value cannot be both.
  if (
    args.out !== undefined &&
    chosen.some((s) => s.name === "codegen") &&
    chosen.filter((s) => s.flags.includes("out")).length > 1
  ) {
    throw usageError(
      "--out names a file for `bdk build config`, `bdk build automation` and `bdk build devtools` and a directory for `bdk build codegen`, so it cannot be given to codegen and another of them in one run.",
      "Run `bdk build codegen` separately, with its own --out.",
    );
  }
  if (args.watch) {
    // One step at a time: a loop watches one directory (pages joins automation in T-CLI-06).
    const only = chosen.length === 1 ? chosen[0] : undefined;
    if (only?.watchDirectory === undefined) {
      const watchable = BUILD_STEPS.filter((s) => s.watchDirectory !== undefined).map(
        (s) => s.name,
      );
      throw usageError(
        `--watch applies to \`bdk build ${watchable.join("`, `bdk build ")}\` only, one step at a time.`,
        "Name the step, for example `bdk build automation --bundle <App.app> --watch`.",
      );
    }
  }
  return chosen;
}

async function runBuild(
  ctx: Context,
  args: ParsedArgs,
  steps: readonly Step[],
  project: Project,
): Promise<Report> {
  const diagnostics: Diagnostic[] = [];
  const data: Record<string, unknown> = {};
  let exitCode: ExitCodeValue = ExitCode.Success;
  for (const step of steps) {
    const result = await step.run({ project, ctx, args });
    diagnostics.push(...result.diagnostics);
    if (result.data !== undefined) data[step.name] = result.data;
    if (result.exitCode !== ExitCode.Success) {
      exitCode = result.exitCode;
      break;
    }
  }
  return { command: `build ${steps.map((s) => s.name).join(" ")}`, exitCode, diagnostics, data };
}

/** The report for an error that stopped the command: its own exit code if it is a `CliError`, else an environment failure. */
function failureReport(command: string, error: unknown): Report {
  return error instanceof CliError
    ? { command, exitCode: error.exitCode, diagnostics: [error.diagnostic] }
    : {
        command,
        exitCode: ExitCode.Environment,
        diagnostics: [
          {
            code: "cli.internal",
            severity: "error",
            message: error instanceof Error ? error.message : String(error),
            fix: "Report this as a bug in bdk.",
          },
        ],
      };
}

/**
 * `--watch`: the first build has run; rebuild the one chosen step whenever its directory changes, until
 * the user stops it. A failed rebuild is reported and watching goes on. Stopping is success.
 */
async function watchBuild(
  ctx: Context,
  args: ParsedArgs,
  steps: readonly Step[],
  project: Project,
): Promise<number> {
  const [step] = steps;
  const directory = join(project.dir, (await step?.watchDirectory?.(project)) ?? "");
  if (!args.json) {
    ctx.stdout(
      `bdk build ${step?.name}: watching ${relative(ctx.cwd, directory) || "."} (Ctrl-C to stop).\n`,
    );
  }
  await watchLoop({
    watch: ctx.watch,
    directory,
    rebuild: async () => {
      try {
        report(ctx, args, args.json, await runBuild(ctx, args, steps, project));
      } catch (error) {
        report(ctx, args, args.json, failureReport("build", error));
      }
    },
  });
  return ExitCode.Success;
}

/** Runs the CLI with `argv` (without the executable and script) and returns the exit code. */
export async function run(argv: readonly string[], ctx: Context): Promise<number> {
  // `--json` is honoured even when the rest of the arguments fail to parse.
  const json = argv.includes("--json");
  let args: ParsedArgs | undefined;
  let command = "bdk";
  try {
    args = parseCliArgs(argv);
    if (args.help) {
      ctx.stdout(helpText(BUILD_STEPS.map((s) => s.name)));
      return ExitCode.Success;
    }
    if (args.version) {
      ctx.stdout(json ? `${JSON.stringify({ version: ctx.version })}\n` : `${ctx.version}\n`);
      return ExitCode.Success;
    }
    const name = args.positionals[0];
    if (name === undefined) {
      ctx.stderr(helpText(BUILD_STEPS.map((s) => s.name)));
      throw usageError("No command given. Commands: build.");
    }
    if (name !== "build") throw usageError(`Unknown command "${name}". Commands: build.`);
    command = "build";
    const steps = selectSteps(ctx, args);
    const project = await findProject(ctx.cwd, args.project);
    const first = report(ctx, args, json, await runBuild(ctx, args, steps, project));
    return args.watch ? await watchBuild(ctx, args, steps, project) : first;
  } catch (error) {
    return report(ctx, args, json, failureReport(command, error));
  }
}
