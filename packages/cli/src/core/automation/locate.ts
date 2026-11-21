import { existsSync } from "node:fs";
import { createRequire } from "node:module";
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

/** Where the `@bdk/automation` package is: the project's own copy, else the repository's. */
function packageDirectory(projectDir: string): string | undefined {
  try {
    const manifest = createRequire(join(projectDir, "package.json")).resolve(
      `${PACKAGE}/package.json`,
    );
    return dirname(manifest);
  } catch {
    // Not installed in the project; running from the repository is the other place it can be.
  }
  let dir = dirname(fileURLToPath(import.meta.url));
  for (;;) {
    const candidate = join(dir, "packages", "js", "automation");
    if (existsSync(join(candidate, "package.json"))) return candidate;
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
