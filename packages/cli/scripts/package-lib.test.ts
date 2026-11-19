import { mkdir, writeFile } from "node:fs/promises";
import { join } from "node:path";
import { describe, expect, it } from "vitest";
import { tempDir } from "../src/test-support.js";
import { artifactBundleInfo, listAssets, seaConfig } from "./package-lib.js";

describe("listAssets", () => {
  it("lists every file at any depth by relative name, except the README", async () => {
    const dir = await tempDir();
    await mkdir(join(dir, "templates", "react"), { recursive: true });
    await writeFile(join(dir, "README.md"), "docs");
    await writeFile(join(dir, "bdk.schema.json"), "{}");
    await writeFile(join(dir, "templates", "react", "App.tsx"), "x");
    expect(listAssets(dir)).toEqual({
      "bdk.schema.json": join(dir, "bdk.schema.json"),
      "templates/react/App.tsx": join(dir, "templates", "react", "App.tsx"),
    });
  });

  it("lists nothing for a directory with only the README", async () => {
    const dir = await tempDir();
    await writeFile(join(dir, "README.md"), "docs");
    expect(listAssets(dir)).toEqual({});
  });
});

describe("seaConfig", () => {
  it("builds a CommonJS executable with the assets and code cache as asked", () => {
    expect(
      seaConfig({ main: "/m.cjs", output: "/o/bdk", assets: { a: "/a" }, useCodeCache: true }),
    ).toEqual({
      main: "/m.cjs",
      output: "/o/bdk",
      mainFormat: "commonjs",
      disableExperimentalSEAWarning: true,
      useCodeCache: true,
      assets: { a: "/a" },
    });
  });
});

describe("artifactBundleInfo", () => {
  it("describes one executable artifact with one variant", () => {
    expect(
      artifactBundleInfo({
        version: "1.0.0",
        variantDir: "bdk-1.0.0-macosx/bin",
        triple: "arm64-apple-macosx",
      }),
    ).toEqual({
      schemaVersion: "1.0",
      artifacts: {
        bdk: {
          type: "executable",
          version: "1.0.0",
          variants: [
            { path: "bdk-1.0.0-macosx/bin/bdk", supportedTriples: ["arm64-apple-macosx"] },
          ],
        },
      },
    });
  });
});
