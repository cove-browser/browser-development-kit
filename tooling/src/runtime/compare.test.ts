import { join } from "node:path";
import { afterEach, describe, expect, it, vi } from "vitest";
import { main as compareMain } from "../chromium/compare-runtimes.js";
import { bdkPaths } from "../paths.js";
import { compareManifests, loadManifest } from "./compare.js";
import type { Manifest } from "./manifest.js";
import { packageRuntime } from "./packager.js";
import { removeTree } from "./store.js";
import { BUILDER, makeOut, makeSandbox, packageOptions, stampOut } from "./testing.js";

const cleanup: string[] = [];
afterEach(async () => {
  for (const d of cleanup.splice(0)) await removeTree(d);
  vi.restoreAllMocks();
});

async function sandbox(label: string, fixture?: Parameters<typeof makeSandbox>[1]) {
  const s = makeSandbox(label, fixture);
  cleanup.push(s.root);
  await stampOut(s);
  return s;
}

/** Packages the sandbox's build output into a store of its own, as a second machine would. */
async function packageInto(s: Awaited<ReturnType<typeof sandbox>>, home: string, over = {}) {
  return packageRuntime({ ...packageOptions(s), paths: bdkPaths(join(s.root, home)), ...over });
}

describe("T-CHR-09 two builds from the same inputs (M9)", () => {
  it("another builder, time and BDK commit give the same ABI hash and equivalent manifests", async () => {
    const s = await sandbox("cmp-same");
    const a = await packageInto(s, "m1");
    const b = await packageInto(s, "m2", {
      builder: { ...BUILDER, xcode: "Xcode 27.3", timestamp: "2026-11-20T00:00:00.000Z" },
      repoCommit: "e".repeat(40),
    });
    for (const mode of ["strict", "paths"] as const) {
      const r = compareManifests(a.manifest, b.manifest, mode);
      expect(r.differences).toEqual([]);
      expect(r.equivalent).toBe(true);
      expect(r.abiHash.a).toBe(r.abiHash.b);
    }
    const notes = compareManifests(a.manifest, b.manifest).notes;
    expect(notes).toContain("builder differs (provenance)");
    expect(notes).toContain("bdk.repo_commit differs (provenance)");
  });

  it("a rebuild with different bytes is equivalent by inventory and not in strict mode", async () => {
    const s = await sandbox("cmp-bytes");
    const a = await packageInto(s, "m1");
    makeOut(s.outDir, "two");
    await stampOut(s);
    const b = await packageInto(s, "m2");
    const paths = compareManifests(a.manifest, b.manifest, "paths");
    expect(paths.equivalent).toBe(true);
    expect(paths.notes).toContain("2 files with different content (not compared in paths mode)");
    const strict = compareManifests(a.manifest, b.manifest, "strict");
    expect(strict.equivalent).toBe(false);
    expect(strict.differences).toContain(
      "file differs: Chromium.app/Contents/Frameworks/Chromium Framework.framework/Versions/158.0.8127.37/Chromium Framework",
    );
  });

  it("a different ABI hash is a different runtime, named by field", async () => {
    const a = await packageInto(await sandbox("cmp-abi1"), "m");
    const b = await packageInto(await sandbox("cmp-abi2", { abiHash: "b".repeat(64) }), "m");
    const r = compareManifests(a.manifest, b.manifest, "paths");
    expect(r.equivalent).toBe(false);
    expect(r.differences.some((d) => d.startsWith("bdk_abi.hash differs"))).toBe(true);
    expect(r.differences.some((d) => d.startsWith("source_key differs"))).toBe(true);
  });

  it("an extra or missing file, a retargeted link and a lost executable bit are differences", () => {
    const base = {
      schema: 1,
      source_key: "k",
      bdk_abi: { version: 1, hash: "a".repeat(64) },
      chromium: { tag: "t", commit: "c", deps_digest: "d" },
      bdk: {
        repo_commit: "r",
        patches_digest: "p",
        overlay_digest: "o",
        core_digest: "c",
        gn_args_digest: "g",
      },
      target: { os: "macos", arch: "arm64", min_os: "13.0" },
      builder: BUILDER,
      files: { bin: "sha256:1:exec", link: "symlink:a", same: "sha256:2" },
      attestation: "",
    } satisfies Manifest;
    const other: Manifest = {
      ...base,
      files: { bin: "sha256:1", link: "symlink:b", extra: "sha256:3" },
    };
    const r = compareManifests(base, other, "paths");
    expect(r.differences).toEqual([
      "bin: executable bit differs",
      "only in the second runtime: extra",
      "link: symlink to a vs b",
      "only in the first runtime: same",
    ]);
  });

  it("the command reads two runtime directories and exits 0 or 1", async () => {
    const s = await sandbox("cmp-cli");
    const a = await packageInto(s, "m1");
    const b = await packageInto(s, "m2");
    vi.spyOn(console, "log").mockImplementation(() => {});
    vi.spyOn(console, "error").mockImplementation(() => {});
    expect(await compareMain([a.runtimeDir, join(b.runtimeDir, "manifest.json")])).toBe(0);
    expect((await loadManifest(a.runtimeDir)).source_key).toBe(a.sourceKey);
    const c = await packageInto(await sandbox("cmp-cli2", { abiHash: "c".repeat(64) }), "m");
    expect(await compareMain([a.runtimeDir, c.runtimeDir, "--files", "strict"])).toBe(1);
    expect(await compareMain([a.runtimeDir])).toBe(2);
    expect(await compareMain([a.runtimeDir, b.runtimeDir, "--files", "loose"])).toBe(2);
  });
});
