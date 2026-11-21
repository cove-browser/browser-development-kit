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

/**
 * Asset names under this prefix are the SDK packages `bdk build codegen` writes for editors and `tsc`:
 * `js/sdk/@bdk/<package>/<file>`, the built `.js` and `.d.ts` files of each package in `SDK_PACKAGES`.
 */
export const SDK_PREFIX = "js/sdk/";

/** The packages of `packages/js` that `bdk build codegen` writes into `<out>/@bdk/`: npm name and directory. */
export const SDK_PACKAGES: readonly { name: string; directory: string }[] = [
  { name: "@bdk/internal", directory: "internal" },
  { name: "@bdk/protocol", directory: "protocol" },
  { name: "@bdk/vite", directory: "vite" },
];

/** Where each SDK package's build output is, by package name. */
export function sdkDirs(): Record<string, string> {
  const root = resolve(dirname(fileURLToPath(import.meta.url)), "..", "..", "js");
  return Object.fromEntries(SDK_PACKAGES.map((p) => [p.name, resolve(root, p.directory, "dist")]));
}

/** The `.js` and `.d.ts` files in `dir` (not tests, not subdirectories), sorted; none if it does not exist. */
function listPackageFiles(dir: string): string[] {
  try {
    return readdirSync(dir, { withFileTypes: true })
      .filter((e) => e.isFile() && /\.(js|d\.ts)$/.test(e.name) && !e.name.includes(".test."))
      .map((e) => e.name)
      .sort();
  } catch (error) {
    if (typeof error === "object" && error !== null && "code" in error && error.code === "ENOENT") {
      return [];
    }
    throw error;
  }
}

/** Every SDK file as asset name to absolute path, sorted by name. */
export function sdkAssets(dirs: Record<string, string> = sdkDirs()): Record<string, string> {
  const assets: Record<string, string> = {};
  for (const [name, dir] of Object.entries(dirs)) {
    for (const file of listPackageFiles(dir))
      assets[`${SDK_PREFIX}${name}/${file}`] = resolve(dir, file);
  }
  return Object.fromEntries(Object.entries(assets).sort(([a], [b]) => (a < b ? -1 : 1)));
}
