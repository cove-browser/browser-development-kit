import { readFile } from "node:fs/promises";
import { join } from "node:path";
import { parse } from "smol-toml";
import { normaliseProjectPath } from "../config/paths.js";
import { CliError, ExitCode } from "../exit-codes.js";

export interface BundledRoute {
  route: string;
  /** Project-relative, `/`-separated. */
  path: string;
}

function resolvedInvalid(file: string, message: string): CliError {
  return new CliError(
    {
      code: "pages.resolved_invalid",
      severity: "error",
      message: `${file} ${message}`,
      file,
      fix: "Run `bdk build config` again to write it.",
    },
    ExitCode.ProjectFailure,
  );
}

/** The `mode: "bundled"` routes of a resolved file. Throws `pages.resolved_missing` or `pages.resolved_invalid`. */
export async function readBundledRoutes(file: string): Promise<BundledRoute[]> {
  let text: string;
  try {
    text = await readFile(file, "utf8");
  } catch (error) {
    if (typeof error === "object" && error !== null && "code" in error && error.code === "ENOENT") {
      throw new CliError(
        {
          code: "pages.resolved_missing",
          severity: "error",
          message: `${file} does not exist, so the pages to build are not known.`,
          file,
          fix: "Run `bdk build config` first, or pass --resolved <file>.",
        },
        ExitCode.ProjectFailure,
      );
    }
    throw error;
  }
  let routes: unknown;
  try {
    routes = (JSON.parse(text) as { routes?: unknown }).routes;
  } catch {
    throw resolvedInvalid(file, "is not valid JSON.");
  }
  if (typeof routes !== "object" || routes === null) {
    throw resolvedInvalid(file, "has no `routes` table.");
  }
  const bundled: BundledRoute[] = [];
  for (const [route, entry] of Object.entries(routes)) {
    if (typeof entry !== "object" || entry === null || !("mode" in entry)) continue;
    if (entry.mode !== "bundled") continue;
    const path =
      "path" in entry && typeof entry.path === "string" ? normaliseProjectPath(entry.path) : "";
    if (path === "" || entry.path !== path) {
      throw resolvedInvalid(file, `holds the bundled route "${route}" with no usable \`path\`.`);
    }
    bundled.push({ route, path });
  }
  return bundled;
}

/** A directory named by `[<table>] directory` in `bdk.toml`, project-relative, or `fallback` when it names none. */
async function configuredDirectory(
  configFile: string,
  table: string,
  fallback: string,
): Promise<string> {
  try {
    const section = parse(await readFile(configFile, "utf8"))[table];
    if (typeof section === "object" && section !== null && "directory" in section) {
      if (typeof section.directory === "string") return normaliseProjectPath(section.directory);
    }
  } catch {
    // `bdk build config` reports an unreadable bdk.toml; the default is right for everything else.
  }
  return fallback;
}

/** The directory that holds the pages, from `[pages] directory` in `bdk.toml` (default `Pages`). */
export const pagesDirectory = (configFile: string): Promise<string> =>
  configuredDirectory(configFile, "pages", "Pages");

/** The directory that holds the automation scripts, from `[automation] directory` (default `Automation`). */
export const automationDirectory = (configFile: string): Promise<string> =>
  configuredDirectory(configFile, "automation", "Automation");

/** The application bundle an Xcode build is making, from the variables Xcode sets. */
export function bundleFolder(env: NodeJS.ProcessEnv): string | undefined {
  if (env.CODESIGNING_FOLDER_PATH) return env.CODESIGNING_FOLDER_PATH;
  if (env.TARGET_BUILD_DIR && env.WRAPPER_NAME) return join(env.TARGET_BUILD_DIR, env.WRAPPER_NAME);
  return undefined;
}
