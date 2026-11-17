import { existsSync } from "node:fs";
import { mkdir, readFile, rename, rm, symlink, writeFile } from "node:fs/promises";
import { join } from "node:path";
import type { BdkPaths } from "../paths.js";
import { extractArchive } from "./archive.js";
import { type Downloader, download } from "./backends.js";
import { sha256File } from "./canonical.js";
import { computeSourceIdentity, sourceKey, type Target } from "./identity.js";
import { type VerifiedRuntime, verifyRuntimeDir } from "./manifest.js";
import { type RuntimeRecord, readPin } from "./pin.js";
import { allocTempDir, installRuntimeDir, removeTree } from "./store.js";
import { makeReadOnly } from "./tree.js";

export type RuntimeMode = "prebuilt" | "source";

export class RuntimeResolveError extends Error {
  override readonly name = "RuntimeResolveError";
  constructor(
    message: string,
    readonly code: "missing" | "integrity" | "download" | "digest",
  ) {
    super(message);
  }
}

export interface ResolveOptions {
  readonly repoRoot: string;
  readonly paths: BdkPaths;
  readonly config?: string;
  readonly target?: Target;
  /** `BDK_RUNTIME_MODE`. `source` never downloads: only a locally built and packaged runtime is used. */
  readonly mode?: RuntimeMode;
  /** `--allow-source-build`: with no published runtime in pin.json, point at the source build. */
  readonly allowSourceBuild?: boolean;
  /** Injectable for tests. */
  readonly downloader?: Downloader;
}

export interface ResolveResult {
  readonly sourceKey: string;
  readonly runtimeDir: string;
  readonly manifestSha256: string;
  readonly origin: "store" | "downloaded";
}

export function modeFromEnv(env: Readonly<Record<string, string | undefined>>): RuntimeMode {
  const v = env.BDK_RUNTIME_MODE;
  if (v === undefined || v === "" || v === "prebuilt") return "prebuilt";
  if (v === "source") return "source";
  throw new RuntimeResolveError(
    `BDK_RUNTIME_MODE must be "prebuilt" or "source", got "${v}".`,
    "missing",
  );
}

function missingMessage(
  key: string,
  paths: BdkPaths,
  record: RuntimeRecord | null,
  opts: ResolveOptions,
): string {
  const lines = [`No runtime for source key ${key} in ${paths.runtimes}.`];
  if (opts.mode === "source") {
    lines.push(
      "BDK_RUNTIME_MODE=source: only a locally built runtime is used. Build and package one:",
    );
    lines.push("  pnpm chromium:build && pnpm chromium:package");
  } else if (record === null) {
    lines.push("chromium/pin.json records no published runtime.");
    lines.push("Either:");
    lines.push(
      "  - build one locally: BDK_RUNTIME_MODE=source pnpm chromium:build && pnpm chromium:package",
    );
    lines.push("  - or check out a commit that has a published runtime.");
    if (opts.allowSourceBuild) {
      lines.push(
        "(--allow-source-build was given: run the source build above, then resolve again.)",
      );
    }
  } else {
    lines.push(
      `chromium/pin.json records a published runtime for source key ${record.source_key}, not ${key}: ` +
        "the patches, overlay, GN arguments, Chromium pin or ABI changed since it was published.",
    );
    lines.push("Either:");
    lines.push(
      "  - build one locally: BDK_RUNTIME_MODE=source pnpm chromium:build && pnpm chromium:package",
    );
    lines.push("  - or check out a commit that has a published runtime.");
  }
  return lines.join("\n");
}

