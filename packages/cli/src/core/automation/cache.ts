import { existsSync, readFileSync } from "node:fs";
import { mkdir, writeFile } from "node:fs/promises";
import { dirname, join } from "node:path";
import { fileDigest } from "../config/digest.js";

/** What the last successful bundle of one script was made from. */
export interface CacheEntry {
  /** Absolute path of the script the bundle was made from. */
  entry: string;
  /** Absolute path of the written bundle. */
  output: string;
  bundle_digest: string;
  /** Digest of every source file that went into it, by absolute path (the script itself included). */
  inputs: Record<string, string>;
}

/** By script name. A `Map`, not an object: a script may be called `__proto__`. */
export type Cache = Map<string, CacheEntry>;

export function cacheFile(projectDir: string): string {
  return join(projectDir, ".bdk", "automation-cache.json");
}

/** The cache, or an empty one when it is missing or unreadable (the build then runs in full). */
export function readCache(projectDir: string): Cache {
  try {
    const parsed: unknown = JSON.parse(readFileSync(cacheFile(projectDir), "utf8"));
    if (typeof parsed !== "object" || parsed === null || Array.isArray(parsed)) return new Map();
    return new Map(Object.entries(parsed as Record<string, CacheEntry>));
  } catch {
    return new Map();
  }
}

export async function writeCache(projectDir: string, cache: Cache): Promise<void> {
  const file = cacheFile(projectDir);
  await mkdir(dirname(file), { recursive: true });
  // fromEntries defines own properties, so `__proto__` stays a key instead of a prototype.
  await writeFile(file, `${JSON.stringify(Object.fromEntries(cache), null, 2)}\n`);
}

export function digestInputs(files: readonly string[]): Record<string, string> {
  return Object.fromEntries(files.map((file) => [file, fileDigest(file)]));
}

/**
 * True when the bundle is on disk with the recorded digest, was made from the same script, and no
 * input changed.
 */
export function isUpToDate(entry: CacheEntry | undefined, script: string, output: string): boolean {
  if (entry === undefined || entry.entry !== script || entry.output !== output) return false;
  if (typeof entry.inputs !== "object" || entry.inputs === null || !existsSync(output)) {
    return false;
  }
  if (fileDigest(output) !== entry.bundle_digest) return false;
  return Object.entries(entry.inputs).every(
    ([file, digest]) => existsSync(file) && fileDigest(file) === digest,
  );
}
