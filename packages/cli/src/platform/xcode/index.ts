import { execFile } from "node:child_process";
import { stat } from "node:fs/promises";
import { resolve } from "node:path";
import { promisify } from "node:util";
import type { BundleFacts } from "../../core/config/index.js";
import { CliError, ExitCode } from "../../core/exit-codes.js";
import type { Platform } from "../platform.js";

const execFileAsync = promisify(execFile);

/** Converts a property list file to its JSON text. */
export type PlistToJson = (file: string) => Promise<string>;

/** `plutil -convert json -o - <file>`: an Apple tool run as a subprocess, no parser needed. */
export const plutilToJson: PlistToJson = async (file) => {
  const { stdout } = await execFileAsync("plutil", ["-convert", "json", "-o", "-", file]);
  return stdout;
};

async function exists(path: string): Promise<boolean> {
  try {
    return (await stat(path)).isFile();
  } catch {
    return false;
  }
}

async function readPlist(file: string, toJson: PlistToJson): Promise<Record<string, unknown>> {
  let text: string;
  try {
    text = await toJson(file);
  } catch (error) {
    if (typeof error === "object" && error !== null && "code" in error && error.code === "ENOENT") {
      throw new CliError(
        {
          code: "cli.tool_missing",
          severity: "error",
          message: "`plutil` was not found, so the property list cannot be read.",
          file,
          fix: "Run on macOS with the command line tools installed (`xcode-select --install`).",
        },
        ExitCode.Environment,
      );
    }
    throw new CliError(
      {
        code: "config.plist_unreadable",
        severity: "error",
        message: `${file} is not a valid property list.`,
        file,
        fix: "Open the file in Xcode, or run `plutil -lint` on it, and fix what it reports.",
      },
      ExitCode.ProjectFailure,
    );
  }
  const parsed: unknown = JSON.parse(text);
  if (typeof parsed !== "object" || parsed === null || Array.isArray(parsed)) {
    throw new CliError(
      {
        code: "config.plist_unreadable",
        severity: "error",
        message: `${file} does not hold a dictionary at its top level.`,
        file,
        fix: "A property list for Info.plist or entitlements must be a dictionary.",
      },
      ExitCode.ProjectFailure,
    );
  }
  return parsed as Record<string, unknown>;
}

/** The macOS platform module: the Xcode build environment and the property lists it names. */
export function createXcodePlatform(toJson: PlistToJson = plutilToJson): Platform {
  return {
    name: "xcode",
    isIdeBuild: (env) => env.XCODE_VERSION_ACTUAL !== undefined,
    async bundleFacts(env, projectDir) {
      const base = env.SRCROOT ?? projectDir;
      const facts: BundleFacts = {};
      const infoPlist = env.INFOPLIST_FILE;
      if (infoPlist) {
        const file = resolve(base, infoPlist);
        if (await exists(file)) facts.infoPlist = await readPlist(file, toJson);
      }
      const entitlements = env.CODE_SIGN_ENTITLEMENTS;
      if (entitlements) {
        const file = resolve(base, entitlements);
        if (await exists(file)) facts.entitlements = await readPlist(file, toJson);
      }
      return facts.infoPlist === undefined && facts.entitlements === undefined ? undefined : facts;
    },
  };
}

export const xcodePlatform: Platform = createXcodePlatform();
