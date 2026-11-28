import { mkdir, writeFile } from "node:fs/promises";
import { join } from "node:path";
import { describe, expect, it } from "vitest";
import { AssetNotFoundError, createAssets, unpackagedAssetsDir } from "./assets.js";
import { tempDir } from "./test-support.js";

describe("createAssets, unpackaged", () => {
  it("reads named files, including nested ones, from the assets directory", async () => {
    const dir = await tempDir();
    await mkdir(join(dir, "templates"));
    await writeFile(join(dir, "a.json"), '{"a":1}');
    await writeFile(join(dir, "templates", "b.txt"), "hello");
    const assets = createAssets({ packaged: false, assetsDir: dir });
    expect(assets.readText("a.json")).toBe('{"a":1}');
    expect(assets.readText("templates/b.txt")).toBe("hello");
  });

  it("throws AssetNotFoundError for a missing name", async () => {
    const assets = createAssets({ packaged: false, assetsDir: await tempDir() });
    expect(() => assets.readText("nope.json")).toThrow(AssetNotFoundError);
  });

  it("does not read outside the assets directory", async () => {
    const dir = await tempDir();
    const assets = createAssets({ packaged: false, assetsDir: join(dir, "inner") });
    await mkdir(join(dir, "inner"));
    await writeFile(join(dir, "secret.txt"), "x");
    expect(() => assets.readText("../secret.txt")).toThrow(AssetNotFoundError);
  });

  it("defaults to packages/cli/assets when not running as an executable", () => {
    expect(unpackagedAssetsDir().endsWith(join("packages", "cli", "assets"))).toBe(true);
    expect(createAssets().readText).toBeTypeOf("function");
  });
});

describe("createAssets, listing and the SDK modules", () => {
  it("lists asset names by prefix, with the built @bdk/internal modules under js/internal/", async () => {
    const assets = await tempDir();
    const internal = await tempDir();
    await writeFile(join(assets, "a.json"), "{}");
    await writeFile(join(assets, "README.md"), "docs");
    await writeFile(join(internal, "settings.js"), "export const s = 1;");
    await writeFile(join(internal, "settings.d.ts"), "export {};");
    await writeFile(join(internal, "settings.test.js"), "test");
    await writeFile(join(internal, "index.js"), "export const i = 1;");
    const list = createAssets({
      packaged: false,
      assetsDir: assets,
      internalDir: internal,
      sdkDirs: {},
      repositoryRoot: await tempDir(),
    });
    expect(list.list("js/internal/")).toEqual(["js/internal/index.js", "js/internal/settings.js"]);
    expect(list.list("")).toEqual([
      "a.json",
      "bdk-release.json",
      "js/internal/index.js",
      "js/internal/settings.js",
    ]);
    expect(list.readText("js/internal/settings.js")).toBe("export const s = 1;");
  });

  it("carries BDK's patch series, the templates and the release facts from the repository", async () => {
    const root = await tempDir();
    await mkdir(join(root, "chromium", "patches"), { recursive: true });
    await writeFile(join(root, "chromium", "patches", "series"), "a.patch\n");
    await writeFile(join(root, "chromium", "patches", "README.md"), "docs");
    await mkdir(join(root, "apps", "templates", "pages", "node_modules", "x"), { recursive: true });
    await writeFile(join(root, "apps", "templates", "pages", "versions.json"), "{}");
    await writeFile(join(root, "apps", "templates", "pages", "project.json"), "{}");
    await writeFile(join(root, "apps", "templates", "pages", "node_modules", "x", "i.js"), "");
    const assets = createAssets({ packaged: false, sdkDirs: {}, repositoryRoot: root });
    expect(assets.list("chromium/")).toEqual(["chromium/patches/series"]);
    expect(assets.list("templates/")).toEqual(["templates/pages/versions.json"]);
    expect(assets.readText("chromium/patches/series")).toBe("a.patch\n");
    expect(() => assets.readText("chromium/patches/README.md")).toThrow(AssetNotFoundError);
    // The release facts come from the repository this bdk was built from.
    const release = JSON.parse(createAssets({ packaged: false }).readText("bdk-release.json"));
    expect(release.chromium.commit).toMatch(/^[0-9a-f]{40}$/);
    expect(release.abi.hash).toMatch(/^[0-9a-f]{64}$/);
  });

  it("lists nothing when the SDK has not been built", async () => {
    const assets = createAssets({
      packaged: false,
      assetsDir: await tempDir(),
      internalDir: join(await tempDir(), "missing"),
    });
    expect(assets.list("js/internal/")).toEqual([]);
    expect(() => assets.readText("js/internal/index.js")).toThrow(AssetNotFoundError);
  });

  it("lists the SDK packages under js/sdk/@bdk/<package>/ and reads them, tests and other files left out", async () => {
    const internal = await tempDir();
    const protocol = await tempDir();
    await writeFile(join(internal, "page.js"), "export const page = 1;");
    await writeFile(join(internal, "page.d.ts"), "export declare const page: number;");
    await writeFile(join(internal, "page.test.js"), "test");
    await writeFile(join(internal, "page.js.map"), "{}");
    await writeFile(join(protocol, "index.d.ts"), "export {};");
    const assets = createAssets({
      packaged: false,
      assetsDir: await tempDir(),
      internalDir: internal,
      sdkDirs: { "@bdk/internal": internal, "@bdk/protocol": protocol },
    });
    expect(assets.list("js/sdk/")).toEqual([
      "js/sdk/@bdk/internal/page.d.ts",
      "js/sdk/@bdk/internal/page.js",
      "js/sdk/@bdk/protocol/index.d.ts",
    ]);
    expect(assets.readText("js/sdk/@bdk/internal/page.d.ts")).toContain("declare const page");
    expect(() => assets.readText("js/sdk/@bdk/internal/page.test.js")).toThrow(AssetNotFoundError);
  });

  it("does not read outside the SDK directory", async () => {
    const dir = await tempDir();
    await mkdir(join(dir, "dist"));
    await writeFile(join(dir, "secret.js"), "x");
    const assets = createAssets({
      packaged: false,
      assetsDir: dir,
      internalDir: join(dir, "dist"),
    });
    expect(() => assets.readText("js/internal/../secret.js")).toThrow(AssetNotFoundError);
  });
});

describe("createAssets, packaged", () => {
  it("lists nothing when the executable embeds no index", () => {
    expect(createAssets({ packaged: true }).list("js/internal/")).toEqual([]);
  });

  it("reports a name that the executable does not embed as not found", () => {
    // Outside a single executable, `node:sea` has no assets, so every name is missing.
    expect(() => createAssets({ packaged: true }).readText("bdk.schema.json")).toThrow(
      AssetNotFoundError,
    );
  });
});
