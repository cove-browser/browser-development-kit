import { readFile } from "node:fs/promises";
import { join } from "node:path";
import { canonicalJson, mapLimit, sha256File, sha256Hex } from "./canonical.js";
import type { BuildIdentity, SourceIdentity } from "./identity.js";
import { walkTree } from "./tree.js";

export const MANIFEST_NAME = "manifest.json";

/**
 * `files` values:
 * - `sha256:<hex>` for a regular file; `sha256:<hex>:exec` when it is executable;
 * - `symlink:<target>` for a symlink, never followed (framework `Versions/Current` links).
 * Directories are implied by the files under them; the manifest itself is not listed.
 */
export interface Manifest {
  readonly schema: 1;
  readonly source_key: string;
  readonly bdk_abi: { readonly version: number; readonly hash: string };
  readonly chromium: {
    readonly tag: string;
    readonly commit: string;
    readonly deps_digest: string;
  };
  readonly bdk: {
    readonly repo_commit: string;
    readonly patches_digest: string;
    readonly overlay_digest: string;
    readonly gn_args_digest: string;
  };
  readonly target: { readonly os: string; readonly arch: string; readonly min_os: string };
  readonly builder: BuildIdentity;
  readonly files: Readonly<Record<string, string>>;
  /** Empty while runtimes are built on a developer's machine (repository-and-tooling.md section 8). */
  readonly attestation: string;
}

export class RuntimeIntegrityError extends Error {
  override readonly name = "RuntimeIntegrityError";
}

/** SHA-256 (hex) of the canonical manifest. */
export function manifestDigest(manifest: Manifest): string {
  return sha256Hex(canonicalJson(manifest));
}

/** Digest of the payload alone: equal for two packagings of identical files, whatever the builder. */
export function contentDigest(files: Readonly<Record<string, string>>): string {
  return sha256Hex(canonicalJson(files));
}

export function renderManifest(manifest: Manifest): string {
  return `${JSON.stringify(JSON.parse(canonicalJson(manifest)), null, 2)}\n`;
}

export interface ManifestInputs {
  readonly sourceKey: string;
  readonly identity: SourceIdentity;
  readonly depsDigest: string;
  readonly repoCommit: string;
  readonly minOs: string;
  readonly builder: BuildIdentity;
  readonly files: Readonly<Record<string, string>>;
}

export function buildManifest(i: ManifestInputs): Manifest {
  return {
    schema: 1,
    source_key: i.sourceKey,
    bdk_abi: i.identity.bdk_abi,
    chromium: { ...i.identity.chromium, deps_digest: i.depsDigest },
    bdk: {
      repo_commit: i.repoCommit,
      patches_digest: i.identity.bdk.patches_digest,
      overlay_digest: i.identity.bdk.overlay_digest,
      gn_args_digest: i.identity.bdk.gn_args_digest,
    },
    target: { ...i.identity.target, min_os: i.minOs },
    builder: i.builder,
    files: i.files,
    attestation: "",
  };
}

/** Hashes every file and records every symlink under `dir`, except the top-level manifest. */
export async function scanFiles(dir: string): Promise<Record<string, string>> {
  const entries = (await walkTree(dir)).filter((e) => e.kind !== "dir" && e.rel !== MANIFEST_NAME);
  const values = await mapLimit(entries, 8, async (e) => {
    if (e.kind === "symlink") return `symlink:${e.target ?? ""}`;
    const digest = await sha256File(join(dir, e.rel));
    return e.mode & 0o111 ? `sha256:${digest}:exec` : `sha256:${digest}`;
  });
  const files: Record<string, string> = {};
  entries.forEach((e, i) => {
    files[e.rel] = values[i] as string;
  });
  return files;
}

export function parseManifest(text: string): Manifest {
  const m = JSON.parse(text) as Partial<Manifest> | null;
  if (m === null || typeof m !== "object" || m.schema !== 1) {
    throw new RuntimeIntegrityError("manifest.json: unsupported or missing schema (expected 1).");
  }
  if (typeof m.source_key !== "string" || m.files === undefined || typeof m.files !== "object") {
    throw new RuntimeIntegrityError("manifest.json: malformed (no source_key or files).");
  }
  return m as Manifest;
}

export interface VerifiedRuntime {
  readonly manifest: Manifest;
  /** SHA-256 (hex) of the canonical manifest. */
  readonly manifestSha256: string;
}

/**
 * Re-verifies a runtime directory: the manifest parses, names `expectedKey`, and the tree matches
 * `files` exactly (no missing, extra, modified or re-linked entries). Reads every file.
 */
export async function verifyRuntimeDir(dir: string, expectedKey: string): Promise<VerifiedRuntime> {
  let text: string;
  try {
    text = await readFile(join(dir, MANIFEST_NAME), "utf8");
  } catch {
    throw new RuntimeIntegrityError(`${dir}: manifest.json is missing or unreadable.`);
  }
  const manifest = parseManifest(text);
  if (manifest.source_key !== expectedKey) {
    throw new RuntimeIntegrityError(
      `${dir}: manifest is for source key ${manifest.source_key}, expected ${expectedKey}.`,
    );
  }
  let actual: Record<string, string>;
  try {
    actual = await scanFiles(dir);
  } catch (error) {
    throw new RuntimeIntegrityError(`${dir}: cannot scan runtime: ${(error as Error).message}`);
  }
  const problems: string[] = [];
  for (const [path, want] of Object.entries(manifest.files)) {
    const got = actual[path];
    if (got === undefined) problems.push(`missing: ${path}`);
    else if (got !== want) problems.push(`modified: ${path}`);
  }
  for (const path of Object.keys(actual)) {
    if (!(path in manifest.files)) problems.push(`unexpected: ${path}`);
  }
  if (problems.length > 0) {
    const shown = problems.slice(0, 10).join("\n  ");
    const more = problems.length > 10 ? `\n  ... and ${problems.length - 10} more` : "";
    throw new RuntimeIntegrityError(
      `Runtime ${expectedKey} failed verification (${problems.length} problem${problems.length === 1 ? "" : "s"}):\n  ${shown}${more}`,
    );
  }
  return { manifest, manifestSha256: manifestDigest(manifest) };
}
