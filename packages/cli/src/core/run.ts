import { type ParsedArgs, parseCliArgs, STEP_FLAGS } from "./args.js";
import type { Context } from "./context.js";
import type { Diagnostic } from "./diagnostics.js";
import { type DiagnosticFormat, formatDiagnostics } from "./diagnostics-format.js";
import { CliError, ExitCode, type ExitCodeValue, usageError } from "./exit-codes.js";
import { helpText } from "./help.js";
import { findProject } from "./project.js";
import { BUILD_STEPS } from "./steps/index.js";
import type { Step } from "./steps/step.js";

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
  return chosen;
}

async function runBuild(ctx: Context, args: ParsedArgs): Promise<Report> {
  const steps = selectSteps(ctx, args);
  const project = await findProject(ctx.cwd, args.project);
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
    return report(ctx, args, json, await runBuild(ctx, args));
  } catch (error) {
    const failure: Report =
      error instanceof CliError
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
    return report(ctx, args, json, failure);
  }
}
