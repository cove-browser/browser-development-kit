import { readFileSync } from "node:fs";
import { dirname, join, resolve, sep } from "node:path";
import { getAsset, isSea } from "node:sea";
import { fileURLToPath } from "node:url";

/** Embedded files (templates, schemas) by name, relative to `packages/cli/assets/`. */
export interface Assets {
  /** The asset as UTF-8 text. Throws `AssetNotFoundError` when there is none by that name. */
  readText(name: string): string;
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

function isMissing(error: unknown): boolean {
  return typeof error === "object" && error !== null && "code" in error && error.code === "ENOENT";
}

/**
 * The one adapter for embedded assets. As a single executable it reads named assets through
 * `node:sea`; unpackaged it reads the same files from `assetsDir`. No other file may import `node:sea`
 * (sdk/cli.md section 2.3, risk R31).
 */
export function createAssets(options: { packaged?: boolean; assetsDir?: string } = {}): Assets {
  const packaged = options.packaged ?? isSea();
  return {
    readText(name) {
      if (packaged) {
        try {
          return getAsset(name, "utf8");
        } catch {
          throw new AssetNotFoundError(name);
        }
      }
      const root = options.assetsDir ?? unpackagedAssetsDir();
      const file = resolve(join(root, name));
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
