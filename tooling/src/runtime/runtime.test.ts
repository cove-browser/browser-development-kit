import {
  appendFileSync,
  chmodSync,
  cpSync,
  existsSync,
  lstatSync,
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
import { createArchive, extractArchive } from "./archive.js";
import { fileBackend, githubBackend } from "./backends.js";
import { canonicalJson } from "./canonical.js";
import { computeSourceIdentity, sourceKey } from "./identity.js";
import { type Manifest, parseManifest, verifyRuntimeDir } from "./manifest.js";
import { ContentMismatchError, MissingFilesError, packageRuntime } from "./packager.js";
import { parsePin, readPin, writePin } from "./pin.js";
import { publishRuntime } from "./publisher.js";
import { checkReproducible } from "./reproducible.js";
import { resolveRuntime } from "./resolver.js";
import { removeTree } from "./store.js";
import {
  BUILDER,
  FAKE_ABI_HASH,
  makeOut,
  makeSandbox,
  packageOptions,
  tempDir,
} from "./testing.js";

const cleanup: string[] = [];
afterEach(async () => {
  for (const d of cleanup.splice(0)) await removeTree(d);
});
function sandbox(label: string, fixture?: Parameters<typeof makeSandbox>[1]) {
  const s = makeSandbox(label, fixture);
  cleanup.push(s.root);
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
    const s = sandbox("identity");
    const base = await keyOf(s.repo);
    writeFileSync(join(s.repo, "chromium/patches/README.md"), "changed docs\n");
    expect(await keyOf(s.repo)).toBe(base);
    writeFileSync(join(s.repo, "chromium/bdk/engine.cc"), "int engine() { return 2; }\n");
    const overlay = await keyOf(s.repo);
    expect(overlay).not.toBe(base);
    writeFileSync(join(s.repo, "chromium/gn/release.gn"), "is_debug = true\n");
    expect(await keyOf(s.repo)).not.toBe(overlay);
  });

  it("changes with the ABI hash and refuses a pin/ABI version mismatch", async () => {
    const s = sandbox("abi");
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
    const s = sandbox("package");
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
    const s = sandbox("missing");
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
    const s = sandbox("guards");
    await expect(
      packageRuntime(packageOptions(s, { checkoutCommit: "0".repeat(40) })),
    ).rejects.toThrow(/pins/);
    await removeTree(join(s.repo, "chromium/gn"));
    await expect(packageRuntime(packageOptions(s))).rejects.toThrow(/GN preset/);
  });

  it("is idempotent for identical content and an error for different content", async () => {
    const s = sandbox("idem");
    const first = await packageRuntime(packageOptions(s));
    const again = await packageRuntime(
      packageOptions(s, { builder: { ...BUILDER, timestamp: "2027-01-01T00:00:00.000Z" } }),
    );
    expect(again.status).toBe("unchanged");
    expect(again.manifestSha256).toBe(first.manifestSha256);
    makeOut(s.outDir, "two");
    await expect(packageRuntime(packageOptions(s))).rejects.toBeInstanceOf(ContentMismatchError);
  });

  it("rejects a symlink that points outside the runtime", async () => {
    const s = sandbox("escape");
    symlinkSync("../../../../etc/passwd", join(s.outDir, "Chromium.app", "Contents", "evil"));
    await expect(packageRuntime(packageOptions(s))).rejects.toThrow(/outside the runtime/);
  });
});

function readdirNoTemp(dir: string): string[] {
  return readdirSync(dir).filter((n) => !n.startsWith(".tmp-"));
}

describe("T-CHR-09 resolver (feasibility 9)", () => {
  it("case 1: no runtime fails naming the key and the remedies", async () => {
    const s = sandbox("case1");
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
    const s = sandbox("case2");
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
    const s = sandbox("case3");
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
    const s = sandbox("case4");
    await packageRuntime(packageOptions(s));
    await resolveRuntime({ repoRoot: s.repo, paths: s.paths });
    const link = join(s.repo, ".bdk/runtime");
    expect(() => writeFileSync(join(link, "Chromium.app/Contents/Info.plist"), "x")).toThrow(
      /EACCES|EPERM/,
    );
    expect(() => writeFileSync(join(link, "new-file"), "x")).toThrow(/EACCES|EPERM/);
  });

  it("case 6: a changed patch changes the key; the other worktree is unaffected", async () => {
    const s = sandbox("case6");
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
    const s = sandbox("case10");
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
    const s = sandbox("case13");
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
    const s = sandbox("source");
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
    const s = sandbox(label);
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
    const s = sandbox("dirty");
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
    const s = sandbox("nopkg");
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
    const s = sandbox("archive");
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
    const s = sandbox("repro");
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
