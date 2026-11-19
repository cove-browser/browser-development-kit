import { spawnSync } from "node:child_process";
import { mkdir, readdir, readFile, writeFile } from "node:fs/promises";
import { dirname, join, relative, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { describe, expect, it } from "vitest";
import { tempDir } from "./test-support.js";

const srcDir = dirname(fileURLToPath(import.meta.url));
const repoRoot = resolve(srcDir, "..", "..", "..");
const biome = join(repoRoot, "node_modules", ".bin", "biome");

/** Runs Biome with the repository's real configuration on one file placed at `path` in a scratch tree. */
async function lintFixture(
  path: string,
  source: string,
): Promise<{ status: number; output: string }> {
  const root = await tempDir("bdk-biome-");
  const config = JSON.parse(await readFile(join(repoRoot, "biome.json"), "utf8"));
  // The scratch tree is not a git repository.
  config.vcs = { enabled: false };
  delete config.$schema;
  await writeFile(join(root, "biome.json"), JSON.stringify(config));
  const file = join(root, path);
  await mkdir(dirname(file), { recursive: true });
  await writeFile(file, source);
  const result = spawnSync(biome, ["lint", "--colors=off", path], { cwd: root, encoding: "utf8" });
  return { status: result.status ?? -1, output: `${result.stdout}${result.stderr}` };
}

const use = (specifier: string): string =>
  `import { thing } from "${specifier}";\nexport const x = thing;\n`;

describe("the core never imports a platform module (lint rule)", () => {
  it.each([
    "../platform/xcode/index.js",
    "../../platform/xcode/index.js",
    "../platform/generic/index.js",
    "../platform/select.js",
  ])("rejects %s from src/core", async (specifier) => {
    const result = await lintFixture("packages/cli/src/core/a.ts", use(specifier));
    expect(result.status).not.toBe(0);
    expect(result.output).toContain("noRestrictedImports");
  });

  it("rejects it from a nested core directory", async () => {
    const result = await lintFixture(
      "packages/cli/src/core/steps/a.ts",
      use("../../platform/xcode/index.js"),
    );
    expect(result.output).toContain("noRestrictedImports");
  });

  it("allows the platform interface file", async () => {
    const result = await lintFixture("packages/cli/src/core/a.ts", use("../platform/platform.js"));
    expect(result.output).not.toContain("noRestrictedImports");
  });

  it("does not restrict code outside src/core", async () => {
    const result = await lintFixture("packages/cli/src/main.ts", use("./platform/select.js"));
    expect(result.output).not.toContain("noRestrictedImports");
  });
});

async function sourceFiles(dir: string): Promise<string[]> {
  const entries = await readdir(dir, { recursive: true, withFileTypes: true });
  return entries
    .filter((e) => e.isFile() && e.name.endsWith(".ts") && !e.name.endsWith(".test.ts"))
    .map((e) => join(e.parentPath, e.name));
}

describe("single points of choice", () => {
  it("only platform/select.ts imports the platform modules, besides themselves", async () => {
    const importers: string[] = [];
    for (const file of await sourceFiles(srcDir)) {
      const rel = relative(srcDir, file);
      // test-support.ts is test scaffolding, not part of the executable.
      if (rel === "test-support.ts") continue;
      const text = await readFile(file, "utf8");
      if (/from "[^"]*\b(xcode|generic)\/index\.js"/.test(text)) importers.push(rel);
    }
    expect(importers).toEqual(["platform/select.ts"]);
  });

  it("only assets.ts imports node:sea", async () => {
    const importers: string[] = [];
    for (const file of await sourceFiles(srcDir)) {
      if ((await readFile(file, "utf8")).includes('"node:sea"'))
        importers.push(relative(srcDir, file));
    }
    expect(importers).toEqual(["assets.ts"]);
  });
});
