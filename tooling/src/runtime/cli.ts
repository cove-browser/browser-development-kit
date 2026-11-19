import { realpathSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

/** The workspace root, from any file under `tooling/src/<dir>/`. */
export function repoRootFromHere(importMetaUrl: string): string {
  return resolve(dirname(fileURLToPath(importMetaUrl)), "..", "..", "..");
}

/** True when the module is the process entry point (not imported by a test). */
export function isMain(importMetaUrl: string): boolean {
  const entry = process.argv[1];
  return entry !== undefined && realpathSync(entry) === fileURLToPath(importMetaUrl);
}

/** Returns the value after `flag` or throws. */
export function takeValue(argv: string[], i: number, flag: string): string {
  const v = argv[i + 1];
  if (v === undefined || v.startsWith("--")) throw new Error(`${flag} needs a value.`);
  return v;
}
