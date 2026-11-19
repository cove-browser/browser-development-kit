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

describe("createAssets, packaged", () => {
  it("reports a name that the executable does not embed as not found", () => {
    // Outside a single executable, `node:sea` has no assets, so every name is missing.
    expect(() => createAssets({ packaged: true }).readText("bdk.schema.json")).toThrow(
      AssetNotFoundError,
    );
  });
});
