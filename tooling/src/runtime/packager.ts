import { constants } from "node:fs";
import {
  chmod,
  copyFile,
  lstat,
  lutimes,
  mkdir,
  readdir,
  readFile,
  readlink,
  symlink,
  utimes,
  writeFile,
} from "node:fs/promises";
import { dirname, isAbsolute, join, posix } from "node:path";
import type { BdkPaths } from "../paths.js";
import { FIXED_MTIME } from "./archive.js";
import { sha256Hex } from "./canonical.js";
import {
  type BuildIdentity,
  computeSourceIdentity,
  gnPresetPath,
  type SourceIdentity,
  sourceKey,
  type Target,
} from "./identity.js";
import {
  buildManifest,
  contentDigest,
  MANIFEST_NAME,
  type Manifest,
  manifestDigest,
  parseManifest,
  renderManifest,
  scanFiles,
  verifyRuntimeDir,
} from "./manifest.js";
import { readRuntimeFiles } from "./runtime-files.js";
import { allocTempDir, installRuntimeDir, removeTree } from "./store.js";
import { makeReadOnly, walkTree } from "./tree.js";

export class PackagingError extends Error {
  override readonly name = "PackagingError";
}

/** A declared runtime file that is not in the build output. Names every missing path. */
export class MissingFilesError extends PackagingError {
  constructor(
    readonly missing: readonly string[],
    outDir: string,
  ) {
    super(
      `Packaging failed: ${missing.length} declared runtime file${missing.length === 1 ? "" : "s"} missing from ${outDir}:\n` +
        `${missing.map((m) => `  ${m}`).join("\n")}\n` +
        "Build first (pnpm chromium:build) or fix chromium/runtime-files.json.",
    );
  }
}

/** The key already exists in the store with different file content. */
export class ContentMismatchError extends PackagingError {}

export interface PackageOptions {
  readonly repoRoot: string;
  readonly paths: BdkPaths;
  /** `<checkout>/src/out/<config>` */
  readonly outDir: string;
  /** Chromium `src/DEPS`, hashed into the manifest. */
  readonly depsFile: string;
  /** The commit the checkout's `src` is at. Must equal the pin. */
  readonly checkoutCommit: string;
  readonly config?: string;
  readonly target?: Target;
  readonly builder: BuildIdentity;
  /** The BDK repository commit, recorded as provenance only. */
  readonly repoCommit: string;
}

export interface PackageResult {
  readonly sourceKey: string;
  readonly runtimeDir: string;
  readonly manifest: Manifest;
  readonly manifestSha256: string;
  readonly status: "created" | "unchanged";
}

async function pathExists(path: string): Promise<boolean> {
  return lstat(path).then(
    () => true,
    () => false,
  );
}

/** Copies a file, directory or symlink, keeping symlinks as symlinks and normalising modes and times. */
async function copyEntry(src: string, dest: string): Promise<void> {
  const st = await lstat(src);
  if (st.isSymbolicLink()) {
    const target = await readlink(src);
    if (isAbsolute(target))
      throw new PackagingError(`Absolute symlink target in runtime: ${src} -> ${target}`);
    await symlink(target, dest);
  } else if (st.isDirectory()) {
    await mkdir(dest, { mode: 0o755 });
    for (const name of (await readdir(src)).sort())
      await copyEntry(join(src, name), join(dest, name));
  } else if (st.isFile()) {
    await copyFile(src, dest, constants.COPYFILE_FICLONE);
    await chmod(dest, st.mode & 0o111 ? 0o755 : 0o644);
  } else {
    throw new PackagingError(`Unsupported file type in build output: ${src}`);
  }
}

/** Every symlink must stay inside the runtime tree. */
async function checkLinks(root: string): Promise<void> {
  for (const e of await walkTree(root)) {
    if (e.kind !== "symlink") continue;
    const resolved = posix.normalize(posix.join(posix.dirname(e.rel), e.target ?? ""));
    if (resolved.startsWith("..") || posix.isAbsolute(e.target ?? "")) {
      throw new PackagingError(`Symlink ${e.rel} -> ${e.target} points outside the runtime.`);
    }
  }
}

async function normaliseTimes(root: string): Promise<void> {
  const entries = await walkTree(root);
  for (const e of entries) {
    const p = join(root, e.rel);
    if (e.kind === "symlink") await lutimes(p, FIXED_MTIME, FIXED_MTIME);
    else if (e.kind === "file") await utimes(p, FIXED_MTIME, FIXED_MTIME);
  }
  for (const e of [...entries].reverse()) {
    if (e.kind === "dir") await utimes(join(root, e.rel), FIXED_MTIME, FIXED_MTIME);
  }
  await utimes(root, FIXED_MTIME, FIXED_MTIME);
}

