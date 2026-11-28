/**
 * Links inside a directory the build copies or hashes: a page, a bundled extension. Hashing follows
 * links, so a directory is checked here before its digest is taken.
 */
import { existsSync, lstatSync, readdirSync, realpathSync, statSync } from "node:fs";
import { join, relative, sep } from "node:path";

/** Whether `absolute` resolves, through symlinks, to a place inside the project. */
export function isInsideProject(projectDir: string, absolute: string): boolean {
  const root = realpathSync(projectDir);
  const real = realpathSync(absolute);
  return real === root || real.startsWith(root + sep);
}

export type LinkProblem = { path: string; kind: "outside" | "broken" | "loop" };

function byCodeUnit(a: string, b: string): number {
  return a < b ? -1 : a > b ? 1 : 0;
}

function isDirectory(path: string): boolean {
  return existsSync(path) && statSync(path).isDirectory();
}

/**
 * The first link under `directory` that cannot be carried: one that leaves `boundary`, points nowhere,
 * or leads back to a directory it is already inside. `directory` is `boundary` or inside it, and was
 * checked by the caller; the path reported is relative to `boundary`.
 */
export function nestedLinkProblem(boundary: string, directory: string): LinkProblem | undefined {
  const walk = (current: string, ancestors: Set<string>): LinkProblem | undefined => {
    for (const name of readdirSync(current).sort(byCodeUnit)) {
      const absolute = join(current, name);
      const child = relative(boundary, absolute).split(sep).join("/");
      if (lstatSync(absolute).isSymbolicLink()) {
        if (!existsSync(absolute)) return { path: child, kind: "broken" };
        if (!isInsideProject(boundary, absolute)) return { path: child, kind: "outside" };
      }
      if (!isDirectory(absolute)) continue;
      const real = realpathSync(absolute);
      if (ancestors.has(real)) return { path: child, kind: "loop" };
      const found = walk(absolute, new Set([...ancestors, real]));
      if (found) return found;
    }
    return undefined;
  };
  // The directory is inside the boundary and each directory between them.
  const segments = relative(boundary, directory)
    .split(sep)
    .filter((s) => s !== "");
  const above = segments.map((_, i) => realpathSync(join(boundary, ...segments.slice(0, i + 1))));
  return walk(directory, new Set([realpathSync(boundary), ...above]));
}
