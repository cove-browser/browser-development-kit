import { execFileSync } from "node:child_process";
import {
  appendFileSync,
  chmodSync,
  cpSync,
  existsSync,
  lstatSync,
  mkdirSync,
  readdirSync,
  readFileSync,
  readlinkSync,
  realpathSync,
  rmSync,
  statSync,
  symlinkSync,
  writeFileSync,
} from "node:fs";
import { join } from "node:path";
import { afterEach, describe, expect, it } from "vitest";
import { bdkPaths } from "../paths.js";
import { assertAbiHashCurrent } from "./abi.js";
import { createArchive, extractArchive } from "./archive.js";
import { fileBackend, githubBackend } from "./backends.js";
import { canonicalJson } from "./canonical.js";
import { computeSourceIdentity, sourceKey } from "./identity.js";
import { type Manifest, parseManifest, verifyRuntimeDir } from "./manifest.js";
import {
  ContentMismatchError,
  DamagedStoreEntryError,
  MissingFilesError,
  PackagingError,
  packageRuntime,
  SAMPLE_CREDITS_MARKER,
} from "./packager.js";
import { parsePin, readPin, writePin } from "./pin.js";
import { publishRuntime } from "./publisher.js";
import { checkReproducible } from "./reproducible.js";
import { resolveRuntime } from "./resolver.js";
import { readRuntimeFiles } from "./runtime-files.js";
import { BUILD_STAMP_NAME, writeBuildStamp } from "./stamp.js";
import { removeTree } from "./store.js";
import {
  BUILDER,
  FAKE_ABI_HASH,
  makeOut,
  makeSandbox,
  NOTICES_FROM,
  packageOptions,
  stampOut,
  tempDir,
} from "./testing.js";

const cleanup: string[] = [];
afterEach(async () => {
  for (const d of cleanup.splice(0)) await removeTree(d);
});
async function sandbox(label: string, fixture?: Parameters<typeof makeSandbox>[1]) {
  const s = makeSandbox(label, fixture);
  cleanup.push(s.root);
  await stampOut(s);
  return s;
}
const keyOf = async (repo: string) => sourceKey(await computeSourceIdentity({ repoRoot: repo }));

describe("canonical JSON and identity", () => {
  it("sorts keys at every depth and drops whitespace", () => {
    expect(canonicalJson({ b: 1, a: { d: [{ z: 1, y: 2 }], c: null } })).toBe(
      '{"a":{"c":null,"d":[{"y":2,"z":1}]},"b":1}',
    );
  });

  it("does not depend on README edits but does on patch, overlay and GN arguments", async () => {
    const s = await sandbox("identity");
    const base = await keyOf(s.repo);
    writeFileSync(join(s.repo, "chromium/patches/README.md"), "changed docs\n");
    expect(await keyOf(s.repo)).toBe(base);
    writeFileSync(join(s.repo, "chromium/bdk/engine.cc"), "int engine() { return 2; }\n");
    const overlay = await keyOf(s.repo);
    expect(overlay).not.toBe(base);
    writeFileSync(join(s.repo, "chromium/gn/release.gn"), "is_debug = true\n");
    expect(await keyOf(s.repo)).not.toBe(overlay);
  });

  it("changes with chromium/runtime-files.json: its paths and target.min_os", async () => {
    const s = await sandbox("inventory");
    const base = await keyOf(s.repo);
    const file = join(s.repo, "chromium/runtime-files.json");
    const original = readFileSync(file, "utf8");
    const write = (paths: string[], minOs: string) =>
      writeFileSync(
        file,
        JSON.stringify({ schema: 1, target: { os: "macos", min_os: minOs }, paths }),
      );
    write(["Chromium.app", "Required.framework"], "13.0");
    const morePaths = await keyOf(s.repo);
    expect(morePaths).not.toBe(base);
    write(["Chromium.app"], "99.0");
    expect(await keyOf(s.repo)).not.toBe(base);
    // Formatting and path order are not content.
    writeFileSync(file, original.replace(/\s+/g, " "));
    expect(await keyOf(s.repo)).toBe(base);
    write(["Required.framework", "Chromium.app"], "13.0");
    expect(await keyOf(s.repo)).toBe(morePaths);
  });

  it("changes with the ABI hash and refuses a pin/ABI version mismatch", async () => {
    const s = await sandbox("abi");
    const base = await keyOf(s.repo);
    writeFileSync(join(s.repo, "packages/core/generated/abi-hash.txt"), `${"b".repeat(64)}\n`);
    expect(await keyOf(s.repo)).not.toBe(base);
    writeFileSync(
      join(s.repo, "packages/core/include/bdk/bdk_base.h"),
      "#define BDK_ABI_VERSION 2\n",
    );
    await expect(keyOf(s.repo)).rejects.toThrow(/ABI version mismatch/);
  });
});

