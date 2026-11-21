import { existsSync, readFileSync } from "node:fs";
import { mkdir, writeFile } from "node:fs/promises";
import { dirname, join } from "node:path";
import { fileDigest } from "../config/digest.js";

/** What the last successful bundle of one script was made from. */
export interface CacheEntry {
  /** Absolute path of the written bundle. */
  output: string;
  bundle_digest: string;
  /** Digest of every source file that went into it, by absolute path (the script itself included). */
  inputs: Record<string, string>;
}

export type Cache = Record<string, CacheEntry>;

export function cacheFile(projectDir: string): string {
  return join(projectDir, ".bdk", "automation-cache.json");
}

/** The cache, or an empty one when it is missing or unreadable (the build then runs in full). */
export function readCache(projectDir: string): Cache {
  try {
    const parsed: unknown = JSON.parse(readFileSync(cacheFile(projectDir), "utf8"));
    return typeof parsed === "object" && parsed !== null ? (parsed as Cache) : {};
  } catch {
    return {};
  }
}

export async function writeCache(projectDir: string, cache: Cache): Promise<void> {
  const file = cacheFile(projectDir);
  await mkdir(dirname(file), { recursive: true });
  await writeFile(file, `${JSON.stringify(cache, null, 2)}\n`);
}

export function digestInputs(files: readonly string[]): Record<string, string> {
  return Object.fromEntries(files.map((file) => [file, fileDigest(file)]));
}

/** True when the bundle is on disk with the recorded digest and no input changed. */
export function isUpToDate(entry: CacheEntry | undefined, output: string): boolean {
  if (entry === undefined || entry.output !== output || !existsSync(output)) return false;
  if (fileDigest(output) !== entry.bundle_digest) return false;
  return Object.entries(entry.inputs).every(
    ([file, digest]) => existsSync(file) && fileDigest(file) === digest,
  );
}
