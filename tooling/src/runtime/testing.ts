import { mkdirSync, mkdtempSync, rmSync, symlinkSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { bdkPaths } from "../paths.js";
import type { BuildIdentity } from "./identity.js";
import { computeSourceIdentity } from "./identity.js";
import type { PackageOptions } from "./packager.js";
import { writeBuildStamp } from "./stamp.js";

/** Test fixtures: a fake BDK repository, a fake Chromium build output, a fake BDK_HOME. Not shipped. */

export const FAKE_COMMIT = "b8c2be851f62f6a394ec57955d7f31335d7cb6d4";
export const FAKE_ABI_HASH = "a".repeat(64);

export function tempDir(label: string): string {
  return mkdtempSync(join(tmpdir(), `bdk-${label}-`));
}

function put(path: string, content: string, mode?: number): void {
  mkdirSync(join(path, ".."), { recursive: true });
  writeFileSync(path, content, mode === undefined ? undefined : { mode });
}

export interface RepoFixture {
  readonly patch?: string;
  readonly abiHash?: string;
}

/** A minimal BDK repository: pin, patches, overlay, GN preset, runtime-files, ABI sources. */
export function makeRepo(root: string, fixture: RepoFixture = {}): string {
  put(
    join(root, "chromium/pin.json"),
    `${JSON.stringify({ tag: "158.0.8127.37", commit: FAKE_COMMIT, bdk_abi_version: 1, runtime: null }, null, 2)}\n`,
  );
  put(join(root, "chromium/patches/series"), "0001-example.patch\n");
  put(join(root, "chromium/patches/0001-example.patch"), fixture.patch ?? "--- a\n+++ b\n");
  put(join(root, "chromium/patches/README.md"), "docs\n");
  put(join(root, "chromium/bdk/engine.cc"), "int engine() { return 1; }\n");
  put(join(root, "chromium/gn/release.gn"), "is_debug = false\n");
  put(
    join(root, "chromium/runtime-files.json"),
    `${JSON.stringify({ schema: 1, target: { os: "macos", min_os: "13.0" }, paths: ["Chromium.app"] })}\n`,
  );
  put(join(root, "packages/core/include/bdk/bdk_base.h"), "#define BDK_ABI_VERSION 1\n");
  put(join(root, "packages/core/generated/abi-hash.txt"), `${fixture.abiHash ?? FAKE_ABI_HASH}\n`);
  return root;
}

/** `out/release` with a fake Chromium.app, including macOS framework symlinks. */
export function makeOut(outDir: string, marker = "one"): void {
  rmSync(outDir, { recursive: true, force: true });
  const app = join(outDir, "Chromium.app", "Contents");
  put(join(app, "Info.plist"), "<plist/>\n");
  put(join(app, "MacOS", "Chromium"), `#!/bin/sh\necho ${marker}\n`, 0o755);
  const fw = join(app, "Frameworks", "Chromium Framework.framework");
  put(join(fw, "Versions", "158.0.8127.37", "Chromium Framework"), `engine ${marker}\n`);
  put(join(fw, "Versions", "158.0.8127.37", "Resources", "icudtl.dat"), "icu\n");
  put(
    join(app, "Frameworks", "Chromium Helper.app", "Contents", "MacOS", "Chromium Helper"),
    "helper\n",
    0o755,
  );
  symlinkSync("158.0.8127.37", join(fw, "Versions", "Current"));
  symlinkSync("Versions/Current/Chromium Framework", join(fw, "Chromium Framework"));
  symlinkSync("Versions/Current/Resources", join(fw, "Resources"));
}

export const BUILDER: BuildIdentity = {
  xcode: "Xcode 27.2",
  sdk: "27.0",
  clang: "clang 23",
  os: "27.2",
  depot_tools: "d".repeat(40),
  timestamp: "2026-11-16T10:00:00.000Z",
  ci_run: "",
};

export interface Sandbox {
  readonly root: string;
  readonly repo: string;
  readonly outDir: string;
  readonly depsFile: string;
  readonly home: string;
  readonly paths: ReturnType<typeof bdkPaths>;
}

/** A fake BDK repository, a fake build output and a fake BDK_HOME under one temp directory. */
export function makeSandbox(label: string, fixture: RepoFixture = {}): Sandbox {
  const root = tempDir(label);
  const repo = makeRepo(join(root, "repo"), fixture);
  const outDir = join(root, "chromium", "src", "out", "release");
  makeOut(outDir);
  const depsFile = join(root, "chromium", "src", "DEPS");
  put(depsFile, "deps = {}\n");
  const home = join(root, "home");
  return { root, repo, outDir, depsFile, home, paths: bdkPaths(home) };
}

export function packageOptions(s: Sandbox, over: Partial<PackageOptions> = {}): PackageOptions {
  return {
    repoRoot: s.repo,
    paths: s.paths,
    outDir: s.outDir,
    depsFile: s.depsFile,
    checkoutBase: FAKE_COMMIT,
    verifyAbiHash: async () => {},
    builder: BUILDER,
    repoCommit: "c".repeat(40),
    ...over,
  };
}

/** Records in `s.outDir` that it was built from the sandbox repository's current inputs (what chromium:build does). */
export async function stampOut(s: Sandbox): Promise<void> {
  await writeBuildStamp(s.outDir, await computeSourceIdentity({ repoRoot: s.repo }));
}