describe("T-CHR-08 packaging", () => {
  it("writes a manifest whose digests match the files, with symlinks kept as symlinks", async () => {
    const s = await sandbox("package");
    const r = await packageRuntime(packageOptions(s));
    expect(r.status).toBe("created");
    const verified = await verifyRuntimeDir(r.runtimeDir, r.sourceKey);
    expect(verified.manifestSha256).toBe(r.manifestSha256);
    const m: Manifest = parseManifest(readFileSync(join(r.runtimeDir, "manifest.json"), "utf8"));
    expect(m.schema).toBe(1);
    expect(m.attestation).toBe("");
    expect(m.bdk_abi).toEqual({ version: 1, hash: FAKE_ABI_HASH });
    expect(m.chromium.deps_digest).toMatch(/^[0-9a-f]{64}$/);
    const fw = "Chromium.app/Contents/Frameworks/Chromium Framework.framework";
    expect(m.files[`${fw}/Versions/Current`]).toBe("symlink:158.0.8127.37");
    expect(m.files[`${fw}/Chromium Framework`]).toBe("symlink:Versions/Current/Chromium Framework");
    expect(m.files["Chromium.app/Contents/MacOS/Chromium"]).toMatch(/^sha256:[0-9a-f]{64}:exec$/);
    expect(m.files[`${fw}/Versions/158.0.8127.37/Chromium Framework`]).toMatch(
      /^sha256:[0-9a-f]{64}$/,
    );
    expect(lstatSync(join(r.runtimeDir, fw, "Versions", "Current")).isSymbolicLink()).toBe(true);
    expect(readlinkSync(join(r.runtimeDir, fw, "Versions", "Current"))).toBe("158.0.8127.37");
  });

  it("fails when a declared file is missing and names it", async () => {
    const s = await sandbox("missing");
    writeFileSync(
      join(s.repo, "chromium/runtime-files.json"),
      JSON.stringify({
        schema: 1,
        target: { os: "macos", min_os: "13.0" },
        paths: ["Chromium.app", "BDKRuntime.framework"],
      }),
    );
    const error = await packageRuntime(packageOptions(s)).catch((e: unknown) => e);
    expect(error).toBeInstanceOf(MissingFilesError);
    expect((error as Error).message).toContain("BDKRuntime.framework");
    expect(existsSync(s.paths.runtimes) ? readdirNoTemp(s.paths.runtimes) : []).toEqual([]);
  });

  it("refuses a checkout that is not at the pin and a missing GN preset", async () => {
    const s = await sandbox("guards");
    await expect(
      packageRuntime(packageOptions(s, { checkoutBase: "0".repeat(40) })),
    ).rejects.toThrow(/pins/);
    await removeTree(join(s.repo, "chromium/gn"));
    await expect(packageRuntime(packageOptions(s))).rejects.toThrow(/GN preset/);
  });

  it("is idempotent for identical content and an error for different content", async () => {
    const s = await sandbox("idem");
    const first = await packageRuntime(packageOptions(s));
    const again = await packageRuntime(
      packageOptions(s, { builder: { ...BUILDER, timestamp: "2027-01-01T00:00:00.000Z" } }),
    );
    expect(again.status).toBe("unchanged");
    expect(again.manifestSha256).toBe(first.manifestSha256);
    makeOut(s.outDir, "two");
    await stampOut(s);
    await expect(packageRuntime(packageOptions(s))).rejects.toBeInstanceOf(ContentMismatchError);
  });

  it("rejects a symlink that points outside the runtime", async () => {
    const s = await sandbox("escape");
    symlinkSync("../../../../etc/passwd", join(s.outDir, "Chromium.app", "Contents", "evil"));
    await expect(packageRuntime(packageOptions(s))).rejects.toThrow(/outside the runtime/);
  });
});

