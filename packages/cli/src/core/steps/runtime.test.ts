import { existsSync } from "node:fs";
import { copyFile, readFile, rm, writeFile } from "node:fs/promises";
import { join, resolve } from "node:path";
import { createArchive } from "@bdk/tooling/dist/runtime/archive.js";
import { computeSourceIdentity, sourceKey } from "@bdk/tooling/dist/runtime/identity.js";
import type { RuntimeRecord } from "@bdk/tooling/dist/runtime/pin.js";
import { describe, expect, it } from "vitest";
import { createAssets } from "../../assets.js";
import {
  STORE_ENTRY,
  storeRuntime,
  tempDir,
  tempProject,
  testContext,
  withPublishedRuntime,
} from "../../test-support.js";
import type { Context } from "../context.js";
import { run } from "../run.js";

const REPO = resolve(import.meta.dirname, "../../../../..");

/** The pointer `storeRuntime` wrote, which is what the step must write for the same runtime. */
async function pointer(dir: string): Promise<{ source_key: string; manifest_sha256: string }> {
  return JSON.parse(await readFile(join(dir, ".bdk", "runtime.json"), "utf8"));
}

function record(manifestSha256: string, over: Partial<RuntimeRecord> = {}): RuntimeRecord {
  return {
    source_key: STORE_ENTRY,
    manifest_sha256: manifestSha256,
    abi_hash: JSON.parse(createAssets({ packaged: false }).readText("bdk-release.json")).abi.hash,
    url: "https://example.invalid/runtime.tar.gz",
    archive_format: "tar.gz",
    archive_sha256: "0".repeat(64),
    size: 1,
    ...over,
  };
}

async function build(dir: string, args: string[], over: Partial<Context> = {}) {
  const ctx = testContext({
    cwd: dir,
    ...over,
    env: { BDK_HOME: join(dir, ".home"), ...over.env },
  });
  const code = await run(["build", ...args, "--json"], ctx);
  const report = JSON.parse(ctx.out.join(""));
  return {
    code,
    result: report.result?.runtime,
    codes: (report.diagnostics ?? []).map((d: { code: string }) => d.code),
    execCalls: ctx.execCalls,
  };
}

/** A published runtime's archive, made from a runtime in another store, and the record that names it. */
async function published(): Promise<{ archive: string; record: RuntimeRecord }> {
  const other = await tempDir();
  const store = await storeRuntime(other, {
    files: { "Chromium.app/Contents/Info.plist": "<plist/>" },
  });
  const { manifest_sha256 } = await pointer(other);
  const archive = join(other, "runtime.tar.gz");
  const made = await createArchive(store, archive, "tar.gz");
  return {
    archive,
    record: record(manifest_sha256, { archive_sha256: made.sha256, size: made.size }),
  };
}

describe("bdk build runtime, prebuilt", () => {
  it("records the published runtime already in the store", async () => {
    const { dir } = await tempProject();
    await storeRuntime(dir, {});
    const expected = await pointer(dir);
    await rm(join(dir, ".bdk"), { recursive: true });
    const assets = withPublishedRuntime(record(expected.manifest_sha256));
    const { code, result } = await build(dir, ["runtime"], { assets });
    expect(code).toBe(0);
    expect(result).toEqual({
      mode: "prebuilt",
      sourceKey: STORE_ENTRY,
      manifestSha256: expected.manifest_sha256,
      origin: "store",
    });
    expect(await pointer(dir)).toEqual(expected);
  });

  it("downloads the published runtime into the store and verifies it", async () => {
    const { dir } = await tempProject();
    const { archive, record } = await published();
    const urls: string[] = [];
    const { code, result } = await build(dir, ["runtime"], {
      assets: withPublishedRuntime(record),
      downloadRuntime: async (url, dest) => {
        urls.push(url);
        await copyFile(archive, dest);
      },
    });
    expect(code).toBe(0);
    expect(urls).toEqual([record.url]);
    expect(result.origin).toBe("downloaded");
    expect(existsSync(join(dir, ".home", "runtimes", STORE_ENTRY, "Chromium.app"))).toBe(true);
    expect((await pointer(dir)).manifest_sha256).toBe(record.manifest_sha256);
  });

  it("runtime.digest_mismatch: installs nothing from an archive that is not the one recorded", async () => {
    const { dir } = await tempProject();
    const { archive, record } = await published();
    const { code, codes } = await build(dir, ["runtime"], {
      assets: withPublishedRuntime({ ...record, archive_sha256: "f".repeat(64) }),
      downloadRuntime: (_url, dest) => copyFile(archive, dest),
    });
    expect(codes).toEqual(["runtime.digest_mismatch"]);
    expect(code).toBe(4);
    expect(existsSync(join(dir, ".home", "runtimes", STORE_ENTRY))).toBe(false);
    expect(existsSync(join(dir, ".bdk", "runtime.json"))).toBe(false);
  });

  it("runtime.offline: does not download with --offline", async () => {
    const { dir } = await tempProject();
    const { record } = await published();
    const downloads: string[] = [];
    const { code, codes } = await build(dir, ["runtime", "--offline"], {
      assets: withPublishedRuntime(record),
      downloadRuntime: async (url) => {
        downloads.push(url);
      },
    });
    expect(codes).toEqual(["runtime.offline"]);
    expect(code).toBe(3);
    expect(downloads).toEqual([]);
  });

  it("runtime.store_modified: refuses a store entry that is not the published runtime", async () => {
    const { dir } = await tempProject();
    await storeRuntime(dir, {});
    const { code, codes } = await build(dir, ["runtime"], {
      assets: withPublishedRuntime(record("a".repeat(64))),
    });
    expect(codes).toEqual(["runtime.store_modified"]);
    expect(code).toBe(4);
  });

  it("runtime.digest_mismatch: installs nothing built for another ABI", async () => {
    const { dir } = await tempProject();
    const { archive, record } = await published();
    const { code, codes } = await build(dir, ["runtime"], {
      assets: withPublishedRuntime({ ...record, abi_hash: "e".repeat(64) }),
      downloadRuntime: (_url, dest) => copyFile(archive, dest),
    });
    expect(codes).toEqual(["runtime.digest_mismatch"]);
    expect(code).toBe(4);
    expect(existsSync(join(dir, ".home", "runtimes", STORE_ENTRY))).toBe(false);
  });

  it("runtime.unpublished: a release with no runtime", async () => {
    const { dir } = await tempProject();
    const { code, codes } = await build(dir, ["runtime"], { assets: withPublishedRuntime(null) });
    expect(codes).toEqual(["runtime.unpublished"]);
    expect(code).toBe(1);
  });

  it("is left out of `bdk build` when the release publishes no runtime", async () => {
    const { dir } = await tempProject();
    const { result } = await build(dir, [], { assets: withPublishedRuntime(null) });
    expect(result).toBeUndefined();
  });
});

