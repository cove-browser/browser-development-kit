import { mkdir, readdir, readFile, readlink, writeFile } from "node:fs/promises";
import { join } from "node:path";
import { describe, expect, it } from "vitest";
import { run } from "../../core/run.js";
import { memoryAssets, tempDir, testContext } from "../../test-support.js";
import { createXcodePlatform } from "./index.js";

const SDK = {
  "bdk.schema.json": "{}",
  "js/internal/index.js": "export const index = 1;\n",
  "js/sdk/@bdk/internal/index.js": "export const index = 1;\n",
  "js/sdk/@bdk/internal/index.d.ts": "export declare const index: number;\n",
};
const COMMANDS = JSON.stringify({ version: 1, commands: [{ name: "app.notes.list" }] });
const PAGE = "<!doctype html><title>x</title>";
const RESOLVED = {
  browser: { internal_scheme: "demo" },
  routes: {
    settings: { mode: "bundled", path: "Pages/Settings" },
    "new-tab": { mode: "bundled", path: "Pages/NewTab" },
    history: { mode: "chromium" },
  },
};

/** A project with two pages, a shared directory, a `node_modules`, and an application bundle path. */
async function project() {
  const dir = await tempDir();
  const files: Record<string, string> = {
    "bdk.toml": '[browser]\nname = "Demo"\ninternal_scheme = "demo"\n',
    "bdk.resolved.json": JSON.stringify(RESOLVED),
    "Pages/Settings/index.html": PAGE,
    "Pages/Settings/main.js": "1",
    "Pages/NewTab/index.html": PAGE,
    "Pages/shared/util.js": "1",
    "Pages/node_modules/left-pad/index.js": "1",
    "Pages/.DS_Store": "x",
    "App/Info.plist": "x",
    "App/App.entitlements": "x",
  };
  for (const [name, text] of Object.entries(files)) {
    await mkdir(join(dir, name, ".."), { recursive: true });
    await writeFile(join(dir, name), text);
  }
  return { dir, derived: await tempDir(), app: join(await tempDir(), "Demo.app") };
}

const fakePlutil = () => Promise.resolve("{}");

function xcodeContext(p: Awaited<ReturnType<typeof project>>, env: Record<string, string> = {}) {
  return testContext({
    cwd: p.dir,
    assets: memoryAssets(SDK),
    platform: createXcodePlatform(fakePlutil),
    env: {
      XCODE_VERSION_ACTUAL: "2600",
      SRCROOT: p.dir,
      DERIVED_FILE_DIR: p.derived,
      CODESIGNING_FOLDER_PATH: p.app,
      ...env,
    },
  });
}

describe("xcode platform: derived-file defaults for bdk build codegen", () => {
  it("defaults the output to $DERIVED_FILE_DIR/bdk/js and the declarations to $DERIVED_FILE_DIR/bdk.commands.json", async () => {
    const p = await project();
    await writeFile(join(p.derived, "bdk.commands.json"), COMMANDS);
    const ctx = xcodeContext(p);
    expect(await run(["build", "codegen"], ctx)).toBe(0);
    expect(ctx.err.join("")).toBe("");
    expect(await readFile(join(p.derived, "bdk/js/@bdk/internal/app.d.ts"), "utf8")).toContain(
      '"app.notes.list"',
    );
    expect(await readlink(join(p.dir, "Pages/node_modules/@bdk"))).toBe(
      join(p.derived, "bdk/js/@bdk"),
    );
  });

  it("takes the declarations from the BDK_COMMANDS_FILE build setting, relative to SRCROOT", async () => {
    const p = await project();
    await writeFile(join(p.dir, "App/decl.json"), COMMANDS);
    const ctx = xcodeContext(p, { BDK_COMMANDS_FILE: "App/decl.json" });
    expect(await run(["build", "codegen"], ctx)).toBe(0);
    expect(await readFile(join(p.derived, "bdk/js/@bdk/internal/app.d.ts"), "utf8")).toContain(
      "app.notes.list",
    );
  });

  it("lets --out and --commands override the defaults", async () => {
    const p = await project();
    await writeFile(join(p.dir, "c.json"), COMMANDS);
    const ctx = xcodeContext(p);
    expect(await run(["build", "codegen", "--out", "o", "--commands", "c.json"], ctx)).toBe(0);
    expect(await readdir(join(p.dir, "o"))).toContain("@bdk");
    expect(await readdir(p.derived)).toEqual([]);
  });

  it("formats diagnostics for Xcode, a note for the missing declarations included", async () => {
    const p = await project();
    const ctx = xcodeContext(p);
    expect(await run(["build", "codegen"], ctx)).toBe(0);
    expect(ctx.err.join("")).toContain(
      `${join(p.derived, "bdk.commands.json")}: note: [codegen.no_declarations]`,
    );
  });
});