describe("packaging guards", () => {
  it("rejects a symlink chain that escapes the runtime through an alias to '.'", async () => {
    const s = await sandbox("chain");
    symlinkSync(".", join(s.outDir, "Chromium.app", "alias"));
    symlinkSync("alias/../../outside.bin", join(s.outDir, "Chromium.app", "escape"));
    mkdirSync(s.paths.runtimes, { recursive: true });
    writeFileSync(join(s.paths.runtimes, "outside.bin"), "outside");
    const error = await packageRuntime(packageOptions(s)).catch((e: unknown) => e as Error);
    expect((error as Error).message).toMatch(/outside the runtime[\s\S]*escape -> alias/);
    expect(readdirNoTemp(s.paths.runtimes)).toEqual(["outside.bin"]);
  });

  it("accepts in-tree link chains, dangling in-tree links, and rejects absolute targets", async () => {
    const s = await sandbox("chain-ok");
    const app = join(s.outDir, "Chromium.app");
    symlinkSync(".", join(app, "alias"));
    symlinkSync("alias/Contents/Info.plist", join(app, "inside"));
    symlinkSync("Contents/not-there", join(app, "dangling"));
    await expect(packageRuntime(packageOptions(s))).resolves.toMatchObject({ status: "created" });
    const s2 = await sandbox("abs");
    symlinkSync("/etc/passwd", join(s2.outDir, "Chromium.app", "abs"));
    await expect(packageRuntime(packageOptions(s2))).rejects.toThrow(/Absolute symlink/);
  });

  it("the store verifier also rejects an escaping link added to an entry", async () => {
    const s = await sandbox("verify-link");
    const r = await packageRuntime(packageOptions(s));
    chmodSync(r.runtimeDir, 0o755);
    const app = join(r.runtimeDir, "Chromium.app");
    chmodSync(app, 0o755);
    symlinkSync("../../outside", join(app, "esc"));
    await expect(verifyRuntimeDir(r.runtimeDir, r.sourceKey)).rejects.toThrow(
      /escaping symlink: Chromium\.app\/esc/,
    );
  });

  it("re-packaging over a damaged store entry fails instead of reporting unchanged", async () => {
    const s = await sandbox("damaged");
    const r = await packageRuntime(packageOptions(s));
    const victim = join(r.runtimeDir, "Chromium.app/Contents/MacOS/Chromium");
    chmodSync(victim, 0o755);
    writeFileSync(victim, "tampered");
    const error = await packageRuntime(packageOptions(s)).catch((e: unknown) => e as Error);
    expect(error).toBeInstanceOf(DamagedStoreEntryError);
    expect((error as Error).message).toContain(r.runtimeDir);
    expect((error as Error).message).toContain("modified: Chromium.app/Contents/MacOS/Chromium");
    expect((error as Error).message).toContain("rm -rf");
    await removeTree(r.runtimeDir);
    await expect(packageRuntime(packageOptions(s))).resolves.toMatchObject({ status: "created" });
  });
});

describe("an application's patches (source mode, T-CLI-03)", () => {
  function appPatches(root: string, text = "From 0 Mon Sep 17 00:00:00 2001\n"): string {
    const dir = join(root, "app", "Chromium", "patches");
    mkdirSync(dir, { recursive: true });
    writeFileSync(join(dir, "series"), "app-change.patch\n");
    writeFileSync(join(dir, "app-change.patch"), text);
    return dir;
  }

  it("are recorded in the identity and the manifest, and give the runtime its own key", async () => {
    const s = await sandbox("app-patches");
    const dir = appPatches(s.root);
    const plain = await computeSourceIdentity({ repoRoot: s.repo });
    const withApp = await computeSourceIdentity({ repoRoot: s.repo, appPatchesDir: dir });
    expect(plain.application).toBeUndefined();
    expect(withApp.application).toEqual({
      patches_digest: expect.stringMatching(/^[0-9a-f]{64}$/),
      patches: ["app-change.patch"],
    });
    expect(sourceKey(withApp)).not.toBe(sourceKey(plain));
    await writeBuildStamp(s.outDir, withApp);
    const r = await packageRuntime(packageOptions(s, { appPatchesDir: dir }));
    expect(r.sourceKey).toBe(sourceKey(withApp));
    const m = parseManifest(readFileSync(join(r.runtimeDir, "manifest.json"), "utf8"));
    expect(m.application).toEqual(withApp.application);
    // A changed application patch is a different runtime.
    writeFileSync(join(dir, "app-change.patch"), "From 1 Mon Sep 17 00:00:00 2001\n");
    const changed = await computeSourceIdentity({ repoRoot: s.repo, appPatchesDir: dir });
    expect(sourceKey(changed)).not.toBe(r.sourceKey);
  });

  it("leave the key alone when the application's series is empty", async () => {
    const s = await sandbox("app-patches-empty");
    const dir = appPatches(s.root);
    writeFileSync(join(dir, "series"), "# none yet\n");
    expect(await keyOf(s.repo)).toBe(
      sourceKey(await computeSourceIdentity({ repoRoot: s.repo, appPatchesDir: dir })),
    );
  });

  it("refuse packaging when the build was stamped without them", async () => {
    const s = await sandbox("app-patches-stale");
    const dir = appPatches(s.root);
    await expect(packageRuntime(packageOptions(s, { appPatchesDir: dir }))).rejects.toThrow(
      /application/,
    );
  });
});

