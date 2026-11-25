import { existsSync, statSync } from "node:fs";
import { readFile } from "node:fs/promises";
import { join } from "node:path";
import { canonicalJson } from "./canonical.js";
import { MANIFEST_NAME, type Manifest, parseManifest } from "./manifest.js";

/**
 * How the two `files` tables are compared:
 * - `strict`: same paths, kinds, modes and digests (one build output packaged twice; `--reproducible-check`);
 * - `paths`: same paths, kinds and modes; a differing digest is reported as a note (two separate builds of the
 *   same inputs, which Chromium does not promise are bit-identical).
 */
export type FilesMode = "strict" | "paths";

export interface ManifestComparison {
  /** Equal source identity, ABI and inventory (and digests, in `strict` mode). */
  readonly equivalent: boolean;
  readonly sourceKey: { readonly a: string; readonly b: string };
  readonly abiHash: { readonly a: string; readonly b: string };
  /** What makes the two manifests not equivalent. Empty when they are. */
  readonly differences: readonly string[];
  /** Expected differences that do not count: provenance, and file digests in `paths` mode. */
  readonly notes: readonly string[];
}

/** Fields that name what the runtime was built from. They must be equal for two builds of the same inputs. */
const IDENTITY_FIELDS: readonly (readonly [string, (m: Manifest) => unknown])[] = [
  ["schema", (m) => m.schema],
  ["source_key", (m) => m.source_key],
  ["bdk_abi.version", (m) => m.bdk_abi.version],
  ["bdk_abi.hash", (m) => m.bdk_abi.hash],
  ["chromium.tag", (m) => m.chromium.tag],
  ["chromium.commit", (m) => m.chromium.commit],
  ["chromium.deps_digest", (m) => m.chromium.deps_digest],
  ["bdk.patches_digest", (m) => m.bdk.patches_digest],
  ["bdk.overlay_digest", (m) => m.bdk.overlay_digest],
  ["bdk.core_digest", (m) => m.bdk.core_digest],
  ["bdk.gn_args_digest", (m) => m.bdk.gn_args_digest],
  ["target.os", (m) => m.target.os],
  ["target.arch", (m) => m.target.arch],
  ["target.min_os", (m) => m.target.min_os],
];

/** Provenance: allowed to differ between two builds of the same inputs, reported as notes. */
const PROVENANCE_FIELDS: readonly (readonly [string, (m: Manifest) => unknown])[] = [
  ["bdk.repo_commit", (m) => m.bdk.repo_commit],
  ["builder", (m) => m.builder],
  ["attestation", (m) => m.attestation],
];

type FileEntry =
  | { readonly kind: "file"; readonly digest: string; readonly exec: boolean }
  | {
      readonly kind: "symlink";
      readonly target: string;
    };

function parseEntry(value: string): FileEntry {
  if (value.startsWith("symlink:")) return { kind: "symlink", target: value.slice(8) };
  const [, digest = "", flag] = value.split(":");
  return { kind: "file", digest, exec: flag === "exec" };
}

function describe(value: unknown): string {
  return typeof value === "string" ? value : canonicalJson(value);
}

/**
 * T-CHR-09 (M9): whether two runtimes are equivalent builds of the same inputs. The ABI hash and the source
 * identity must be equal, the file inventory must be the same (paths, file or symlink, executable bit, link
 * targets), and in `strict` mode every digest too. Who built it, when and from which BDK commit is provenance.
 */
export function compareManifests(
  a: Manifest,
  b: Manifest,
  files: FilesMode = "strict",
): ManifestComparison {
  const differences: string[] = [];
  const notes: string[] = [];
  for (const [name, get] of IDENTITY_FIELDS) {
    const x = describe(get(a));
    const y = describe(get(b));
    if (x !== y) differences.push(`${name} differs: ${x} vs ${y}`);
  }
  for (const [name, get] of PROVENANCE_FIELDS) {
    if (describe(get(a)) !== describe(get(b))) notes.push(`${name} differs (provenance)`);
  }
  const paths = [...new Set([...Object.keys(a.files), ...Object.keys(b.files)])].sort();
  let digestNotes = 0;
  for (const path of paths) {
    const x = a.files[path];
    const y = b.files[path];
    if (x === undefined) {
      differences.push(`only in the second runtime: ${path}`);
      continue;
    }
    if (y === undefined) {
      differences.push(`only in the first runtime: ${path}`);
      continue;
    }
    if (x === y) continue;
    const ex = parseEntry(x);
    const ey = parseEntry(y);
    if (ex.kind !== ey.kind) {
      differences.push(`${path}: ${ex.kind} vs ${ey.kind}`);
    } else if (ex.kind === "symlink" && ey.kind === "symlink") {
      differences.push(`${path}: symlink to ${ex.target} vs ${ey.target}`);
    } else if (ex.kind === "file" && ey.kind === "file") {
      if (ex.exec !== ey.exec) differences.push(`${path}: executable bit differs`);
      else if (files === "strict") differences.push(`file differs: ${path}`);
      else digestNotes++;
    }
  }
  if (digestNotes > 0) {
    notes.push(
      `${digestNotes} file${digestNotes === 1 ? "" : "s"} with different content (not compared in paths mode)`,
    );
  }
  return {
    equivalent: differences.length === 0,
    sourceKey: { a: a.source_key, b: b.source_key },
    abiHash: { a: a.bdk_abi.hash, b: b.bdk_abi.hash },
    differences,
    notes,
  };
}

/** Reads `manifest.json` from a runtime directory, or a manifest file given directly. */
export async function loadManifest(path: string): Promise<Manifest> {
  const file = existsSync(path) && statSync(path).isDirectory() ? join(path, MANIFEST_NAME) : path;
  return parseManifest(await readFile(file, "utf8"));
}
