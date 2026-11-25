import { type Dirent, readdirSync } from "node:fs";
import { dirname, join, relative, resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";
import { readAbi } from "@bdk/tooling/dist/runtime/abi.js";
import { type RuntimeRecord, readPin } from "@bdk/tooling/dist/runtime/pin.js";

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
  { name: "@bdk/automation", directory: "automation" },
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

/** The repository root, from `packages/cli/src`. */
export function repositoryRoot(): string {
  return resolve(dirname(fileURLToPath(import.meta.url)), "..", "..", "..");
}

/**
 * The embedded facts about this BDK release that `bdk doctor` and `bdk patch` need: the Chromium pin,
 * the ABI the SDK was compiled against, and the published runtime record (`null` while there is none).
 * Written from `chromium/pin.json` and `packages/core` when packaging; computed from them unpackaged.
 */
export const RELEASE_ASSET = "bdk-release.json";

export interface ReleaseFacts {
  chromium: { tag: string; commit: string };
  abi: { version: number; hash: string };
  runtime: RuntimeRecord | null;
}

export function releaseFacts(root: string = repositoryRoot()): ReleaseFacts {
  const pin = readPin(root);
  return {
    chromium: { tag: pin.tag, commit: pin.commit },
    abi: readAbi(root),
    runtime: pin.runtime,
  };
}

/** Repository directories embedded under a prefix: BDK's patch series and the `bdk init` templates. */
export const REPOSITORY_ASSET_DIRS: readonly { prefix: string; dir: string }[] = [
  { prefix: "chromium/patches/", dir: "chromium/patches" },
  { prefix: "templates/swift-browser/", dir: "apps/templates/swift-browser" },
  { prefix: "templates/pages/", dir: "apps/templates/pages" },
];

/** Files at the top of those directories that describe them and are not part of what they carry. */
const NOT_REPOSITORY_ASSETS = new Set(["README.md", "project.json"]);

/** Every file of `REPOSITORY_ASSET_DIRS` as asset name to absolute path, sorted by name. */
export function repositoryAssets(root: string = repositoryRoot()): Record<string, string> {
  const assets: Record<string, string> = {};
  for (const { prefix, dir } of REPOSITORY_ASSET_DIRS) {
    const base = resolve(root, dir);
    let entries: Dirent[];
    try {
      entries = readdirSync(base, { recursive: true, withFileTypes: true });
    } catch (error) {
      if (typeof error === "object" && error !== null && "code" in error && error.code === "ENOENT")
        continue;
      throw error;
    }
    for (const entry of entries) {
      if (!entry.isFile() || entry.name === ".DS_Store") continue;
      const name = relative(base, join(entry.parentPath, entry.name)).split(sep).join("/");
      if (NOT_REPOSITORY_ASSETS.has(name)) continue;
      // What a development build of a template leaves behind is not part of it.
      if (/(^|\/)(node_modules|\.build|build|dist|xcuserdata)\//.test(name)) continue;
      assets[`${prefix}${name}`] = join(base, name);
    }
  }
  return Object.fromEntries(Object.entries(assets).sort(([a], [b]) => (a < b ? -1 : 1)));
}