describe("build stamp (out/<config> must match the identity being packaged)", () => {
  it("packages when the stamp matches, and the stamp is not part of the runtime", async () => {
    const s = await sandbox("stamp-ok");
    expect(existsSync(join(s.outDir, BUILD_STAMP_NAME))).toBe(true);
    const r = await packageRuntime(packageOptions(s));
    expect(Object.keys(r.manifest.files).some((f) => f.includes(BUILD_STAMP_NAME))).toBe(false);
    expect(existsSync(join(r.runtimeDir, BUILD_STAMP_NAME))).toBe(false);
  });

  it("refuses an out dir with no stamp", async () => {
    const s = await sandbox("stamp-missing");
    rmSync(join(s.outDir, BUILD_STAMP_NAME));
    await expect(packageRuntime(packageOptions(s))).rejects.toThrow(/no bdk-build-identity\.json/);
  });

  it("refuses a stale stamp (overlay or GN args changed after the build) naming the fields", async () => {
    const s = await sandbox("stamp-stale");
    writeFileSync(join(s.repo, "chromium/bdk/engine.cc"), "int engine() { return 2; }\n");
    const overlay = await packageRuntime(packageOptions(s)).catch((e: unknown) => e as Error);
    expect((overlay as Error).message).toMatch(/built from different inputs/);
    expect((overlay as Error).message).toContain("bdk.overlay_digest");
    writeFileSync(join(s.repo, "chromium/gn/release.gn"), "is_debug = true\n");
    await expect(packageRuntime(packageOptions(s))).rejects.toThrow(/bdk\.gn_args_digest/);
    expect(existsSync(s.paths.runtimes) ? readdirNoTemp(s.paths.runtimes) : []).toEqual([]);
    await stampOut(s);
    await expect(packageRuntime(packageOptions(s))).resolves.toMatchObject({ status: "created" });
  });
});

describe("ABI hash freshness (packager only)", () => {
  const script = join(import.meta.dirname, "..", "..", "..", "packages/core/cmake/abi-hash.cmake");
  /** A sandbox repo with the real abi-hash.cmake and its header, and the hash cmake computes. */
  async function withCmake(label: string, stale: boolean) {
    const s = await sandbox(label);
    mkdirSync(join(s.repo, "packages/core/cmake"), { recursive: true });
    cpSync(script, join(s.repo, "packages/core/cmake/abi-hash.cmake"));
    execFileSync("cmake", ["-DMODE=generate", "-P", "packages/core/cmake/abi-hash.cmake"], {
      cwd: s.repo,
      stdio: "ignore",
    });
    if (stale)
      writeFileSync(join(s.repo, "packages/core/generated/abi-hash.txt"), `${"0".repeat(64)}\n`);
    await stampOut(s);
    return s;
  }

  it("packages when the generated hash is current", async () => {
    const s = await withCmake("abi-fresh", false);
    await expect(assertAbiHashCurrent(s.repo)).resolves.toBeUndefined();
    const { verifyAbiHash: _skip, ...real } = packageOptions(s);
    await expect(packageRuntime(real)).resolves.toMatchObject({ status: "created" });
  });

  it("refuses a drifted hash and surfaces the cmake message", async () => {
    const s = await withCmake("abi-stale", true);
    const { verifyAbiHash: _skip, ...real } = packageOptions(s);
    const error = await packageRuntime(real).catch((e: unknown) => e as Error);
    expect((error as Error).message).toMatch(/generated ABI hash is out of date/);
    expect((error as Error).message).toMatch(/generate/);
    expect(existsSync(s.paths.runtimes) ? readdirNoTemp(s.paths.runtimes) : []).toEqual([]);
  });
});

function readdirNoTemp(dir: string): string[] {
  return readdirSync(dir).filter((n) => !n.startsWith(".tmp-"));
}

