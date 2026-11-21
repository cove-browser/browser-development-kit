import { readdirSync, readFileSync } from "node:fs";
import { dirname, join, relative, resolve, sep } from "node:path";
import { getAsset, isSea } from "node:sea";
import { fileURLToPath } from "node:url";
import {
  ASSET_INDEX,
  INTERNAL_MODULES_PREFIX,
  internalModulesDir,
  listInternalModules,
  SDK_PREFIX,
  sdkAssets,
  sdkDirs,
} from "./asset-sources.js";

/** Embedded files (templates, schemas) by name, relative to `packages/cli/assets/`. */
export interface Assets {
  /** The asset as UTF-8 text. Throws `AssetNotFoundError` when there is none by that name. */
  readText(name: string): string;
  /** The names of the assets that start with `prefix`, sorted. */
  list(prefix: string): string[];
}

export class AssetNotFoundError extends Error {
  readonly assetName: string;

  constructor(name: string) {
    super(`The embedded asset "${name}" does not exist.`);
    this.name = "AssetNotFoundError";
    this.assetName = name;
  }
}

/** Where unpackaged runs (development, tests) find the same files that `cli:package` embeds. */
export function unpackagedAssetsDir(): string {
  return resolve(dirname(fileURLToPath(import.meta.url)), "..", "assets");
}

/** Every file under `dir` by `/`-separated relative name, except the README; none if `dir` does not exist. */
function listFiles(dir: string): string[] {
  try {
    return readdirSync(dir, { recursive: true, withFileTypes: true })
      .filter((e) => e.isFile())
      .map((e) => relative(dir, join(e.parentPath, e.name)).split(sep).join("/"))
      .filter((name) => name !== "README.md");
  } catch (error) {
    if (isMissing(error)) return [];
    throw error;
  }
}

function isMissing(error: unknown): boolean {
  return typeof error === "object" && error !== null && "code" in error && error.code === "ENOENT";
}

/**
 * The one adapter for embedded assets. As a single executable it reads named assets through
 * `node:sea`; unpackaged it reads the same files from `assetsDir`, and the `js/internal/` names from the
 * built `@bdk/internal` package (`asset-sources.ts`). No other file may import `node:sea`
 * (sdk/cli.md section 2.3, risk R31).
 */
export function createAssets(
  options: {
    packaged?: boolean;
    assetsDir?: string;
    internalDir?: string;
    /** Build output of each SDK package by name; default `packages/js/<package>/dist`. */
    sdkDirs?: Record<string, string>;
  } = {},
): Assets {
  const packaged = options.packaged ?? isSea();
  const internalDir = options.internalDir ?? internalModulesDir();
  const sdk = (): Record<string, string> => sdkAssets(options.sdkDirs ?? sdkDirs());
  return {
    list(prefix) {
      let names: string[];
      if (packaged) {
        try {
          names = JSON.parse(getAsset(ASSET_INDEX, "utf8"));
        } catch {
          names = [];
        }
      } else {
        names = [
          ...listInternalModules(internalDir).map((name) => `${INTERNAL_MODULES_PREFIX}${name}`),
          ...Object.keys(sdk()),
          ...listFiles(options.assetsDir ?? unpackagedAssetsDir()),
        ];
      }
      return names.filter((name) => name.startsWith(prefix)).sort();
    },
    readText(name) {
      if (packaged) {
        try {
          return getAsset(name, "utf8");
        } catch {
          throw new AssetNotFoundError(name);
        }
      }
      if (name.startsWith(SDK_PREFIX)) {
        const file = sdk()[name];
        if (file === undefined) throw new AssetNotFoundError(name);
        return readFileSync(file, "utf8");
      }
      const internal = name.startsWith(INTERNAL_MODULES_PREFIX);
      const root = internal ? internalDir : (options.assetsDir ?? unpackagedAssetsDir());
      const file = resolve(
        join(root, internal ? name.slice(INTERNAL_MODULES_PREFIX.length) : name),
      );
      if (!file.startsWith(`${resolve(root)}${sep}`)) throw new AssetNotFoundError(name);
      try {
        return readFileSync(file, "utf8");
      } catch (error) {
        if (isMissing(error)) throw new AssetNotFoundError(name);
        throw error;
      }
    },
  };
}
