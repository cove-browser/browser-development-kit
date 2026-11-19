import { existsSync, readFileSync } from "node:fs";
import { readFile, readlink } from "node:fs/promises";
import { arch as nodeArch, platform as nodePlatform } from "node:os";
import { join } from "node:path";
import { readAbi } from "./abi.js";
import { canonicalJson, sha256Hex } from "./canonical.js";
import { readPin } from "./pin.js";
import { readRuntimeFiles } from "./runtime-files.js";
import { walkTree } from "./tree.js";

/** Files that never affect a build and must not force a runtime rebuild when edited. */
const IGNORED_NAMES = new Set(["README.md", ".DS_Store"]);

export interface Target {
  readonly os: string;
  readonly arch: string;
}

export function hostTarget(): Target {
  const os =
    nodePlatform() === "darwin" ? "macos" : nodePlatform() === "win32" ? "windows" : "linux";
  return { os, arch: nodeArch() };
}

/**
 * Everything that determines what the engine binary is, and nothing about who built it. Computable
 * in a worktree with no Chromium checkout: the Chromium commit already fixes `DEPS`, so no deps
 * digest is needed here (tooling/README.md explains the decision).
 */
export interface SourceIdentity {
  readonly chromium: { readonly tag: string; readonly commit: string };
  readonly bdk_abi: { readonly version: number; readonly hash: string };
  readonly bdk: {
    readonly patches_digest: string;
    readonly overlay_digest: string;
    /** Digest of packages/core/runtime, the runtime core the engine compiles into //bdk. */
    readonly core_digest: string;
    /** `"absent"` when the selected preset file does not exist. Packaging refuses that. */
    readonly gn_args_digest: string;
    /** Digest of chromium/runtime-files.json (paths and min_os): what is packaged decides what the key names. */
    readonly runtime_files_digest: string;
  };
  readonly target: Target;
}

export interface IdentityOptions {
  readonly repoRoot: string;
  /** GN preset name: `chromium/gn/<config>.gn`. Default `release`. */
  readonly config?: string;
  readonly target?: Target;
}

/**
 * SHA-256 over every file in `dir`, path-sorted, with each relative path hashed in. A missing
 * directory has the digest of an empty tree. Symlinks contribute their target text.
 */
export async function directoryDigest(dir: string): Promise<string> {
  if (!existsSync(dir)) return sha256Hex("");
  const lines: string[] = [];
  for (const e of await walkTree(dir)) {
    if (e.kind === "dir") continue;
    if (!e.rel.includes("/") && IGNORED_NAMES.has(e.rel)) continue;
    if (e.kind === "symlink") {
      lines.push(`${e.rel}\0link:${await readlink(join(dir, e.rel))}\n`);
    } else {
      lines.push(`${e.rel}\0${sha256Hex(await readFile(join(dir, e.rel)))}\n`);
    }
  }
  return sha256Hex(lines.join(""));
}

/** SHA-256 of the canonical runtime inventory: target and the sorted, normalised path list. */
export function runtimeFilesDigest(repoRoot: string): string {
  const rf = readRuntimeFiles(repoRoot);
  return sha256Hex(canonicalJson({ target: rf.target, paths: [...new Set(rf.paths)].sort() }));
}

export function gnPresetPath(repoRoot: string, config: string): string {
  return join(repoRoot, "chromium", "gn", `${config}.gn`);
}

export async function computeSourceIdentity(opts: IdentityOptions): Promise<SourceIdentity> {
  const { repoRoot } = opts;
  const config = opts.config ?? "release";
  const pin = readPin(repoRoot);
  const abi = readAbi(repoRoot);
  if (abi.version !== pin.bdk_abi_version) {
    throw new Error(
      `ABI version mismatch: chromium/pin.json says ${pin.bdk_abi_version} but packages/core defines ` +
        `BDK_ABI_VERSION ${abi.version}. Update the pin in the same change that bumps the ABI.`,
    );
  }
  const preset = gnPresetPath(repoRoot, config);
  return {
    chromium: { tag: pin.tag, commit: pin.commit },
    bdk_abi: { version: abi.version, hash: abi.hash },
    bdk: {
      patches_digest: await directoryDigest(join(repoRoot, "chromium", "patches")),
      overlay_digest: await directoryDigest(join(repoRoot, "chromium", "bdk")),
      core_digest: await directoryDigest(join(repoRoot, "packages", "core", "runtime")),
      gn_args_digest: existsSync(preset) ? sha256Hex(readFileSync(preset)) : "absent",
      runtime_files_digest: runtimeFilesDigest(repoRoot),
    },
    target: opts.target ?? hostTarget(),
  };
}

/** The store key: first 16 hex digits of the SHA-256 of the canonical source identity. */
export function sourceKey(id: SourceIdentity): string {
  return sha256Hex(canonicalJson({ key_schema: 3, ...id })).slice(0, 16);
}

export interface BuildIdentity {
  readonly xcode: string;
  readonly sdk: string;
  readonly clang: string;
  readonly os: string;
  readonly depot_tools: string;
  /** ISO 8601 UTC. The only field expected to differ between two packagings of the same inputs. */
  readonly timestamp: string;
  readonly ci_run: string;
}
