import { lstat, mkdir, readdir, readFile, readlink, writeFile } from "node:fs/promises";
import { join } from "node:path";
import { describe, expect, it } from "vitest";
import { memoryAssets, tempDir, testContext } from "../../test-support.js";
import { run } from "../run.js";

const SDK = {
  "js/sdk/@bdk/automation/index.js": "export const session = 0;\n",
  "js/sdk/@bdk/automation/index.d.ts": "export declare const session: number;\n",
  "js/sdk/@bdk/automation/commands.js": "export const commands = 0;\n",
  "js/sdk/@bdk/automation/commands.d.ts": "export declare const commands: object;\n",
  "js/sdk/@bdk/automation/events.js": "export const events = 0;\n",
  "js/sdk/@bdk/automation/events.d.ts": "export declare const events: object;\n",
  "js/sdk/@bdk/internal/index.js": "export const index = 1;\n",
  "js/sdk/@bdk/internal/index.d.ts": "export declare const index: number;\n",
  "js/sdk/@bdk/internal/settings.js": "export const settings = 2;\n",
  "js/sdk/@bdk/internal/settings.d.ts": "export declare const settings: number;\n",
  "js/sdk/@bdk/protocol/index.d.ts": "export type Transport = unknown;\n",
  "js/sdk/@bdk/vite/index.js": "export const bdk = () => ({});\n",
  "js/sdk/@bdk/vite/index.d.ts": "export declare const bdk: () => object;\n",
};
const RESOLVED = JSON.stringify({
  browser: { internal_scheme: "demo" },
  routes: { settings: { mode: "bundled", path: "Pages/Settings" }, history: { mode: "chromium" } },
  aliases: { prefs: "settings" },
});
const COMMANDS = JSON.stringify({
  version: 1,
  commands: [{ name: "app.notes.list", result: "string[]" }],
  settings: [{ key: "app.notes.sort", type: "string", exposedToPages: true }],
});

async function tree(root: string): Promise<string[]> {
  try {
    return (await readdir(root, { recursive: true, withFileTypes: true }))
      .filter((e) => e.isFile())
      .map((e) => join(e.parentPath, e.name).slice(root.length + 1))
      .sort();
  } catch {
    return [];
  }
}

/** A project with the given files and a `bdk.resolved.json`. */
async function project(files: Record<string, string> = {}, toml = '[browser]\nname = "Demo"\n') {
  const dir = await tempDir();
  await writeFile(join(dir, "bdk.toml"), toml);
  await writeFile(join(dir, "bdk.resolved.json"), RESOLVED);
  for (const [name, text] of Object.entries(files)) {
    await mkdir(join(dir, name, ".."), { recursive: true });
    await writeFile(join(dir, name), text);
  }
  return dir;
}

const context = (dir: string, extra: Parameters<typeof testContext>[0] = {}) =>
  testContext({ cwd: dir, assets: memoryAssets(SDK), ...extra });

const PAGES = { "Pages/Settings/index.html": "<p>x</p>" };

