import { readdirSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

/** Asset names under this prefix are the built `@bdk/internal` modules (`js/internal/settings.js`). */
export const INTERNAL_MODULES_PREFIX = "js/internal/";

/** The embedded list of every asset name. `node:sea` can read an asset by name but cannot list them. */
export const ASSET_INDEX = "asset-index.json";

/** Where unpackaged runs and `cli:package` find the built `@bdk/internal` modules. */
export function internalModulesDir(): string {
  return resolve(dirname(fileURLToPath(import.meta.url)), "..", "..", "js", "internal", "dist");
}

/** The JavaScript modules in `dir` (not declarations, not tests, not subdirectories), sorted; none if it does not exist. */
export function listInternalModules(dir: string = internalModulesDir()): string[] {
  try {
    return readdirSync(dir, { withFileTypes: true })
      .filter((e) => e.isFile() && e.name.endsWith(".js") && !e.name.endsWith(".test.js"))
      .map((e) => e.name)
      .sort();
  } catch (error) {
    if (typeof error === "object" && error !== null && "code" in error && error.code === "ENOENT") {
      return [];
    }
    throw error;
  }
}