describe("T-CHR-09 resolver (feasibility 9)", () => {
  it("case 1: no runtime fails naming the key and the remedies", async () => {
    const s = await sandbox("case1");
    const key = await keyOf(s.repo);
    const error = await resolveRuntime({ repoRoot: s.repo, paths: s.paths }).catch(
      (e: unknown) => e as Error,
    );
    expect((error as Error).message).toContain(key);
    expect((error as Error).message).toContain("BDK_RUNTIME_MODE=source");
    expect((error as Error).message).toContain("pnpm chromium:build && pnpm chromium:package");
    expect(existsSync(join(s.repo, ".bdk"))).toBe(false);
  });

  it("case 2: a published runtime is read-only and complete", async () => {
    const s = await sandbox("case2");
    const r = await packageRuntime(packageOptions(s));
    for (const rel of [
      "manifest.json",
      "Chromium.app/Contents/Info.plist",
      "Chromium.app/Contents/MacOS/Chromium",
    ]) {
      expect(statSync(join(r.runtimeDir, rel)).mode & 0o222).toBe(0);
    }
    expect(statSync(r.runtimeDir).mode & 0o222).toBe(0);
    expect(
      statSync(join(r.runtimeDir, "Chromium.app/Contents/MacOS/Chromium")).mode & 0o111,
    ).not.toBe(0);
  });

  it("case 3: two worktrees resolve the same inode", async () => {
    const s = await sandbox("case3");
    await packageRuntime(packageOptions(s));
    const wtB = join(s.root, "repo-b");
    cpSync(s.repo, wtB, { recursive: true });
    const a = await resolveRuntime({ repoRoot: s.repo, paths: s.paths });
    const b = await resolveRuntime({ repoRoot: wtB, paths: s.paths });
    expect(a.runtimeDir).toBe(b.runtimeDir);
    expect(statSync(join(s.repo, ".bdk/runtime")).ino).toBe(
      statSync(join(wtB, ".bdk/runtime")).ino,
    );
    expect(realpathSync(join(wtB, ".bdk/runtime"))).toBe(realpathSync(a.runtimeDir));
    const pointer = JSON.parse(readFileSync(join(s.repo, ".bdk/runtime.json"), "utf8"));
    expect(pointer).toEqual({ source_key: a.sourceKey, manifest_sha256: a.manifestSha256 });
  });

  it("case 4: writing into the runtime is refused", async () => {
    const s = await sandbox("case4");
    await packageRuntime(packageOptions(s));
    await resolveRuntime({ repoRoot: s.repo, paths: s.paths });
    const link = join(s.repo, ".bdk/runtime");
    expect(() => writeFileSync(join(link, "Chromium.app/Contents/Info.plist"), "x")).toThrow(
      /EACCES|EPERM/,
    );
    expect(() => writeFileSync(join(link, "new-file"), "x")).toThrow(/EACCES|EPERM/);
  });

  it("case 6: a changed patch changes the key; the other worktree is unaffected", async () => {
    const s = await sandbox("case6");
    await packageRuntime(packageOptions(s));
    const wtB = join(s.root, "repo-b");
    cpSync(s.repo, wtB, { recursive: true });
    writeFileSync(join(wtB, "chromium/patches/0001-example.patch"), "--- a\n+++ changed\n");
    await expect(resolveRuntime({ repoRoot: wtB, paths: s.paths })).rejects.toThrow(
      /No runtime for source key/,
    );
    await expect(resolveRuntime({ repoRoot: s.repo, paths: s.paths })).resolves.toMatchObject({
      origin: "store",
    });
  });

  it("cases 10-12: tamper, extra file, retargeted symlink and eviction are detected on every run", async () => {
    const s = await sandbox("case10");
    const r = await packageRuntime(packageOptions(s));
    const opts = { repoRoot: s.repo, paths: s.paths };
    await resolveRuntime(opts);

    const victim = join(r.runtimeDir, "Chromium.app/Contents/MacOS/Chromium");
    chmodSync(victim, 0o755);
    appendFileSync(victim, "evil");
    await expect(resolveRuntime(opts)).rejects.toThrow(
      /modified: Chromium\.app\/Contents\/MacOS\/Chromium/,
    );
    writeFileSync(victim, readFileSync(victim).subarray(0, -4));
    chmodSync(victim, 0o555);
    await expect(resolveRuntime(opts)).resolves.toBeDefined();

    chmodSync(r.runtimeDir, 0o755);
    writeFileSync(join(r.runtimeDir, "extra"), "x");
    await expect(resolveRuntime(opts)).rejects.toThrow(/unexpected: extra/);
    rmSync(join(r.runtimeDir, "extra"), { force: true });
    chmodSync(r.runtimeDir, 0o555);
    await expect(resolveRuntime(opts)).resolves.toBeDefined();

    await removeTree(r.runtimeDir);
    await expect(resolveRuntime(opts)).rejects.toThrow(/No runtime for source key/);
  });

  it("case 13: a toolchain change keeps the source key and changes the manifest digest", async () => {
    const s = await sandbox("case13");
    const a = await packageRuntime(packageOptions(s));
    const otherHome = join(s.root, "home-b");
    const b = await packageRuntime(
      packageOptions(s, {
        paths: bdkPaths(otherHome),
        builder: { ...BUILDER, xcode: "Xcode 28.0", clang: "clang 24" },
      }),
    );
    expect(b.sourceKey).toBe(a.sourceKey);
    expect(b.manifestSha256).not.toBe(a.manifestSha256);
    expect(b.manifest.files).toEqual(a.manifest.files);
  });

  it("BDK_RUNTIME_MODE=source never downloads", async () => {
    const s = await sandbox("source");
    await packageRuntime(packageOptions(s));
    const dest = tempDir("dest");
    cleanup.push(dest);
    await publishRuntime({
      repoRoot: s.repo,
      paths: s.paths,
      backend: fileBackend(`file://${dest}`),
      gitStatus: async () => "",
    });
    const home2 = bdkPaths(join(s.root, "home2"));
    const downloader = async () => {
      throw new Error("should not download");
    };
    await expect(
      resolveRuntime({ repoRoot: s.repo, paths: home2, mode: "source", downloader }),
    ).rejects.toThrow(/BDK_RUNTIME_MODE=source/);
  });
});

