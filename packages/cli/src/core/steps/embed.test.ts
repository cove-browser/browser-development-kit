import { chmod, lstat, mkdir, readFile, readlink, stat, writeFile } from "node:fs/promises";
import { join } from "node:path";
import { describe, expect, it } from "vitest";
import { STORE_ENTRY, storeRuntime, tempProject, testContext } from "../../test-support.js";
import { run } from "../run.js";

const FW = "Chromium.app/Contents/Frameworks";
const APP = "Demo.app";

/** A runtime shaped as packaging makes one: a versioned framework, its symlinks, and a helper. */
const RUNTIME = {
  files: {
    "Chromium.app/Contents/Info.plist": "<plist/>",
    [`${FW}/Lib.framework/Versions/1/Lib`]: "binary",
    [`${FW}/Lib.framework/Versions/1/Resources/Info.plist`]: "<plist/>",
    [`${FW}/Lib.framework/Versions/1/Resources/resources.pak`]: "pak",
  },
  links: {
    [`${FW}/Lib.framework/Versions/Current`]: "1",
    [`${FW}/Lib.framework/Lib`]: "Versions/Current/Lib",
  },
};

async function embed(dir: string, args: string[] = ["--bundle", APP]) {
  const ctx = testContext({ cwd: dir, env: { BDK_HOME: join(dir, ".home") } });
  const code = await run(["build", "embed", ...args, "--json"], ctx);
  const report = JSON.parse(ctx.out.join(""));
  return {
    code,
    result: report.result?.embed,
    codes: (report.diagnostics ?? []).map((d: { code: string }) => d.code),
  };
}

const frameworks = (dir: string) => join(dir, APP, "Contents", "Frameworks");

describe("bdk build embed", () => {
  it("copies the runtime's frameworks into the bundle, symlinks as they are", async () => {
    const { dir } = await tempProject();
    const store = await storeRuntime(dir, RUNTIME);
    // The store is read-only; the copy must not be, or signing cannot rewrite it.
    await chmod(join(store, FW, "Lib.framework/Versions/1/Resources/resources.pak"), 0o444);
    const { code, result } = await embed(dir);
    expect(code).toBe(0);
    expect(result).toEqual({
      sourceKey: STORE_ENTRY,
      frameworks: ["Lib.framework"],
      unchanged: false,
    });
    const lib = join(frameworks(dir), "Lib.framework");
    expect(await readlink(join(lib, "Versions/Current"))).toBe("1");
    expect(await readlink(join(lib, "Lib"))).toBe("Versions/Current/Lib");
    expect(await readFile(join(lib, "Versions/1/Lib"), "utf8")).toBe("binary");
    expect((await stat(join(lib, "Versions/1/Resources/resources.pak"))).mode & 0o200).toBe(0o200);
  });

  it("leaves a copy that matches, signed or not, and the bundle's own frameworks", async () => {
    const { dir } = await tempProject();
    await storeRuntime(dir, RUNTIME);
    await mkdir(join(frameworks(dir), "Own.framework"), { recursive: true });
    await embed(dir);
    const lib = join(frameworks(dir), "Lib.framework", "Versions", "1");
    // What signing rewrites: the Info.plist and a _CodeSignature directory.
    await writeFile(join(lib, "Resources/Info.plist"), "<plist signed/>");
    await mkdir(join(lib, "_CodeSignature"));
    await writeFile(join(lib, "_CodeSignature", "CodeResources"), "");
    const before = (await lstat(join(lib, "Lib"))).mtimeMs;
    const { code, result } = await embed(dir);
    expect(code).toBe(0);
    expect(result.unchanged).toBe(true);
    expect((await lstat(join(lib, "Lib"))).mtimeMs).toBe(before);
    expect((await stat(join(frameworks(dir), "Own.framework"))).isDirectory()).toBe(true);
  });

  it("replaces a copy whose files are not the runtime's", async () => {
    const { dir } = await tempProject();
    await storeRuntime(dir, RUNTIME);
    await embed(dir);
    const pak = join(frameworks(dir), "Lib.framework/Versions/1/Resources/resources.pak");
    await writeFile(pak, "changed");
    await writeFile(join(frameworks(dir), "Lib.framework/Versions/1/stray"), "");
    const { code, result } = await embed(dir);
    expect(code).toBe(0);
    expect(result.unchanged).toBe(false);
    expect(await readFile(pak, "utf8")).toBe("pak");
    await expect(stat(join(frameworks(dir), "Lib.framework/Versions/1/stray"))).rejects.toThrow();
  });

  it("replaces a signed copy of another runtime whose engine alone differs", async () => {
    const { dir } = await tempProject();
    const engine = `${FW}/Lib.framework/Versions/1/Libraries/libengine.dylib`;
    await storeRuntime(dir, { files: { ...RUNTIME.files, [engine]: "old" }, links: RUNTIME.links });
    await embed(dir);
    // A new runtime whose only change is a library, which signing rewrites and the comparison ignores.
    await storeRuntime(dir, {
      files: { ...RUNTIME.files, [engine]: "new" },
      links: RUNTIME.links,
      key: "other",
    });
    const { code, result } = await embed(dir);
    expect(code).toBe(0);
    expect(result).toMatchObject({ sourceKey: "other", unchanged: false });
    const copied = join(frameworks(dir), "Lib.framework/Versions/1/Libraries/libengine.dylib");
    expect(await readFile(copied, "utf8")).toBe("new");
  });

  it("embed.no_runtime: needs the runtime step first", async () => {
    const { dir } = await tempProject();
    const { code, codes } = await embed(dir);
    expect(codes).toEqual(["embed.no_runtime"]);
    expect(code).toBe(1);
  });

  it("embed.runtime_missing: the resolved runtime is not in the store", async () => {
    const { dir } = await tempProject();
    await storeRuntime(dir, RUNTIME);
    await writeFile(
      join(dir, ".bdk", "runtime.json"),
      JSON.stringify({ source_key: "gone", manifest_sha256: "0".repeat(64) }),
    );
    const { code, codes } = await embed(dir);
    expect(codes).toEqual(["embed.runtime_missing"]);
    expect(code).toBe(1);
  });

  it("embed.runtime_changed: the store entry is not the runtime the project resolved", async () => {
    const { dir } = await tempProject();
    await storeRuntime(dir, RUNTIME);
    await writeFile(
      join(dir, ".bdk", "runtime.json"),
      JSON.stringify({ source_key: STORE_ENTRY, manifest_sha256: "0".repeat(64) }),
    );
    const { code, codes } = await embed(dir);
    expect(codes).toEqual(["embed.runtime_changed"]);
    expect(code).toBe(4);
  });

  it("embed.runtime_modified: the store entry does not match its manifest", async () => {
    const { dir } = await tempProject();
    const store = await storeRuntime(dir, RUNTIME);
    await writeFile(join(store, FW, "Lib.framework/Versions/1/Lib"), "tampered");
    const { code, codes } = await embed(dir);
    expect(codes).toEqual(["embed.runtime_modified"]);
    expect(code).toBe(4);
  });

  it("needs the bundle", async () => {
    const { dir } = await tempProject();
    const { code } = await embed(dir, []);
    expect(code).toBe(2);
  });
});
