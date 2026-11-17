import { randomBytes } from "node:crypto";
import { existsSync } from "node:fs";
import { mkdir, rename, rm } from "node:fs/promises";
import { join } from "node:path";
import type { BdkPaths } from "../paths.js";
import { MANIFEST_NAME } from "./manifest.js";
import { makeWritable } from "./tree.js";

/** A fresh temporary directory beside the runtimes, on the same filesystem so rename is atomic. */
export async function allocTempDir(paths: BdkPaths, label: string): Promise<string> {
  await mkdir(paths.runtimes, { recursive: true });
  const dir = join(
    paths.runtimes,
    `.tmp-${label}-${process.pid}-${randomBytes(4).toString("hex")}`,
  );
  await mkdir(dir);
  return dir;
}

export function runtimeDir(paths: BdkPaths, sourceKey: string): string {
  return paths.runtime(sourceKey);
}

export function runtimeIsInstalled(paths: BdkPaths, sourceKey: string): boolean {
  return existsSync(join(paths.runtime(sourceKey), MANIFEST_NAME));
}

/** Atomically moves a finished, read-only temp tree to `runtimes/<key>`. Returns false if it already exists. */
export async function installRuntimeDir(
  paths: BdkPaths,
  sourceKey: string,
  tempDir: string,
): Promise<boolean> {
  const dest = paths.runtime(sourceKey);
  try {
    await rename(tempDir, dest);
    return true;
  } catch (error) {
    const code = (error as NodeJS.ErrnoException).code;
    if (code === "EEXIST" || code === "ENOTEMPTY" || code === "EISDIR") return false;
    throw error;
  }
}

/** Deletes a directory that may be read-only (a store entry or a failed temp tree). */
export async function removeTree(dir: string): Promise<void> {
  await makeWritable(dir);
  await rm(dir, { recursive: true, force: true });
}