async function fetchRuntime(
  key: string,
  record: RuntimeRecord,
  paths: BdkPaths,
  downloader: Downloader,
): Promise<VerifiedRuntime> {
  const tmp = await allocTempDir(paths, `dl-${key}`);
  // The tree is its own temp directory directly under runtimes/: renaming a read-only directory to a
  // different parent is refused, renaming it within the same parent is not.
  const tree = await allocTempDir(paths, `x-${key}`);
  try {
    const archive = join(tmp, `archive.${record.archive_format}`);
    try {
      await downloader(record.url, archive);
    } catch (error) {
      throw new RuntimeResolveError(
        `Downloading ${record.url} failed: ${(error as Error).message}`,
        "download",
      );
    }
    const sha = await sha256File(archive);
    if (sha !== record.archive_sha256) {
      throw new RuntimeResolveError(
        `Refusing ${record.url}: archive SHA-256 is ${sha} but chromium/pin.json records ${record.archive_sha256}. Nothing was unpacked.`,
        "digest",
      );
    }
    await extractArchive(archive, tree, record.archive_format);
    await rm(archive, { force: true });
    let verified: VerifiedRuntime;
    try {
      verified = await verifyRuntimeDir(tree, key);
    } catch (error) {
      throw new RuntimeResolveError(
        `Downloaded runtime is invalid: ${(error as Error).message}`,
        "integrity",
      );
    }
    if (verified.manifestSha256 !== record.manifest_sha256) {
      throw new RuntimeResolveError(
        `Refusing ${record.url}: manifest digest is ${verified.manifestSha256} but chromium/pin.json records ${record.manifest_sha256}.`,
        "digest",
      );
    }
    if (verified.manifest.bdk_abi.hash !== record.abi_hash) {
      throw new RuntimeResolveError(
        `Refusing ${record.url}: manifest ABI hash ${verified.manifest.bdk_abi.hash} differs from pin.json ${record.abi_hash}.`,
        "digest",
      );
    }
    await makeReadOnly(tree);
    if (!(await installRuntimeDir(paths, key, tree))) {
      // Another process installed it first; it is verified below like any store entry.
    }
    return verified;
  } finally {
    await removeTree(tmp);
    await removeTree(tree);
  }
}

/**
 * Prebuilt-mode resolver (repository-and-tooling.md 7.1). Never cached: every call recomputes the key
 * and re-verifies the manifest and every file digest in the store.
 */
export async function resolveRuntime(opts: ResolveOptions): Promise<ResolveResult> {
  const { repoRoot, paths } = opts;
  const identity = await computeSourceIdentity({
    repoRoot,
    ...(opts.config === undefined ? {} : { config: opts.config }),
    ...(opts.target === undefined ? {} : { target: opts.target }),
  });
  const key = sourceKey(identity);
  const dir = paths.runtime(key);
  const record = readPin(repoRoot).runtime;
  let origin: ResolveResult["origin"] = "store";

  if (!existsSync(dir)) {
    if (opts.mode !== "source" && record !== null && record.source_key === key) {
      await fetchRuntime(key, record, paths, opts.downloader ?? download);
      origin = "downloaded";
    } else {
      throw new RuntimeResolveError(missingMessage(key, paths, record, opts), "missing");
    }
  }

  let verified: VerifiedRuntime;
  try {
    verified = await verifyRuntimeDir(dir, key);
  } catch (error) {
    throw new RuntimeResolveError(
      `${(error as Error).message}\nThe store entry ${dir} is damaged or was modified. Delete it ` +
        `(chmod -R u+w "${dir}" && rm -rf "${dir}") and resolve again to fetch or rebuild it.`,
      "integrity",
    );
  }
  await writePointer(repoRoot, dir, key, verified.manifestSha256);
  return { sourceKey: key, runtimeDir: dir, manifestSha256: verified.manifestSha256, origin };
}

/** Writes `.bdk/runtime.json` (only when changed, so its mtime is stable) and the `.bdk/runtime` symlink. */
async function writePointer(
  repoRoot: string,
  dir: string,
  key: string,
  manifestSha256: string,
): Promise<void> {
  const bdk = join(repoRoot, ".bdk");
  await mkdir(bdk, { recursive: true });
  const pointer = `${JSON.stringify({ source_key: key, manifest_sha256: manifestSha256 }, null, 2)}\n`;
  const file = join(bdk, "runtime.json");
  const current = await readFile(file, "utf8").catch(() => "");
  if (current !== pointer) await writeFile(file, pointer);
  const link = join(bdk, "runtime");
  const tmp = `${link}.${process.pid}.tmp`;
  await rm(tmp, { force: true });
  await symlink(dir, tmp);
  await rename(tmp, link);
}