describe("bdk build runtime, source", () => {
  const SOURCE = '[browser]\nname = "Demo"\n\n[runtime]\nmode = "source"\n';

  async function sourceProject() {
    const { dir } = await tempProject();
    await writeFile(join(dir, "bdk.toml"), SOURCE);
    return dir;
  }

  it("runtime.source_build_not_allowed: never starts a Chromium build unless asked", async () => {
    const dir = await sourceProject();
    const { code, codes, execCalls } = await build(dir, ["runtime"], {
      env: { BDK_SOURCE_ROOT: REPO },
    });
    expect(codes).toEqual(["runtime.source_build_not_allowed"]);
    expect(code).toBe(1);
    expect(execCalls).toEqual([]);
  });

  it("builds and packages the runtime with --allow-source-build, then records it", async () => {
    const dir = await sourceProject();
    const key = sourceKey(await computeSourceIdentity({ repoRoot: REPO }));
    const calls: string[][] = [];
    const { code, result } = await build(dir, ["runtime", "--allow-source-build"], {
      env: { BDK_SOURCE_ROOT: REPO },
      exec: async (command, args, options) => {
        calls.push([command, ...args, options.cwd]);
        // The packaging run puts the runtime in the store.
        if (args.includes("chromium:package")) await storeRuntime(dir, { key });
        return { code: 0, stdout: "", stderr: "" };
      },
    });
    expect(code).toBe(0);
    expect(calls.map((c) => c[4])).toEqual(["chromium:build", "chromium:package"]);
    expect(calls.every((c) => c.at(-1) === REPO)).toBe(true);
    expect(result).toMatchObject({ mode: "source", sourceKey: key, origin: "built" });
  });

  it("finds a source-built runtime in the store without building", async () => {
    const dir = await sourceProject();
    const key = sourceKey(await computeSourceIdentity({ repoRoot: REPO }));
    await storeRuntime(dir, { key });
    const { code, result, execCalls } = await build(dir, ["runtime"], {
      env: { BDK_SOURCE_ROOT: REPO },
    });
    expect(code).toBe(0);
    expect(execCalls).toEqual([]);
    expect(result).toMatchObject({ sourceKey: key, origin: "store" });
  });

  it("runtime.source_build_failed: names the target that failed", async () => {
    const dir = await sourceProject();
    const { code, codes } = await build(dir, ["runtime", "--allow-source-build"], {
      env: { BDK_SOURCE_ROOT: REPO },
      exec: async () => ({ code: 1, stdout: "", stderr: "patch 0004 does not apply" }),
    });
    expect(codes).toEqual(["runtime.source_build_failed"]);
    expect(code).toBe(1);
  });

  it("runtime.source_root_missing: needs a BDK checkout", async () => {
    const dir = await sourceProject();
    const { code, codes } = await build(dir, ["runtime"]);
    expect(codes).toEqual(["runtime.source_root_missing"]);
    expect(code).toBe(3);
  });

  it("takes BDK_RUNTIME_MODE over bdk.toml, and refuses another value", async () => {
    const dir = await sourceProject();
    const { codes } = await build(dir, ["runtime"], {
      env: { BDK_RUNTIME_MODE: "prebuilt" },
      assets: withPublishedRuntime(null),
    });
    expect(codes).toEqual(["runtime.unpublished"]);
    const bad = await build(dir, ["runtime"], { env: { BDK_RUNTIME_MODE: "other" } });
    expect(bad.code).toBe(2);
  });

  it("refuses --allow-source-build without the runtime step", async () => {
    const dir = await sourceProject();
    const { code } = await build(dir, ["config", "--allow-source-build"]);
    expect(code).toBe(2);
  });
});
