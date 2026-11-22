import { existsSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const PACKAGE = "@bdk/automation";
/** Entry file (without extension) for each specifier the host provides no module for. */
const ENTRIES: Readonly<Record<string, string>> = {
  "@bdk/automation": "index",
  "@bdk/automation/client": "client",
  "@bdk/automation/commands": "commands",
  "@bdk/automation/events": "events",
  "@bdk/commands": "commands",
  "@bdk/events": "events",
};

/**
 * Where the `@bdk/automation` package is: the project's own copy, else the repository's.
 *
 * The project's copy is found by walking `node_modules` directories up from the project, as Node
 * does, rather than with `require.resolve`: the package's `exports` map does not list
 * `package.json`, and resolving an entry instead would need the package to be built.
 */
function packageDirectory(projectDir: string): string | undefined {
  const installed = upwards(resolve(projectDir), (dir) => join(dir, "node_modules", PACKAGE));
  if (installed !== undefined) return installed;
  return upwards(dirname(fileURLToPath(import.meta.url)), (dir) =>
    join(dir, "packages", "js", "automation"),
  );
}

/** The first `candidate(dir)` holding a `package.json`, from `start` up to the filesystem root. */
function upwards(start: string, candidate: (dir: string) => string): string | undefined {
  let dir = start;
  for (;;) {
    const found = candidate(dir);
    if (existsSync(join(found, "package.json"))) return found;
    const parent = dirname(dir);
    if (parent === dir) return undefined;
    dir = parent;
  }
}

/**
 * The aliases that inline `@bdk/automation` into a script (`@bdk/commands` and `@bdk/events` are
 * short names for its `commands` and `events` entries). An entry the package does not have is left
 * out, so importing it reports an unresolved import.
 */
export function automationAliases(projectDir: string): Map<string, string> {
  const aliases = new Map<string, string>();
  const root = packageDirectory(projectDir);
  if (root === undefined) return aliases;
  for (const [specifier, entry] of Object.entries(ENTRIES)) {
    const file = [`src/${entry}.ts`, `dist/${entry}.js`]
      .map((relative) => resolve(root, relative))
      .find((candidate) => existsSync(candidate));
    if (file !== undefined) aliases.set(specifier, file);
  }
  return aliases;
}
