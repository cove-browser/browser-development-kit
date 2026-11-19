import { mkdir, writeFile } from "node:fs/promises";
import { dirname, resolve } from "node:path";
import { AssetNotFoundError } from "../../assets.js";
import { hasErrors } from "../diagnostics-format.js";
import { CliError, ExitCode } from "../exit-codes.js";
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
      ...(bundleFacts === undefined ? {} : { bundleFacts }),
    });
    if (hasErrors(result.diagnostics)) {
      return {
        diagnostics: result.diagnostics,
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
    await writeFile(out, `${JSON.stringify(result.resolved, null, 2)}\n`);
    return {
      diagnostics: result.diagnostics,
      exitCode: ExitCode.Success,
      data: { wrote: true, out },
    };
  },
};
