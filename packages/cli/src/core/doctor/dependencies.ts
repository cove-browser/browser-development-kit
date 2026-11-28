import { readFile } from "node:fs/promises";
import { join } from "node:path";
import { SUPPORTED_DEPENDENCIES, type VersionRange } from "@bdk/vite/tooling";
import { isRecord } from "../config/reporter.js";
import { pagesDirectory } from "../pages/routes.js";
import type { Project } from "../project.js";
import type { Findings } from "./findings.js";
import { compareVersions } from "./requirements.js";

/** The version a `package.json` specifier names: `8.3.8`, `^8.3.8`, `~8.3`; undefined for a tag, a path or a URL. */
export function pinnedVersion(specifier: string): string | undefined {
  return /^[\^~=v]*\s*(\d+(?:\.\d+){0,2})$/.exec(specifier.trim())?.[1];
}

/** Whether `version` is in the range: from `from`, up to but not including `before`. */
export function inRange(version: string, range: VersionRange): boolean {
  return compareVersions(version, range.from) >= 0 && compareVersions(version, range.before) < 0;
}

/**
 * The pages' `package.json` (written by `bdk init`): each dependency the Vite preset declares a supported
 * range for, pinned to a version outside it.
 */
export async function viteDependencyFindings(f: Findings, project: Project): Promise<void> {
  const file = join(project.dir, await pagesDirectory(project.configFile), "package.json");
  let parsed: unknown;
  try {
    parsed = JSON.parse(await readFile(file, "utf8"));
  } catch {
    return;
  }
  if (!isRecord(parsed)) return;
  for (const field of ["dependencies", "devDependencies"] as const) {
    const declared = isRecord(parsed[field]) ? parsed[field] : {};
    for (const [name, specifier] of Object.entries(declared)) {
      const range = SUPPORTED_DEPENDENCIES[name];
      if (range === undefined || typeof specifier !== "string") continue;
      const version = pinnedVersion(specifier);
      if (version === undefined || inRange(version, range)) continue;
      f.warning(
        "doctor.vite_dependency_unsupported",
        `The pages pin ${name} ${specifier}, which BDK's Vite preset does not support: it is built and checked with ${range.from} up to, not including, ${range.before}.`,
        `Pin ${name} to a version from ${range.from} up to, not including, ${range.before}, and install again.`,
        file,
      );
    }
  }
}
