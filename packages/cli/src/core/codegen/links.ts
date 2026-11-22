import { lstat, mkdir, readlink, rm, symlink, writeFile } from "node:fs/promises";
import { join } from "node:path";
import type { Diagnostic } from "../diagnostics.js";

export interface LinkResult {
  /** The link, `<directory>/node_modules/@bdk`. */
  link: string;
  /** True when this call created or repointed it. */
  changed: boolean;
  /** Set when the link could not be made. */
  diagnostic?: Diagnostic;
}

async function lstatOrUndefined(path: string) {
  try {
    return await lstat(path);
  } catch (error) {
    if (typeof error === "object" && error !== null && "code" in error && error.code === "ENOENT") {
      return undefined;
    }
    throw error;
  }
}

/**
 * Points `<directory>/node_modules/@bdk` at `target` (javascript-api.md section 7, "Editor support"). An
 * existing link is refreshed. A real file or directory is never replaced: the result carries a warning.
 * A `node_modules` this call creates gets a `.gitignore` that ignores it, so the link is never committed.
 */
export async function linkScope(directory: string, target: string): Promise<LinkResult> {
  const modules = join(directory, "node_modules");
  const link = join(modules, "@bdk");
  const existing = await lstatOrUndefined(link);
  if (existing?.isSymbolicLink()) {
    if ((await readlink(link)) === target) return { link, changed: false };
    await rm(link);
  } else if (existing !== undefined) {
    return {
      link,
      changed: false,
      diagnostic: {
        code: "codegen.link_blocked",
        severity: "warning",
        message: `${link} is a real ${existing.isDirectory() ? "directory" : "file"}, so it was not replaced with a link to the generated @bdk packages. Editors will not see them.`,
        file: link,
        fix: `Remove ${link}, then run \`bdk build codegen\` again. Do not install @bdk packages with a package manager.`,
      },
    };
  }
  const createdModules = (await lstatOrUndefined(modules)) === undefined;
  await mkdir(modules, { recursive: true });
  if (createdModules) await writeFile(join(modules, ".gitignore"), "*\n");
  await symlink(target, link, "dir");
  return { link, changed: true };
}