describe("T-REP-05 publish and second machine", () => {
  async function published(label: string) {
    const s = await sandbox(label);
    await packageRuntime(packageOptions(s));
    const dest = tempDir("release");
    cleanup.push(dest);
    const result = await publishRuntime({
      repoRoot: s.repo,
      paths: s.paths,
      backend: fileBackend(`file://${dest}`),
      gitStatus: async () => "",
    });
    return { s, dest, result };
  }

  it("refuses a store entry whose manifest was rewritten to disagree with pin.json", async () => {
    const { s } = await published("rewritten");
    const pinned = readPin(s.repo).runtime?.manifest_sha256;
    const dir = s.paths.runtime(await keyOf(s.repo));
    await expect(resolveRuntime({ repoRoot: s.repo, paths: s.paths })).resolves.toMatchObject({
      manifestSha256: pinned,
    });
    chmodSync(dir, 0o755);
    await removeTree(join(dir, "Chromium.app"));
    chmodSync(join(dir, "manifest.json"), 0o644);
    const m = JSON.parse(readFileSync(join(dir, "manifest.json"), "utf8")) as Manifest;
    writeFileSync(join(dir, "manifest.json"), JSON.stringify({ ...m, files: {} }));
    const error = (await resolveRuntime({ repoRoot: s.repo, paths: s.paths }).catch(
      (e: unknown) => e,
    )) as { code?: string; message: string };
    expect(error.code).toBe("integrity");
    expect(error.message).toContain(pinned);
  });

  it("refuses a self-consistent entry with a different ABI hash than pin.json records", async () => {
    const { s } = await published("abi-rewritten");
    const dir = s.paths.runtime(await keyOf(s.repo));
    chmodSync(dir, 0o755);
    chmodSync(join(dir, "manifest.json"), 0o644);
    const m = JSON.parse(readFileSync(join(dir, "manifest.json"), "utf8")) as Manifest;
    writeFileSync(
      join(dir, "manifest.json"),
      JSON.stringify({ ...m, bdk_abi: { ...m.bdk_abi, hash: "0".repeat(64) } }),
    );
    await expect(resolveRuntime({ repoRoot: s.repo, paths: s.paths })).rejects.toMatchObject({
      code: "integrity",
    });
  });

  it("records the pin runtime record and resolves on a second machine", async () => {
    const { s, result } = await published("pub");
    const pin = readPin(s.repo);
    expect(pin.runtime).toEqual(result.record);
    expect(pin.runtime?.url).toMatch(/^file:\/\/\/.*bdk-runtime-[0-9a-f]{16}\.tar\.(zst|gz)$/);
    expect(pin.runtime?.abi_hash).toBe(FAKE_ABI_HASH);
    const manifestOnDisk = parseManifest(
      readFileSync(join(s.paths.runtime(pin.runtime?.source_key ?? ""), "manifest.json"), "utf8"),
    );
    expect(pin.runtime?.source_key).toBe(manifestOnDisk.source_key);

    const machine2 = bdkPaths(join(s.root, "machine2"));
    const repo2 = join(s.root, "repo2");
    cpSync(s.repo, repo2, { recursive: true });
    const r = await resolveRuntime({ repoRoot: repo2, paths: machine2 });
    expect(r.origin).toBe("downloaded");
    expect(r.manifestSha256).toBe(pin.runtime?.manifest_sha256);
    await verifyRuntimeDir(r.runtimeDir, r.sourceKey);
    expect(statSync(join(r.runtimeDir, "manifest.json")).mode & 0o222).toBe(0);
    const again = await resolveRuntime({ repoRoot: repo2, paths: machine2 });
    expect(again.origin).toBe("store");
  });

  it("refuses a tampered archive and installs nothing", async () => {
    const { s, result } = await published("tamper");
    const archive = new URL(result.record.url).pathname;
    appendFileSync(archive, "tamper");
    const machine2 = bdkPaths(join(s.root, "machine2"));
    const error = await resolveRuntime({ repoRoot: s.repo, paths: machine2 }).catch(
      (e: unknown) => e as Error,
    );
    expect(error).toMatchObject({ code: "digest" });
    expect((error as Error).message).toMatch(/archive SHA-256/);
    expect(existsSync(machine2.runtime(result.record.source_key))).toBe(false);
  });

  it("refuses a manifest digest that differs from pin.json", async () => {
    const { s, result } = await published("manifestdigest");
    const pin = readPin(s.repo);
    writePin(s.repo, { ...pin, runtime: { ...result.record, manifest_sha256: "0".repeat(64) } });
    const machine2 = bdkPaths(join(s.root, "machine2"));
    const error = await resolveRuntime({ repoRoot: s.repo, paths: machine2 }).catch(
      (e: unknown) => e as Error,
    );
    expect(error).toMatchObject({ code: "digest" });
    expect((error as Error).message).toMatch(/manifest digest/);
    expect(existsSync(machine2.runtime(result.record.source_key))).toBe(false);
  });

  it("reports a rebuild is needed when the pin record is for another key", async () => {
    const { s } = await published("stale");
    writeFileSync(join(s.repo, "chromium/patches/0001-example.patch"), "--- changed\n");
    const machine2 = bdkPaths(join(s.root, "machine2"));
    await expect(resolveRuntime({ repoRoot: s.repo, paths: machine2 })).rejects.toThrow(
      /changed since it was published/,
    );
  });

  it("refuses to publish from a dirty tree", async () => {
    const s = await sandbox("dirty");
    await packageRuntime(packageOptions(s));
    const dest = tempDir("release");
    cleanup.push(dest);
    await expect(
      publishRuntime({
        repoRoot: s.repo,
        paths: s.paths,
        backend: fileBackend(`file://${dest}`),
        gitStatus: async () => " M chromium/pin.json\n",
      }),
    ).rejects.toThrow(/dirty tree/);
    expect(readPin(s.repo).runtime).toBeNull();
  });

  it("refuses to publish a runtime that is not packaged or has been tampered with", async () => {
    const s = await sandbox("nopkg");
    const dest = tempDir("release");
    cleanup.push(dest);
    const opts = {
      repoRoot: s.repo,
      paths: s.paths,
      backend: fileBackend(`file://${dest}`),
      gitStatus: async () => "",
    };
    await expect(publishRuntime(opts)).rejects.toThrow(/No intact packaged runtime/);
  });

  it("the github backend runs gh release create and upload without --clobber (fake runner)", async () => {
    const calls: string[][] = [];
    const backend = githubBackend({
      run: async (cmd, args) => {
        calls.push([cmd, ...args]);
        if (cmd === "git")
          return { stdout: "git@github.com:cove-browser/browser-development-kit.git\n" };
        if (args[1] === "view") throw new Error("release not found");
        return { stdout: "" };
      },
    });
    const { url } = await backend.upload({
      archivePath: "/x/bdk-runtime-abc.tar.zst",
      sourceKey: "abc",
    });
    expect(url).toBe(
      "https://github.com/cove-browser/browser-development-kit/releases/download/runtime-abc/bdk-runtime-abc.tar.zst",
    );
    expect(calls.map((c) => c.slice(0, 3).join(" "))).toEqual([
      "git -C .",
      "gh release view",
      "gh release create",
      "gh release upload",
    ]);
    expect(calls.flat()).not.toContain("--clobber");
  });
});

