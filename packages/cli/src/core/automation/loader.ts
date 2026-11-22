import { createRequire } from "node:module";
import { join } from "node:path";
import { pathToFileURL } from "node:url";
import type { Bundler } from "./bundler.js";

/**
 * Loads Rolldown from the project, else from the CLI's own dependency (running from the repository).
 * The packaged `bdk` cannot carry Rolldown's native binding, so it never bundles it. Resolves to
 * `undefined` when neither has it.
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
    return (await import(pathToFileURL(file).href)) as Bundler;
  }
  return undefined;
}