describe("bdk build codegen", () => {
  it("writes the packages, the generated declarations and the editor link under .bdk/js", async () => {
    const dir = await project({ ...PAGES, ".bdk/bdk.commands.json": COMMANDS });
    const ctx = context(dir);
    expect(await run(["build", "codegen", "--json"], ctx)).toBe(0);
    const out = join(dir, ".bdk", "js");
    // `node_modules/@bdk` is a link to `@bdk`; the listing follows it.
    expect((await tree(out)).filter((f) => !f.startsWith("node_modules/@bdk/"))).toEqual([
      "@bdk/automation/app.d.ts",
      "@bdk/automation/commands.d.ts",
      "@bdk/automation/commands.js",
      "@bdk/automation/events.d.ts",
      "@bdk/automation/events.js",
      "@bdk/automation/index.d.ts",
      "@bdk/automation/index.js",
      "@bdk/automation/package.json",
      "@bdk/commands/index.d.ts",
      "@bdk/commands/index.js",
      "@bdk/commands/package.json",
      "@bdk/env.d.ts",
      "@bdk/events/index.d.ts",
      "@bdk/events/index.js",
      "@bdk/events/package.json",
      "@bdk/internal/app.d.ts",
      "@bdk/internal/index.d.ts",
      "@bdk/internal/index.js",
      "@bdk/internal/package.json",
      "@bdk/internal/settings.d.ts",
      "@bdk/internal/settings.js",
      "@bdk/protocol/index.d.ts",
      "@bdk/protocol/package.json",
      "@bdk/vite/index.d.ts",
      "@bdk/vite/index.js",
      "@bdk/vite/package.json",
      "node_modules/.gitignore",
      "sdk.digest",
    ]);
    const app = await readFile(join(out, "@bdk/internal/app.d.ts"), "utf8");
    expect(app).toContain('"app.notes.list"');
    expect(app).toContain('"app.notes.sort": string;');
    const env = await readFile(join(out, "@bdk/env.d.ts"), "utf8");
    expect(env).toContain('"history" | "prefs" | "settings"');
    expect(env).toContain('InternalScheme = "demo"');
    const internal = JSON.parse(await readFile(join(out, "@bdk/internal/package.json"), "utf8"));
    expect(internal).toMatchObject({ name: "@bdk/internal", type: "module", version: "1.2.3" });
    expect(internal.exports).toEqual({
      ".": { types: "./index.d.ts", default: "./index.js" },
      "./app": { types: "./app.d.ts" },
      "./settings": { types: "./settings.d.ts", default: "./settings.js" },
    });
    const protocol = JSON.parse(await readFile(join(out, "@bdk/protocol/package.json"), "utf8"));
    expect(protocol.exports).toEqual({});
    // The packages resolve each other (`@bdk/protocol` from `@bdk/internal`) once links are followed.
    expect(await readlink(join(out, "node_modules", "@bdk"))).toBe(join(out, "@bdk"));
    const link = join(dir, "Pages", "node_modules", "@bdk");
    expect(await readlink(link)).toBe(join(out, "@bdk"));
    expect(await readFile(join(dir, "Pages", "node_modules", ".gitignore"), "utf8")).toBe("*\n");
    const document = JSON.parse(ctx.out.join(""));
    expect(document.result.codegen).toMatchObject({
      generated: true,
      out,
      declarations: join(dir, ".bdk", "bdk.commands.json"),
      links: [link],
    });
  });

  it("writes nothing when the recorded digests equal the inputs, and regenerates when one changes", async () => {
    const dir = await project({ ...PAGES, "app.commands.json": COMMANDS });
    const args = ["build", "codegen", "--commands", "app.commands.json", "--out", "gen", "--json"];
    expect(await run(args, context(dir))).toBe(0);
    const marker = join(dir, "gen", "@bdk", "internal", "index.js");
    const before = (await lstat(marker)).mtimeMs;
    await new Promise((resolve) => setTimeout(resolve, 20));

    const again = context(dir);
    expect(await run(args, again)).toBe(0);
    expect(JSON.parse(again.out.join("")).result.codegen.generated).toBe(false);
    expect((await lstat(marker)).mtimeMs).toBe(before);

    await writeFile(
      join(dir, "app.commands.json"),
      JSON.stringify({
        version: 1,
        commands: [{ name: "app.notes.add", args: { text: "string" } }],
      }),
    );
    expect(await run(args, context(dir))).toBe(0);
    expect(await readFile(join(dir, "gen/@bdk/internal/app.d.ts"), "utf8")).toContain(
      "app.notes.add",
    );

    await writeFile(join(dir, "bdk.resolved.json"), RESOLVED.replace("demo", "other"));
    expect(await run(args, context(dir))).toBe(0);
    expect(await readFile(join(dir, "gen/@bdk/env.d.ts"), "utf8")).toContain('"other"');

    const newSdk = { ...SDK, "js/sdk/@bdk/internal/index.js": "export const index = 9;\n" };
    expect(await run(args, context(dir, { assets: memoryAssets(newSdk) }))).toBe(0);
    expect(await readFile(join(dir, "gen/@bdk/internal/index.js"), "utf8")).toContain("9");
  });

  it("removes files an earlier run wrote that the SDK no longer has", async () => {
    const dir = await project(PAGES);
    expect(await run(["build", "codegen"], context(dir))).toBe(0);
    const gone = { ...SDK } as Record<string, string>;
    delete gone["js/sdk/@bdk/internal/settings.js"];
    delete gone["js/sdk/@bdk/internal/settings.d.ts"];
    expect(await run(["build", "codegen"], context(dir, { assets: memoryAssets(gone) }))).toBe(0);
    expect(await tree(join(dir, ".bdk/js/@bdk/internal"))).not.toContain("settings.js");
  });

  it("generates with no app. declarations and says so in a note when the file is missing", async () => {
    const dir = await project(PAGES);
    const ctx = context(dir);
    expect(await run(["build", "codegen", "--json"], ctx)).toBe(0);
    const document = JSON.parse(ctx.out.join(""));
    expect(document.diagnostics).toEqual([
      expect.objectContaining({ code: "codegen.no_declarations", severity: "note" }),
    ]);
    expect(document.result.codegen.declarations).toBeNull();
    const app = await readFile(join(dir, ".bdk/js/@bdk/internal/app.d.ts"), "utf8");
    expect(app).not.toContain("AppCommands");
    expect(ctx.err.join("")).toContain("note:");
    expect(ctx.err.join("")).toContain("[codegen.no_declarations]");
  });

  it("generates nothing with no Pages and no Automation directory", async () => {
    const dir = await project();
    const ctx = context(dir);
    expect(await run(["build", "codegen", "--json"], ctx)).toBe(0);
    expect(JSON.parse(ctx.out.join("")).result.codegen.generated).toBe(false);
    expect(await tree(join(dir, ".bdk"))).toEqual([]);
  });

  it("generates for an Automation directory alone, and links both directories that exist", async () => {
    const dir = await project(
      { "Automation/a.ts": "1", "Scripts/x.ts": "1" },
      '[browser]\nname = "Demo"\n[pages]\ndirectory = "Views"\n[automation]\ndirectory = "Automation"\n',
    );
    expect(await run(["build", "codegen"], context(dir))).toBe(0);
    expect(await readlink(join(dir, "Automation/node_modules/@bdk"))).toBe(
      join(dir, ".bdk/js/@bdk"),
    );
    expect(await tree(join(dir, "Views"))).toEqual([]);
  });

  it("gives scripts the typed packages: @bdk/automation with its app. declarations, and @bdk/commands and @bdk/events standing for its entries", async () => {
    const dir = await project({ "Automation/a.ts": "1", ".bdk/bdk.commands.json": COMMANDS });
    const ctx = context(dir, {});
    expect(await run(["build", "codegen", "--json"], ctx)).toBe(0);
    const out = join(dir, ".bdk", "js");
    const app = await readFile(join(out, "@bdk/automation/app.d.ts"), "utf8");
    expect(app).toContain('declare module "@bdk/automation/commands"');
    expect(app).toContain('"app.notes.list"');
    expect(app).not.toContain("@bdk/internal");
    const automation = JSON.parse(
      await readFile(join(out, "@bdk/automation/package.json"), "utf8"),
    );
    expect(automation.exports).toEqual({
      ".": { types: "./index.d.ts", default: "./index.js" },
      "./app": { types: "./app.d.ts" },
      "./commands": { types: "./commands.d.ts", default: "./commands.js" },
      "./events": { types: "./events.d.ts", default: "./events.js" },
    });
    expect(await readFile(join(out, "@bdk/commands/index.d.ts"), "utf8")).toBe(
      'export * from "@bdk/automation/commands";\n',
    );
    expect(await readFile(join(out, "@bdk/events/index.js"), "utf8")).toBe(
      'export * from "@bdk/automation/events";\n',
    );
    const events = JSON.parse(await readFile(join(out, "@bdk/events/package.json"), "utf8"));
    expect(events).toMatchObject({ name: "@bdk/events", type: "module", version: "1.2.3" });
    // The link in the Automation directory reaches them all, `@bdk/commands` included.
    const link = join(dir, "Automation", "node_modules", "@bdk");
    expect(await readlink(link)).toBe(join(out, "@bdk"));
    expect((await readdir(link)).sort()).toEqual([
      "automation",
      "commands",
      "env.d.ts",
      "events",
      "internal",
      "protocol",
      "vite",
    ]);
    expect(JSON.parse(ctx.out.join("")).result.codegen.packages).toEqual([
      "@bdk/automation",
      "@bdk/internal",
      "@bdk/protocol",
      "@bdk/vite",
      "@bdk/commands",
      "@bdk/events",
    ]);
  });

  it("writes no @bdk/commands or @bdk/events when the SDK has no @bdk/automation", async () => {
    const dir = await project(PAGES);
    const sdk = Object.fromEntries(
      Object.entries(SDK).filter(([name]) => !name.includes("@bdk/automation")),
    );
    expect(await run(["build", "codegen"], context(dir, { assets: memoryAssets(sdk) }))).toBe(0);
    expect(await tree(join(dir, ".bdk/js/@bdk"))).not.toContain("commands/index.js");
  });

  it("uses the directory named in [pages] of bdk.toml for the link", async () => {
    const dir = await project(
      { "Views/Settings/index.html": "x" },
      '[browser]\nname = "Demo"\n[pages]\ndirectory = "Views"\n',
    );
    expect(await run(["build", "codegen"], context(dir))).toBe(0);
    expect(await lstat(join(dir, "Views/node_modules/@bdk"))).toBeTruthy();
  });

  it("refreshes a link that points elsewhere and never replaces a real directory", async () => {
    const dir = await project(PAGES);
    await mkdir(join(dir, "Pages/node_modules"), { recursive: true });
    await import("node:fs/promises").then((fs) =>
      fs.symlink(join(dir, "stale"), join(dir, "Pages/node_modules/@bdk")),
    );
    expect(await run(["build", "codegen"], context(dir))).toBe(0);
    expect(await readlink(join(dir, "Pages/node_modules/@bdk"))).toBe(join(dir, ".bdk/js/@bdk"));

    const other = await project(PAGES);
    await mkdir(join(other, "Pages/node_modules/@bdk"), { recursive: true });
    await writeFile(join(other, "Pages/node_modules/@bdk/mine.txt"), "keep");
    const ctx = context(other);
    expect(await run(["build", "codegen"], ctx)).toBe(0);
    expect(ctx.err.join("")).toContain("[codegen.link_blocked]");
    expect(await readFile(join(other, "Pages/node_modules/@bdk/mine.txt"), "utf8")).toBe("keep");
    expect((await lstat(join(other, "Pages/node_modules/@bdk"))).isSymbolicLink()).toBe(false);
  });

  it("reports invalid declarations at the file, per issue, and writes nothing", async () => {
    const dir = await project(PAGES);
    const file = join(dir, "bad.json");
    await writeFile(
      file,
      JSON.stringify({
        version: 1,
        commands: [{ name: "notes.list" }, { name: "app.x", args: { a: "Missing" } }],
      }),
    );
    const ctx = context(dir);
    expect(await run(["build", "codegen", "--commands", "bad.json"], ctx)).toBe(1);
    const err = ctx.err.join("");
    expect(err).toContain(file);
    expect(err).toContain("[codegen.declarations_invalid]");
    expect(err).toContain("$.commands[0].name");
    expect(err).toContain("unknown type");
    expect(await tree(join(dir, ".bdk"))).toEqual([]);
  });

  it("reports an invalid resolved file with its own code, and a missing one", async () => {
    const dir = await project(PAGES);
    await writeFile(join(dir, "bdk.resolved.json"), '{"routes":{}}');
    const bad = context(dir);
    expect(await run(["build", "codegen"], bad)).toBe(1);
    expect(bad.err.join("")).toContain("[codegen.resolved_invalid]");
    expect(bad.err.join("")).toContain("internal_scheme");

    const missing = context(dir);
    expect(await run(["build", "codegen", "--resolved", "nope.json"], missing)).toBe(1);
    expect(missing.err.join("")).toContain("[codegen.resolved_missing]");
    expect(missing.err.join("")).toContain("bdk build config");
  });

  it("exits 3 with cli.asset_missing when the executable carries no packages", async () => {
    const dir = await project(PAGES);
    const ctx = context(dir, { assets: memoryAssets({}) });
    expect(await run(["build", "codegen"], ctx)).toBe(3);
    expect(ctx.err.join("")).toContain("[cli.asset_missing]");
  });

  it("rejects --commands on another step", async () => {
    const ctx = testContext();
    expect(await run(["build", "config", "--commands", "x.json"], ctx)).toBe(2);
    expect(ctx.err.join("")).toContain("--commands applies to `bdk build codegen` only");
  });
});
