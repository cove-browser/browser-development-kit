import { createRequire } from "node:module";
import { join } from "node:path";
import type { Bundler } from "./bundler.js";

/**
 * Loads Rolldown from the project, else from the CLI's own dependency (running from the repository).
 * The packaged `bdk` cannot carry Rolldown's native binding, so it never bundles it. Resolves to
 * `undefined` when neither has it. `require`, not `import()`, as for Vite (pages/vite.ts): Rolldown is an
 * ES module, which Node's `require` loads, and inside the single executable `import()` of a file is not
 * available (only built-in modules are).
 */
export async function loadBundler(projectDir: string): Promise<Bundler | undefined> {
  const requirers = [
    createRequire(join(projectDir, "package.json")),
    createRequire(import.meta.url),
  ];
  for (const requirer of requirers) {
    let file: string;
    try {
      file = requirer.resolve("rolldown");
    } catch {
      continue;
    }
    return requirer(file) as Bundler;
  }
  return undefined;
}
