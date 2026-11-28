import { mkdir, writeFile } from "node:fs/promises";
import { dirname, isAbsolute, resolve } from "node:path";
import { AssetNotFoundError } from "../../assets.js";
import { serialiseResolved } from "../config/index.js";
import type { Context } from "../context.js";
import type { Diagnostic } from "../diagnostics.js";
import { hasErrors } from "../diagnostics-format.js";
import { CliError, ExitCode } from "../exit-codes.js";
import { buildFileLists } from "../filelists.js";
import type { Project } from "../project.js";
import type { Step, StepResult } from "./step.js";

export const SCHEMA_ASSET = "bdk.schema.json";
export const RESOLVED_FILE = "bdk.resolved.json";

/**
 * `bdk build config`: resolve `bdk.toml` against the embedded schema and the platform's bundle facts,
 * and write `bdk.resolved.json` only when there are no errors.
 */
/** The embedded `bdk.toml` schema; a `bdk` without it is an environment failure (exit 3). */
export function readSchema(ctx: Context): unknown {
  try {
    return JSON.parse(ctx.assets.readText(SCHEMA_ASSET));
  } catch (error) {
    if (error instanceof AssetNotFoundError) {
      throw new CliError(
        {
          code: "cli.asset_missing",
          severity: "error",
          message: `This bdk has no embedded ${SCHEMA_ASSET}.`,
          fix: "Reinstall bdk from a release, or rebuild it with `pnpm cli:package`.",
        },
        ExitCode.Environment,
      );
    }
    throw error;
  }
}

/**
 * Resolves the project's configuration with the platform's bundle facts read from `env`, as
 * `bdk build config` and `bdk doctor` both do. Diagnostic files are absolute.
 */
export async function resolveProject(
  ctx: Context,
  project: Project,
  env: NodeJS.ProcessEnv = ctx.env,
): Promise<{ diagnostics: Diagnostic[]; resolved?: Record<string, unknown> }> {
  const schema = readSchema(ctx);
  const bundleFacts = await ctx.platform.bundleFacts(env, project.dir);
  const result = await ctx.resolveConfig({
    projectDir: project.dir,
    schema,
    // The CLI ships with the SDK and runtime and carries their version (cli.md section 2).
    bdkVersion: ctx.version,
    ...(bundleFacts === undefined ? {} : { bundleFacts }),
  });
  return {
    diagnostics: result.diagnostics.map((d) => inProject(d, project.dir)),
    ...(result.resolved === undefined ? {} : { resolved: result.resolved }),
  };
}

export const configStep: Step = {
  name: "config",
  flags: ["out"],
  async run({ project, ctx, args }): Promise<StepResult> {
    const result = await resolveProject(ctx, project);
    const { diagnostics } = result;
    if (hasErrors(diagnostics)) {
      return {
        diagnostics,
        exitCode: ExitCode.ProjectFailure,
        data: { wrote: false },
      };
    }
    if (result.resolved === undefined) {
      throw new CliError(
        {
          code: "cli.internal",
          severity: "error",
          message: "The configuration resolver returned no result and no error.",
          fix: "Report this as a bug in bdk.",
        },
        ExitCode.Environment,
      );
    }
    const out =
      args.out === undefined ? resolve(project.dir, RESOLVED_FILE) : resolve(ctx.cwd, args.out);
    await mkdir(dirname(out), { recursive: true });
    await writeFile(out, serialiseResolved(result.resolved));
    const locations = ctx.platform.buildLocations(ctx.env);
    const fileLists =
      locations === undefined
        ? []
        : await ctx.platform.writeFileLists(
            ctx.env,
            await buildFileLists({
              project,
              ctx,
              resolvedFile: out,
              jsOut: locations.jsOut,
              commandsFile: locations.commandsFile,
            }),
          );
    return {
      diagnostics,
      exitCode: ExitCode.Success,
      data: { wrote: true, out, ...(fileLists.length === 0 ? {} : { fileLists }) },
    };
  },
};

/** The resolver reports project-relative paths; Xcode and terminals need them absolute. */
export function inProject(diagnostic: Diagnostic, projectDir: string): Diagnostic {
  if (diagnostic.file === undefined || isAbsolute(diagnostic.file)) return diagnostic;
  return { ...diagnostic, file: resolve(projectDir, diagnostic.file) };
}