describe("xcode platform: input and output file lists written by bdk build config", () => {
  const lists = (p: Awaited<ReturnType<typeof project>>) => join(p.derived, "bdk");
  const read = async (p: Awaited<ReturnType<typeof project>>, name: string) =>
    (await readFile(join(lists(p), name), "utf8")).split("\n").filter(Boolean);

  it("lists bdk.toml, the property lists and the declarations for the runtime phase, and what it writes", async () => {
    const p = await project();
    await writeFile(join(p.derived, "bdk.commands.json"), COMMANDS);
    const ctx = xcodeContext(p, {
      INFOPLIST_FILE: "App/Info.plist",
      CODE_SIGN_ENTITLEMENTS: "App/App.entitlements",
    });
    // The fake resolver stands in for the real one; the file it "resolves" is the fixture's.
    ctx.resolveConfig = () => Promise.resolve({ diagnostics: [], resolved: RESOLVED });
    expect(await run(["build", "config", "--json"], ctx)).toBe(0);
    expect(await read(p, "runtime.input.xcfilelist")).toEqual([
      join(p.dir, "bdk.toml"),
      join(p.dir, "App/Info.plist"),
      join(p.dir, "App/App.entitlements"),
      join(p.derived, "bdk.commands.json"),
    ]);
    expect(await read(p, "runtime.output.xcfilelist")).toEqual([
      join(p.dir, "bdk.resolved.json"),
      join(p.derived, "bdk/js/sdk.digest"),
      join(p.derived, "bdk/js/@bdk/env.d.ts"),
    ]);
    const document = JSON.parse(ctx.out.join(""));
    expect(document.result.config.fileLists).toHaveLength(4);
  });

  it("lists every page file but node_modules and dot-files for the bundle phase, and the bundle destinations", async () => {
    const p = await project();
    const ctx = xcodeContext(p);
    ctx.resolveConfig = () => Promise.resolve({ diagnostics: [], resolved: RESOLVED });
    expect(await run(["build", "config"], ctx)).toBe(0);
    expect(await read(p, "bundle.input.xcfilelist")).toEqual([
      join(p.dir, "bdk.toml"),
      join(p.dir, "bdk.resolved.json"),
      join(p.dir, "Pages/NewTab/index.html"),
      join(p.dir, "Pages/Settings/index.html"),
      join(p.dir, "Pages/Settings/main.js"),
      join(p.dir, "Pages/shared/util.js"),
    ]);
    const resources = join(p.app, "Contents", "Resources");
    expect(await read(p, "bundle.output.xcfilelist")).toEqual([
      join(resources, "Pages/Settings"),
      join(resources, "Pages/NewTab"),
      join(resources, "BDK/js/internal"),
    ]);
  });

  it("omits the bundle lists when Xcode names no bundle, and writes nothing outside Xcode", async () => {
    const p = await project();
    const env = xcodeContext(p).env;
    const ctx = xcodeContext(p);
    delete env.CODESIGNING_FOLDER_PATH;
    ctx.env = env;
    ctx.resolveConfig = () => Promise.resolve({ diagnostics: [], resolved: RESOLVED });
    expect(await run(["build", "config"], ctx)).toBe(0);
    expect((await readdir(lists(p))).sort()).toEqual([
      "runtime.input.xcfilelist",
      "runtime.output.xcfilelist",
    ]);

    const plain = await project();
    const generic = testContext({ cwd: plain.dir, env: { DERIVED_FILE_DIR: plain.derived } });
    expect(await run(["build", "config"], generic)).toBe(0);
    expect(await readdir(plain.derived)).toEqual([]);
  });
});
