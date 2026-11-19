import { stat } from "node:fs/promises";
import { dirname, join, resolve } from "node:path";
import { CliError, ExitCode } from "./exit-codes.js";

export const CONFIG_FILE = "bdk.toml";

export interface Project {
  /** Absolute path of the directory that holds `bdk.toml`. */
  dir: string;
  /** Absolute path of `bdk.toml`. */
  configFile: string;
}

async function isFile(path: string): Promise<boolean> {
  try {
    return (await stat(path)).isFile();
  } catch {
    return false;
  }
}

/**
 * The project for this run: `--project <dir>` when given (it must hold `bdk.toml`), otherwise the
 * nearest ancestor of `cwd` that does. Throws `config.missing` (exit 1) when there is none.
 */
export async function findProject(cwd: string, explicit?: string): Promise<Project> {
  if (explicit !== undefined) {
    const dir = resolve(cwd, explicit);
    const configFile = join(dir, CONFIG_FILE);
    if (await isFile(configFile)) return { dir, configFile };
    throw new CliError(
      {
        code: "config.missing",
        severity: "error",
        message: `No ${CONFIG_FILE} in ${dir} (from --project).`,
        file: configFile,
        fix: `Pass the directory that contains ${CONFIG_FILE}, or create one with \`bdk init\`.`,
      },
      ExitCode.ProjectFailure,
    );
  }
  let dir = resolve(cwd);
  for (;;) {
    const configFile = join(dir, CONFIG_FILE);
    if (await isFile(configFile)) return { dir, configFile };
    const parent = dirname(dir);
    if (parent === dir) break;
    dir = parent;
  }
  throw new CliError(
    {
      code: "config.missing",
      severity: "error",
      message: `No ${CONFIG_FILE} in ${resolve(cwd)} or any parent directory.`,
      fix: "Run from inside a BDK project, pass --project <dir>, or create one with `bdk init`.",
    },
    ExitCode.ProjectFailure,
  );
}