describe("archives and reproducibility", () => {
  it("archives are deterministic and round-trip symlinks and modes", async () => {
    const s = await sandbox("archive");
    const r = await packageRuntime(packageOptions(s));
    const dest = tempDir("arch");
    cleanup.push(dest);
    for (const format of ["tar.gz", "tar.zst"] as const) {
      const a = await createArchive(r.runtimeDir, join(dest, `a.${format}`), format);
      const b = await createArchive(r.runtimeDir, join(dest, `b.${format}`), format);
      expect(a.sha256).toBe(b.sha256);
      const out = join(dest, `out-${format}`);
      await extractArchive(join(dest, `a.${format}`), out, format);
      await verifyRuntimeDir(out, r.sourceKey);
    }
  });

  it("packaging the same inputs twice gives the same files and manifests apart from the timestamp", async () => {
    const s = await sandbox("repro");
    const { paths: _paths, ...opts } = packageOptions(s);
    const report = await checkReproducible(opts);
    expect(report.differences).toEqual([]);
    expect(report.reproducible).toBe(true);
  });
});

describe("pin.json schema", () => {
  it("accepts null and a complete record; rejects malformed records", () => {
    const base = { tag: "1", commit: "c", bdk_abi_version: 1 };
    expect(parsePin(JSON.stringify({ ...base, runtime: null })).runtime).toBeNull();
    const rec = {
      source_key: "k",
      url: "https://example.com/x.tar.zst",
      archive_format: "tar.zst",
      archive_sha256: "a".repeat(64),
      manifest_sha256: "b".repeat(64),
      abi_hash: "c".repeat(64),
      size: 5,
    };
    expect(parsePin(JSON.stringify({ ...base, runtime: rec })).runtime).toEqual(rec);
    expect(() =>
      parsePin(JSON.stringify({ ...base, runtime: { ...rec, url: "http://x/y" } })),
    ).toThrow(/https/);
    expect(() =>
      parsePin(JSON.stringify({ ...base, runtime: { ...rec, archive_sha256: "zz" } })),
    ).toThrow(/hex/);
  });

  it("the repository's own pin.json parses", () => {
    expect(() => readPin(join(import.meta.dirname, "..", "..", ".."))).not.toThrow();
  });
});

