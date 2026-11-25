import { chmod, mkdir, readdir, stat, writeFile } from "node:fs/promises";
import { dirname, join, relative, resolve } from "node:path";
import type { ParsedArgs } from "../args.js";
import type { Context } from "../context.js";
import type { Diagnostic } from "../diagnostics.js";
import { CliError, ExitCode, type ExitCodeValue, usageError } from "../exit-codes.js";
import { productName } from "./choices.js";
import { generateProject, packageSource } from "./generate.js";
import { collectAnswers, replayCommand } from "./prompts.js";

export interface InitReport {
  exitCode: ExitCodeValue;
  diagnostics: Diagnostic[];
  data: Record<string, unknown>;
}

async function exists(path: string): Promise<boolean> {
  try {
    await stat(path);
    return true;
  } catch {
    return false;
  }
}

/**
 * `bdk init [<directory>]`: asks for each choice not given as a flag (sdk/cli.md section 4), writes the
 * project, then creates a Git repository and installs the page packages when asked to. It keeps no state
 * of its own: every answer is in a file of the project.
 */
export async function runInit(ctx: Context, args: ParsedArgs): Promise<InitReport> {
  if (args.positionals.length > 2) {
    throw usageError(`bdk init takes one directory, not ${args.positionals.length - 1}.`);
  }
  if (args.project !== undefined) {
    throw usageError("--project does not apply to `bdk init`; name the new directory instead.");
  }
  const flags = args.init ?? {};
  const answers = await collectAnswers(ctx, flags, args.positionals[1]);
  const dir = resolve(ctx.cwd, answers.directory);
  const diagnostics: Diagnostic[] = [];

  if ((await exists(dir)) && (await readdir(dir)).length > 0 && !flags.force) {
    throw new CliError(
      {
        code: "init.directory_not_empty",
        severity: "error",
        message: `${dir} is not empty.`,
        file: dir,
        fix: "Choose a new directory, or pass --force to add the project's files to it; no existing file is overwritten.",
      },
      ExitCode.ProjectFailure,
    );
  }

  const files = generateProject({
    answers,
    assets: ctx.assets,
    version: ctx.version,
    package: packageSource(flags["bdk-package"], ctx.version, ctx.cwd, dir),
    bdkCli: ctx.executable ?? "",
  });
  const written: string[] = [];
  const kept: string[] = [];
  for (const [path, text] of [...files].sort(([a], [b]) => (a < b ? -1 : 1))) {
    const file = join(dir, path);
    if (await exists(file)) {
      kept.push(path);
      continue;
    }
    await mkdir(dirname(file), { recursive: true });
    await writeFile(file, text);
    if (path.endsWith(".sh")) await chmod(file, 0o755);
    written.push(path);
  }
  for (const path of kept) {
    diagnostics.push({
      code: "init.file_kept",
      severity: "warning",
      message: `${path} already exists and was not overwritten.`,
      file: join(dir, path),
      fix: "Compare it with what `bdk init` writes into an empty directory.",
    });
  }
  if (ctx.executable === undefined) {
    diagnostics.push({
      code: "init.cli_unset",
      severity: "note",
      message:
        "This bdk is not the packaged executable, so the Xcode project's BDK_CLI build setting is empty.",
      fix: "Set BDK_CLI to the path of a packaged bdk (`pnpm cli:package` writes packages/cli/dist/bdk).",
    });
  }

  if (answers.git) {
    const result = await ctx.exec("git", ["init", "--quiet"], { cwd: dir });
    if (result.code !== 0) {
      diagnostics.push({
        code: "init.git_failed",
        severity: "warning",
        message: `\`git init\` failed: ${result.stderr.trim() || `exit ${result.code}`}`,
        fix: "Run `git init` in the project yourself.",
      });
    }
  }
  // Vite pages install in Pages; the automation host's Rolldown in the project itself.
  const installs = [
    ...(answers.pages === "vite" ? [join(dir, "Pages")] : []),
    ...(answers.automation ? [dir] : []),
  ];
  const manager = answers.packageManager;
  if (answers.install && manager !== undefined) {
    for (const where of installs) {
      if (args.offline) {
        diagnostics.push({
          code: "init.install_skipped",
          severity: "warning",
          message: `--offline: the packages in ${where} were not installed.`,
          fix: `Run \`${manager} install\` in ${where}.`,
        });
        continue;
      }
      const result = await ctx.exec(manager, ["install"], { cwd: where });
      if (result.code !== 0) {
        diagnostics.push({
          code: "init.install_failed",
          severity: "warning",
          message: `\`${manager} install\` failed in ${where}: ${result.stderr.trim() || `exit ${result.code}`}`,
          fix: `Run \`${manager} install\` in ${where} and fix what it reports.`,
        });
      }
    }
  }

  const replay = replayCommand(
    answers,
    flags["bdk-package"] === undefined ? {} : { bdkPackage: flags["bdk-package"] },
  );
  if (!args.json) {
    const shown = relative(ctx.cwd, dir) || ".";
    ctx.stdout(
      `Created ${answers.name} in ${shown} (${written.length} files).\n` +
        `Open it with: open ${join(shown, `${productName(answers.name)}.xcodeproj`)}\n` +
        `The same project, with no questions:\n  ${replay}\n`,
    );
  }
  return {
    exitCode: ExitCode.Success,
    diagnostics,
    data: { directory: dir, answers, files: written, kept, replay },
  };
}
