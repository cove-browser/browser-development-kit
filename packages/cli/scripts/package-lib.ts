import { readdirSync } from "node:fs";
import { join, relative, sep } from "node:path";

/** Files under `assets/` that are documentation, not assets. */
const NOT_ASSETS = new Set(["README.md"]);

/** Every file under `dir` as `{ name: absolute path }`, the name being the `/`-separated relative path. */
export function listAssets(dir: string): Record<string, string> {
  const assets: Record<string, string> = {};
  for (const entry of readdirSync(dir, { recursive: true, withFileTypes: true })) {
    if (!entry.isFile()) continue;
    const file = join(entry.parentPath, entry.name);
    const name = relative(dir, file).split(sep).join("/");
    if (NOT_ASSETS.has(name)) continue;
    assets[name] = file;
  }
  return Object.fromEntries(Object.entries(assets).sort(([a], [b]) => (a < b ? -1 : 1)));
}

/** The built `@bdk/internal` modules as assets, named under `prefix`: `{ "js/internal/settings.js": "<dir>/settings.js" }`. */
export function internalAssets(
  dir: string,
  modules: readonly string[],
  prefix: string,
): Record<string, string> {
  return Object.fromEntries(modules.map((name) => [`${prefix}${name}`, join(dir, name)]));
}

export interface SeaConfigInput {
  main: string;
  output: string;
  assets: Record<string, string>;
  /** Code cache is tied to the Node build that makes it, so it is on only for host builds. */
  useCodeCache: boolean;
}

/** The `node --build-sea` configuration file contents. */
export function seaConfig(input: SeaConfigInput): Record<string, unknown> {
  return {
    main: input.main,
    output: input.output,
    mainFormat: "commonjs",
    disableExperimentalSEAWarning: true,
    useCodeCache: input.useCodeCache,
    assets: input.assets,
  };
}

export interface ArtifactBundleInput {
  version: string;
  /** Directory inside the bundle that holds the executable, relative to the bundle root. */
  variantDir: string;
  triple: string;
}

/** `info.json` of a Swift package artifact bundle with one executable artifact, `bdk`. */
export function artifactBundleInfo(input: ArtifactBundleInput): Record<string, unknown> {
  return {
    schemaVersion: "1.0",
    artifacts: {
      bdk: {
        type: "executable",
        version: input.version,
        variants: [{ path: `${input.variantDir}/bdk`, supportedTriples: [input.triple] }],
      },
    },
  };
}