describe("T-REL-02 third-party notices in the packaged runtime", () => {
  it("copies Chromium's credits to THIRD_PARTY_NOTICES.html and lists it in the manifest", async () => {
    const s = await sandbox("notices", { notices: true });
    const r = await packageRuntime(packageOptions(s));
    expect(r.manifest.files["THIRD_PARTY_NOTICES.html"]).toMatch(/^sha256:[0-9a-f]{64}$/);
    expect(readFileSync(join(r.runtimeDir, "THIRD_PARTY_NOTICES.html"), "utf8")).toContain(
      "libfoo",
    );
    await verifyRuntimeDir(r.runtimeDir, r.sourceKey);
  });

  it("refuses Chromium's placeholder credits page", async () => {
    const s = await sandbox("notices-sample", { notices: true });
    writeFileSync(join(s.outDir, NOTICES_FROM), `<html>${SAMPLE_CREDITS_MARKER}</html>\n`);
    await expect(packageRuntime(packageOptions(s))).rejects.toThrow(PackagingError);
    await expect(packageRuntime(packageOptions(s))).rejects.toThrow(/generate_about_credits/);
  });

  it("fails naming the notices when the build wrote none", async () => {
    const s = await sandbox("notices-missing", { notices: true });
    rmSync(join(s.outDir, NOTICES_FROM));
    const error = await packageRuntime(packageOptions(s)).catch((e: unknown) => e);
    expect(error).toBeInstanceOf(MissingFilesError);
    expect((error as MissingFilesError).missing).toEqual([NOTICES_FROM]);
  });

  it("copies the notices into a declared directory too, the framework's Resources", async () => {
    const alsoIn = "Chromium.app/Contents/Frameworks/Chromium Framework.framework/Resources";
    const s = await sandbox("notices-also", { notices: true, noticesAlsoIn: alsoIn });
    const file = join(s.repo, "chromium/runtime-files.json");
    const rf = JSON.parse(readFileSync(file, "utf8")) as Record<string, unknown>;
    const r = await packageRuntime(packageOptions(s));
    // Resources is a link to Versions/Current/Resources: the copy lands in the version directory.
    const inFramework =
      "Chromium.app/Contents/Frameworks/Chromium Framework.framework/Versions/158.0.8127.37/Resources/THIRD_PARTY_NOTICES.html";
    expect(r.manifest.files[inFramework]).toMatch(/^sha256:[0-9a-f]{64}$/);
    expect(readFileSync(join(r.runtimeDir, inFramework), "utf8")).toContain("libfoo");
    writeFileSync(
      file,
      JSON.stringify({
        ...rf,
        notices: {
          from: NOTICES_FROM,
          to: "THIRD_PARTY_NOTICES.html",
          also_in: "Elsewhere/Resources",
        },
      }),
    );
    expect(() => readRuntimeFiles(s.repo)).toThrow(/also_in/);
  });

  it("declaring notices changes the source key; notices.to must be a top-level name", async () => {
    const plain = await sandbox("notices-key1");
    const noted = await sandbox("notices-key2", { notices: true });
    expect(await keyOf(plain.repo)).not.toBe(await keyOf(noted.repo));
    const file = join(noted.repo, "chromium/runtime-files.json");
    const rf = JSON.parse(readFileSync(file, "utf8")) as Record<string, unknown>;
    writeFileSync(file, JSON.stringify({ ...rf, notices: { from: NOTICES_FROM, to: "a/b.html" } }));
    expect(() => readRuntimeFiles(noted.repo)).toThrow(/notices.to/);
  });

  it("the repository's runtime declares its notices", () => {
    const rf = readRuntimeFiles(join(import.meta.dirname, "..", "..", ".."));
    expect(rf.notices).toEqual({
      from: NOTICES_FROM,
      to: "THIRD_PARTY_NOTICES.html",
      also_in: "Chromium.app/Contents/Frameworks/Chromium Framework.framework/Resources",
    });
  });
});
