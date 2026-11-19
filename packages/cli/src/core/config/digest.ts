import { createHash } from "node:crypto";
import { readdirSync, readFileSync, statSync } from "node:fs";
import { join } from "node:path";

const IGNORED = new Set([".DS_Store"]);

export function sha256(data: string | Uint8Array): string {
  return `sha256:${createHash("sha256").update(data).digest("hex")}`;
}

export function fileDigest(file: string): string {
  return sha256(readFileSync(file));
}

function listFiles(root: string, relative: string): string[] {
  const out: string[] = [];
  const names = readdirSync(join(root, relative)).sort((a, b) => (a < b ? -1 : a > b ? 1 : 0));
  for (const name of names) {
    if (IGNORED.has(name)) continue;
    const child = relative === "" ? name : `${relative}/${name}`;
    if (statSync(join(root, child)).isDirectory()) out.push(...listFiles(root, child));
    else out.push(child);
  }
  return out;
}

/** A digest of every file under a directory: relative path, then content, in sorted path order. */
export function directoryDigest(directory: string): string {
  const hash = createHash("sha256");
  for (const file of listFiles(directory, "")) {
    const content = readFileSync(join(directory, file));
    hash.update(`${file}\0${content.length}\0`);
    hash.update(content);
  }
  return `sha256:${hash.digest("hex")}`;
}
