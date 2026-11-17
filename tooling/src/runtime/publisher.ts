import { execFile } from "node:child_process";
import { rm } from "node:fs/promises";
import { join } from "node:path";
import type { BdkPaths } from "../paths.js";
import { archiveFileName, createArchive, defaultFormat } from "./archive.js";
import type { PublishBackend } from "./backends.js";
import { computeSourceIdentity, sourceKey, type Target } from "./identity.js";
import { verifyRuntimeDir } from "./manifest.js";
import { type ArchiveFormat, type RuntimeRecord, readPin, writePin } from "./pin.js";
import { allocTempDir, removeTree } from "./store.js";

export class PublishError extends Error {
  override readonly name = "PublishError";
}

export function gitStatusPorcelain(repoRoot: string): Promise<string> {
  return new Promise((resolve, reject) => {
    execFile("git", ["-C", repoRoot, "status", "--porcelain"], (error, stdout, stderr) => {
      if (error) reject(new Error(`git status failed: ${stderr.trim() || error.message}`));
      else resolve(stdout);
    });
  });
}

export interface PublishOptions {
  readonly repoRoot: string;
  readonly paths: BdkPaths;
  readonly backend: PublishBackend;
  readonly config?: string;
  readonly target?: Target;
  readonly format?: ArchiveFormat;
  /** Archive and print the record without uploading or writing pin.json. */
  readonly dryRun?: boolean;
  /** Injectable for tests. Default: `git status --porcelain` in `repoRoot`. */
  readonly gitStatus?: (repoRoot: string) => Promise<string>;
}

export interface PublishResult {
  readonly record: RuntimeRecord;
  readonly uploaded: boolean;
}

/**
 * Archives `runtimes/<key>`, uploads it, and records the result in `chromium/pin.json`. Refuses from a
 * dirty tree, because the key must describe committed inputs.
 */
export async function publishRuntime(opts: PublishOptions): Promise<PublishResult> {
  const { repoRoot, paths } = opts;
  const dirty = (await (opts.gitStatus ?? gitStatusPorcelain)(repoRoot)).trim();
  if (dirty !== "") {
    throw new PublishError(
      `Refusing to publish from a dirty tree; commit or stash first (git status --porcelain):\n${dirty}`,
    );
  }
  const identity = await computeSourceIdentity({
    repoRoot,
    ...(opts.config === undefined ? {} : { config: opts.config }),
    ...(opts.target === undefined ? {} : { target: opts.target }),
  });
  const key = sourceKey(identity);
  const dir = paths.runtime(key);
  let verified: Awaited<ReturnType<typeof verifyRuntimeDir>>;
  try {
    verified = await verifyRuntimeDir(dir, key);
  } catch (error) {
    throw new PublishError(
      `No intact packaged runtime for source key ${key}: ${(error as Error).message}\n` +
        "Run pnpm chromium:build && pnpm chromium:package for this commit first.",
    );
  }
  const pin = readPin(repoRoot);
  if (pin.runtime !== null && pin.runtime.source_key === key) {
    throw new PublishError(
      `chromium/pin.json already records a published runtime for source key ${key}. ` +
        "Published runtimes are immutable; change the inputs (a new key) or remove the record deliberately.",
    );
  }
  const format = opts.format ?? defaultFormat();
  const tmp = await allocTempDir(paths, `pub-${key}`);
  try {
    const archivePath = join(tmp, archiveFileName(key, format));
    const archive = await createArchive(dir, archivePath, format);
    if (opts.dryRun === true) {
      return {
        record: recordFor(
          key,
          "(dry run: not uploaded)",
          format,
          archive,
          verified.manifestSha256,
          identity.bdk_abi.hash,
        ),
        uploaded: false,
      };
    }
    const { url } = await opts.backend.upload({ archivePath, sourceKey: key });
    const record = recordFor(
      key,
      url,
      format,
      archive,
      verified.manifestSha256,
      identity.bdk_abi.hash,
    );
    writePin(repoRoot, { ...pin, runtime: record });
    return { record, uploaded: true };
  } finally {
    await removeTree(tmp).catch(() => rm(tmp, { recursive: true, force: true }));
  }
}

function recordFor(
  key: string,
  url: string,
  format: ArchiveFormat,
  archive: { sha256: string; size: number },
  manifestSha256: string,
  abiHash: string,
): RuntimeRecord {
  return {
    source_key: key,
    url,
    archive_format: format,
    archive_sha256: archive.sha256,
    manifest_sha256: manifestSha256,
    abi_hash: abiHash,
    size: archive.size,
  };
}