/**
 * Assembles `runtimes/<source-key>` from the build output: copy the declared files into a temporary
 * directory in the store, write and verify `manifest.json`, make it read-only, rename into place.
 * Idempotent when the key exists with identical file content; an error when the content differs.
 */
export async function packageRuntime(opts: PackageOptions): Promise<PackageResult> {
  const { repoRoot, paths, outDir } = opts;
  const declared = readRuntimeFiles(repoRoot);
  const identity = await computeSourceIdentity({
    repoRoot,
    ...(opts.config === undefined ? {} : { config: opts.config }),
    ...(opts.target === undefined ? {} : { target: opts.target }),
  });
  if (identity.bdk.gn_args_digest === "absent") {
    throw new PackagingError(
      `GN preset ${gnPresetPath(repoRoot, opts.config ?? "release")} does not exist; the runtime's identity would not cover its build arguments.`,
    );
  }
  if (identity.chromium.commit !== opts.checkoutCommit) {
    throw new PackagingError(
      `The checkout is at ${opts.checkoutCommit} but chromium/pin.json pins ${identity.chromium.commit}. ` +
        "Run pnpm chromium:sync and pnpm chromium:build for the pin before packaging.",
    );
  }
  const missing: string[] = [];
  for (const rel of declared.paths) {
    if (!(await pathExists(join(outDir, rel)))) missing.push(rel);
  }
  if (missing.length > 0) throw new MissingFilesError(missing, outDir);
  const depsDigest = sha256Hex(await readFile(opts.depsFile));

  const key = sourceKey(identity);
  const tmp = await allocTempDir(paths, key);
  try {
    for (const rel of declared.paths) {
      const dest = join(tmp, rel);
      await mkdir(dirname(dest), { recursive: true });
      await copyEntry(join(outDir, rel), dest);
    }
    await checkLinks(tmp);
    await normaliseTimes(tmp);
    const files = await scanFiles(tmp);
    const manifest = buildManifest({
      sourceKey: key,
      identity,
      depsDigest,
      repoCommit: opts.repoCommit,
      minOs: declared.target.min_os,
      builder: opts.builder,
      files,
    });
    await writeFile(join(tmp, MANIFEST_NAME), renderManifest(manifest));
    await utimes(join(tmp, MANIFEST_NAME), FIXED_MTIME, FIXED_MTIME);
    await utimes(tmp, FIXED_MTIME, FIXED_MTIME);
    const verified = await verifyRuntimeDir(tmp, key);

    const dest = paths.runtime(key);
    const existing = await sameContentOrThrow(dest, key, files);
    if (existing !== undefined) {
      return {
        sourceKey: key,
        runtimeDir: dest,
        manifest: existing.manifest,
        manifestSha256: existing.sha,
        status: "unchanged",
      };
    }
    await makeReadOnly(tmp);
    if (!(await installRuntimeDir(paths, key, tmp))) {
      const raced = await sameContentOrThrow(dest, key, files);
      if (raced === undefined) throw new PackagingError(`Could not install ${dest}.`);
      return {
        sourceKey: key,
        runtimeDir: dest,
        manifest: raced.manifest,
        manifestSha256: raced.sha,
        status: "unchanged",
      };
    }
    return {
      sourceKey: key,
      runtimeDir: dest,
      manifest,
      manifestSha256: verified.manifestSha256,
      status: "created",
    };
  } finally {
    await removeTree(tmp);
  }
}

async function sameContentOrThrow(
  dest: string,
  key: string,
  files: Readonly<Record<string, string>>,
): Promise<{ manifest: Manifest; sha: string } | undefined> {
  if (!(await pathExists(join(dest, MANIFEST_NAME)))) return undefined;
  const existing = parseManifest(await readFile(join(dest, MANIFEST_NAME), "utf8"));
  if (contentDigest(existing.files) !== contentDigest(files)) {
    throw new ContentMismatchError(
      `Runtime ${key} already exists in the store with different file content (${dest}). ` +
        "The same source identity must produce the same files. If the build is non-deterministic, " +
        "fix that; to replace the stored runtime deliberately, delete it first " +
        `(chmod -R u+w "${dest}" && rm -rf "${dest}").`,
    );
  }
  return { manifest: existing, sha: manifestDigest(existing) };
}

export type { SourceIdentity };
