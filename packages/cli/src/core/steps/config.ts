import { mkdir, writeFile } from "node:fs/promises";
import { dirname, isAbsolute, resolve } from "node:path";
import { AssetNotFoundError } from "../../assets.js";
import { serialiseResolved } from "../config/index.js";
import type { Diagnostic } from "../diagnostics.js";
import { hasErrors } from "../diagnostics-format.js";
import { CliError, ExitCode } from "../exit-codes.js";
import { buildFileLists } from "../filelists.js";
import type { Step, StepResult } from "./step.js";

export const SCHEMA_ASSET = "bdk.schema.json";
export const RESOLVED_FILE = "bdk.resolved.json";

/**
 * `bdk build config`: resolve `bdk.toml` against the embedded schema and the platform's bundle facts,
 * and write `bdk.resolved.json` only when there are no errors.
 */
export const configStep: Step = {
  name: "config",
  flags: ["out"],
  async run({ project, ctx, args }): Promise<StepResult> {
    let schema: unknown;
    try {
      schema = JSON.parse(ctx.assets.readText(SCHEMA_ASSET));
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
    const bundleFacts = await ctx.platform.bundleFacts(ctx.env, project.dir);
    const result = await ctx.resolveConfig({
      projectDir: project.dir,
      schema,
      // The CLI ships with the SDK and runtime and carries their version (cli.md section 2).
      bdkVersion: ctx.version,
      ...(bundleFacts === undefined ? {} : { bundleFacts }),
    });
    const diagnostics = result.diagnostics.map((d) => inProject(d, project.dir));
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
function inProject(diagnostic: Diagnostic, projectDir: string): Diagnostic {
  if (diagnostic.file === undefined || isAbsolute(diagnostic.file)) return diagnostic;
  return { ...diagnostic, file: resolve(projectDir, diagnostic.file) };
}
