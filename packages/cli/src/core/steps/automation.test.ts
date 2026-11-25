import { existsSync } from "node:fs";
import { mkdir, readFile, rm, writeFile } from "node:fs/promises";
import { join } from "node:path";
import { describe, expect, it, vi } from "vitest";
import { createAssets } from "../../assets.js";
import { fakeWatch, tempDir, testContext } from "../../test-support.js";
import type { Bundler } from "../automation/bundler.js";
import { sha256 } from "../config/digest.js";
import { resolveConfig } from "../config/index.js";
import { configDigest } from "../config/resolve.js";
import { run } from "../run.js";

async function write(root: string, file: string, text: string): Promise<void> {
  await mkdir(join(root, file, ".."), { recursive: true });
  await writeFile(join(root, file), text);
}

/** A project with `Automation/hello.ts` and its own copy of `@bdk/automation`. */
async function project(
  script: string,
  options: { enabled?: boolean } = {},
): Promise<{ dir: string; app: string }> {
  const dir = await tempDir();
  await write(
    dir,
    "bdk.toml",
    `[browser]\nname = "Demo"\n\n[automation]\nenabled = ${options.enabled ?? true}\n`,
  );
  await write(dir, "Automation/hello.ts", script);
  await write(dir, "node_modules/@bdk/automation/package.json", '{"name":"@bdk/automation"}\n');
  await write(
    dir,
    "node_modules/@bdk/automation/src/index.ts",
    'import { call } from "./client.js";\nexport const contents = { list: () => call("contents.list") };\n',
  );
  await write(
    dir,
    "node_modules/@bdk/automation/src/client.ts",
    "export function call(command: string): string { return `call ${command}`; }\n",
  );
  await write(
    dir,
    "node_modules/@bdk/automation/src/commands.ts",
    'export const commands = { id: "commands" };\n',
  );
  await write(
    dir,
    "node_modules/@bdk/automation/src/events.ts",
    'export const events = { id: "events" };\n',
  );
  return { dir, app: join(dir, "Build", "Demo.app") };
}

const real = (dir: string, overrides = {}) =>
  testContext({
    cwd: dir,
    assets: createAssets({ packaged: false }),
    resolveConfig,
    ...overrides,
  });

async function build(dir: string) {
  const ctx = real(dir);
  expect(await run(["build", "config"], ctx)).toBe(0);
  return { ctx };
}

async function resolvedOf(dir: string): Promise<Record<string, unknown>> {
  return JSON.parse(await readFile(join(dir, "bdk.resolved.json"), "utf8"));
}

const HELLO = `import { contents } from "@bdk/automation";
import { commands } from "@bdk/commands";
import { events } from "@bdk/events";
export default async function main() {
  return [contents.list(), commands.id, events.id];
}
`;

describe("bdk build automation", () => {
  it("does nothing, and says so, when there are no scripts", async () => {
    const { dir, app } = await project(HELLO, { enabled: false });
    await build(dir);
    const ctx = real(dir);
    expect(await run(["build", "automation", "--bundle", app], ctx)).toBe(0);
    expect(ctx.out.join("")).toContain("nothing to do");
    expect(existsSync(app)).toBe(false);
  });

  it("bundles a script into one module with no imports and records its digest", async () => {
    const { dir, app } = await project(HELLO);
    await build(dir);
    const before = await resolvedOf(dir);
    const ctx = real(dir);
    expect(await run(["build", "automation", "--bundle", app], ctx)).toBe(0);

    const file = join(app, "Contents/Resources/BDK/Automation/hello.js");
    const code = await readFile(file, "utf8");
    expect(code).not.toMatch(/^\s*import\s/m);
    expect(code).not.toMatch(/^\s*export\s.*\sfrom\s/m);
    expect(code).toContain("call ");
    expect(code).toContain("commands");
    expect(code).toContain("events");

    const after = await resolvedOf(dir);
    const scripts = (after.automation as { scripts: Record<string, Record<string, string>> })
      .scripts;
    expect(scripts.hello).toMatchObject({
      path: "Automation/hello.ts",
      bundle: "BDK/Automation/hello.js",
      bundle_digest: sha256(await readFile(file)),
    });
    expect(after.config_digest).not.toBe(before.config_digest);
    const { config_digest, ...body } = after;
    expect(config_digest).toBe(configDigest(body));
    expect(Object.keys(after).slice(0, 3)).toEqual(Object.keys(before).slice(0, 3));
  });

  it("keeps the exact resolved serialisation when nothing is bundled but the fields", async () => {
    const { dir, app } = await project(HELLO);
    await build(dir);
    await run(["build", "automation", "--bundle", app], real(dir));
    const text = await readFile(join(dir, "bdk.resolved.json"), "utf8");
    expect(text).toBe(`${JSON.stringify(JSON.parse(text), null, 2)}\n`);
  });

  it("does not rebuild when nothing changed, and rebuilds when a module changes", async () => {
    const { dir, app } = await project(HELLO);
    await build(dir);
    expect(await run(["build", "automation", "--bundle", app], real(dir))).toBe(0);
    const file = join(app, "Contents/Resources/BDK/Automation/hello.js");
    const resolvedText = await readFile(join(dir, "bdk.resolved.json"), "utf8");

    const second = real(dir);
    expect(await run(["build", "automation", "--bundle", app, "--json"], second)).toBe(0);
    expect(JSON.parse(second.out.join("")).result.automation).toEqual({
      built: [],
      unchanged: ["hello"],
    });
    expect(await readFile(join(dir, "bdk.resolved.json"), "utf8")).toBe(resolvedText);

    // `build config` drops the bundle fields; the unchanged bundle is recorded again, not rebuilt.
    await build(dir);
    const third = real(dir);
    expect(await run(["build", "automation", "--bundle", app, "--json"], third)).toBe(0);
    expect(JSON.parse(third.out.join("")).result.automation.built).toEqual([]);
    expect(await readFile(join(dir, "bdk.resolved.json"), "utf8")).toBe(resolvedText);

    const bytes = await readFile(file);
    await write(
      dir,
      "node_modules/@bdk/automation/src/client.ts",
      "export function call(command: string): string { return `other ${command}`; }\n",
    );
    const fourth = real(dir);
    expect(await run(["build", "automation", "--bundle", app, "--json"], fourth)).toBe(0);
    expect(JSON.parse(fourth.out.join("")).result.automation.built).toEqual(["hello"]);
    expect(await readFile(file)).not.toEqual(bytes);
  });

  it("rebuilds when the script moves to another directory with the same name", async () => {
    const { dir, app } = await project(HELLO);
    await build(dir);
    expect(await run(["build", "automation", "--bundle", app], real(dir))).toBe(0);

    // The old file stays, unchanged, so only the entry path tells the two builds apart.
    await write(dir, "Scripts/hello.ts", "export default () => 'moved-script';\n");
    await write(
      dir,
      "bdk.toml",
      '[browser]\nname = "Demo"\n\n[automation]\nenabled = true\ndirectory = "Scripts"\n',
    );
    await build(dir);
    const ctx = real(dir);
    expect(await run(["build", "automation", "--bundle", app, "--json"], ctx)).toBe(0);
    expect(JSON.parse(ctx.out.join("")).result.automation.built).toEqual(["hello"]);
    const file = join(app, "Contents/Resources/BDK/Automation/hello.js");
    expect(await readFile(file, "utf8")).toContain("moved-script");
    const scripts = (await resolvedOf(dir)).automation as {
      scripts: Record<string, Record<string, string>>;
    };
    expect(scripts.scripts.hello).toMatchObject({
      path: "Scripts/hello.ts",
      bundle_digest: sha256(await readFile(file)),
    });
  });

  it("keeps a script called __proto__ in the resolved file and the cache", async () => {
    const { dir, app } = await project(HELLO);
    await write(dir, "Automation/__proto__.ts", "export default () => 'proto';\n");
    await build(dir);
    expect(await run(["build", "automation", "--bundle", app], real(dir))).toBe(0);
    const resolved = await resolvedOf(dir);
    const scripts = (resolved.automation as { scripts: Record<string, Record<string, string>> })
      .scripts;
    expect(Object.keys(scripts).sort()).toEqual(["__proto__", "hello"]);
    expect(Object.getPrototypeOf(scripts)).toBe(Object.prototype);
    expect(Object.getOwnPropertyDescriptor(scripts, "__proto__")?.value).toMatchObject({
      path: "Automation/__proto__.ts",
      bundle: "BDK/Automation/__proto__.js",
    });

    const ctx = real(dir);
    expect(await run(["build", "automation", "--bundle", app, "--json"], ctx)).toBe(0);
    expect(JSON.parse(ctx.out.join("")).result.automation).toEqual({
      built: [],
      unchanged: ["__proto__", "hello"],
    });
  });

  it("finds an installed copy whose exports map does not list package.json", async () => {
    const { dir, app } = await project(HELLO);
    await write(
      dir,
      "node_modules/@bdk/automation/package.json",
      JSON.stringify({
        name: "@bdk/automation",
        type: "module",
        exports: {
          ".": "./dist/index.js",
          "./commands": "./dist/commands.js",
          "./events": "./dist/events.js",
        },
      }),
    );
    await write(
      dir,
      "node_modules/@bdk/automation/src/commands.ts",
      'export const commands = { id: "installed-commands" };\n',
    );
    await build(dir);
    expect(await run(["build", "automation", "--bundle", app], real(dir))).toBe(0);
    const code = await readFile(join(app, "Contents/Resources/BDK/Automation/hello.js"), "utf8");
    expect(code).toContain("installed-commands");
  });

  it("rebuilds when the bundle on disk no longer has the recorded digest", async () => {
    const { dir, app } = await project(HELLO);
    await build(dir);
    await run(["build", "automation", "--bundle", app], real(dir));
    await writeFile(join(app, "Contents/Resources/BDK/Automation/hello.js"), "tampered");
    const ctx = real(dir);
    await run(["build", "automation", "--bundle", app, "--json"], ctx);
    expect(JSON.parse(ctx.out.join("")).result.automation.built).toEqual(["hello"]);
  });

  it("takes the bundle from CODESIGNING_FOLDER_PATH", async () => {
    const { dir, app } = await project(HELLO);
    await build(dir);
    const ctx = real(dir, { env: { CODESIGNING_FOLDER_PATH: app } });
    expect(await run(["build", "automation"], ctx)).toBe(0);
    expect(existsSync(join(app, "Contents/Resources/BDK/Automation/hello.js"))).toBe(true);
  });

  it("refuses a Node built-in", async () => {
    const { dir, app } = await project(
      'import { readFileSync } from "node:fs";\nexport default () => readFileSync;\n',
    );
    await build(dir);
    const ctx = real(dir);
    expect(await run(["build", "automation", "--bundle", app], ctx)).toBe(1);
    expect(ctx.err.join("")).toContain("automation.import_not_allowed");
    expect(ctx.err.join("")).toContain('"hello"');
    expect(ctx.err.join("")).toContain("node:fs");
    expect(existsSync(join(app, "Contents/Resources/BDK/Automation/hello.js"))).toBe(false);
    const scripts = (await resolvedOf(dir)).automation as { scripts: Record<string, object> };
    expect(scripts.scripts.hello).not.toHaveProperty("bundle");
  });

  it("refuses a bare built-in name too", async () => {
    const { dir, app } = await project(
      'import { join } from "path";\nexport default () => join;\n',
    );
    await build(dir);
    const ctx = real(dir);
    expect(await run(["build", "automation", "--bundle", app], ctx)).toBe(1);
    expect(ctx.err.join("")).toContain("automation.import_not_allowed");
  });

  it("reports an import that cannot be resolved", async () => {
    const { dir, app } = await project('import x from "left-pad-9000";\nexport default () => x;\n');
    await build(dir);
    const ctx = real(dir);
    expect(await run(["build", "automation", "--bundle", app], ctx)).toBe(1);
    expect(ctx.err.join("")).toContain("automation.import_unresolved");
    expect(ctx.err.join("")).toContain("left-pad-9000");
  });

  it("inlines a package installed in the project", async () => {
    const { dir, app } = await project(
      'import { answer } from "left-pad";\nexport default () => answer;\n',
    );
    await write(
      dir,
      "node_modules/left-pad/package.json",
      '{"name":"left-pad","main":"index.js"}\n',
    );
    await write(dir, "node_modules/left-pad/index.js", "exports.answer = 'inlined-42';\n");
    await build(dir);
    expect(await run(["build", "automation", "--bundle", app], real(dir))).toBe(0);
    const code = await readFile(join(app, "Contents/Resources/BDK/Automation/hello.js"), "utf8");
    expect(code).toContain("inlined-42");
    expect(code).not.toMatch(/^\s*import\s/m);
  });

  it("reports a syntax error as automation.bundle_failed", async () => {
    const { dir, app } = await project("export default function (( {\n");
    await build(dir);
    const ctx = real(dir);
    expect(await run(["build", "automation", "--bundle", app], ctx)).toBe(1);
    expect(ctx.err.join("")).toContain("automation.bundle_failed");
  });

  it("fails with automation.bundler_missing when no bundler can be loaded", async () => {
    const { dir, app } = await project(HELLO);
    await build(dir);
    const ctx = real(dir, { loadBundler: () => Promise.resolve(undefined) });
    expect(await run(["build", "automation", "--bundle", app], ctx)).toBe(3);
    expect(ctx.err.join("")).toContain("automation.bundler_missing");
    expect(ctx.err.join("")).toContain("rolldown@1.2.18");
  });

  it("uses the bundler it is given", async () => {
    const { dir, app } = await project(HELLO);
    await build(dir);
    const bundler: Bundler = {
      rolldown: () =>
        Promise.resolve({
          generate: () => Promise.resolve({ output: [{ type: "chunk", code: "// fake\n" }] }),
          close: () => Promise.resolve(),
        }),
    };
    const ctx = real(dir, { loadBundler: () => Promise.resolve(bundler) });
    expect(await run(["build", "automation", "--bundle", app], ctx)).toBe(0);
    expect(await readFile(join(app, "Contents/Resources/BDK/Automation/hello.js"), "utf8")).toBe(
      "// fake\n",
    );
  });

  it("is a usage error without --bundle", async () => {
    const { dir } = await project(HELLO);
    await build(dir);
    const ctx = real(dir);
    expect(await run(["build", "automation"], ctx)).toBe(2);
    expect(ctx.err.join("")).toContain("--bundle");
  });

  it("asks for `bdk build config` when there is no resolved file", async () => {
    const { dir, app } = await project(HELLO);
    const ctx = real(dir);
    expect(await run(["build", "automation", "--bundle", app], ctx)).toBe(1);
    expect(ctx.err.join("")).toContain("bdk build config");
  });

  it("carries the step data in --json", async () => {
    const { dir, app } = await project(HELLO);
    await build(dir);
    const ctx = real(dir);
    expect(await run(["build", "automation", "--bundle", app, "--json"], ctx)).toBe(0);
    const document = JSON.parse(ctx.out.join(""));
    expect(document).toMatchObject({
      ok: true,
      command: "build automation",
      result: { automation: { built: ["hello"], unchanged: [] } },
    });
  });
});

describe("bdk build automation --watch", () => {
  const bundleOf = (app: string) => join(app, "Contents/Resources/BDK/Automation/hello.js");

  async function recordedDigest(dir: string): Promise<string | undefined> {
    const automation = (await resolvedOf(dir)).automation as {
      scripts: Record<string, Record<string, string>>;
    };
    return automation.scripts.hello?.bundle_digest;
  }

  it("builds once, rebuilds a changed script into the same bundle, survives a build error and stops with 0", async () => {
    const { dir, app } = await project(HELLO);
    await build(dir);
    const fake = fakeWatch();
    const ctx = real(dir, { watch: fake.watch });
    const running = run(["build", "automation", "--bundle", app, "--watch"], ctx);

    // The first build has finished once the loop is watching the automation directory.
    await vi.waitFor(() => expect(fake.watched).toEqual([join(dir, "Automation")]));
    expect(ctx.out.join("")).toContain("watching Automation");
    expect(await recordedDigest(dir)).toBe(sha256(await readFile(bundleOf(app))));

    await write(dir, "Automation/hello.ts", "export default () => 'second';\n");
    fake.change();
    // The bundle is written before the resolved file records its digest: wait for both.
    await vi.waitFor(async () => {
      expect(await readFile(bundleOf(app), "utf8")).toContain("second");
      expect(await recordedDigest(dir)).toBe(sha256(await readFile(bundleOf(app))));
    });

    await write(dir, "Automation/hello.ts", "export default function (( {\n");
    fake.change();
    await vi.waitFor(() => expect(ctx.err.join("")).toContain("automation.bundle_failed"));
    // The last good bundle and its digest are left alone.
    expect(await readFile(bundleOf(app), "utf8")).toContain("second");
    expect(await recordedDigest(dir)).toBe(sha256(await readFile(bundleOf(app))));

    await write(dir, "Automation/hello.ts", "export default () => 'third';\n");
    fake.change();
    // The bundle is written before the resolved file records its digest: wait for both.
    await vi.waitFor(async () => {
      expect(await readFile(bundleOf(app), "utf8")).toContain("third");
      expect(await recordedDigest(dir)).toBe(sha256(await readFile(bundleOf(app))));
    });
    const { config_digest, ...body } = await resolvedOf(dir);
    expect(config_digest).toBe(configDigest(body));

    fake.stop();
    expect(await running).toBe(0);
    expect(fake.watched).toEqual([]);
  });

  it("watches the configured automation directory", async () => {
    const { dir, app } = await project(HELLO);
    await write(dir, "Scripts/hello.ts", "export default () => 'moved';\n");
    await write(
      dir,
      "bdk.toml",
      '[browser]\nname = "Demo"\n\n[automation]\nenabled = true\ndirectory = "Scripts"\n',
    );
    await build(dir);
    const fake = fakeWatch();
    const running = run(
      ["build", "automation", "--bundle", app, "--watch"],
      real(dir, { watch: fake.watch }),
    );
    await vi.waitFor(() => expect(fake.watched).toEqual([join(dir, "Scripts")]));
    fake.stop();
    expect(await running).toBe(0);
  });

  it("reports an error that stops a rebuild and keeps watching", async () => {
    const { dir, app } = await project(HELLO);
    await build(dir);
    const fake = fakeWatch();
    const ctx = real(dir, { watch: fake.watch });
    const running = run(["build", "automation", "--bundle", app, "--watch"], ctx);
    await vi.waitFor(() => expect(fake.watched).toHaveLength(1));

    await rm(join(dir, "bdk.resolved.json"));
    fake.change();
    await vi.waitFor(() => expect(ctx.err.join("")).toContain("automation.resolved_missing"));
    expect(fake.watched).toHaveLength(1);
    fake.stop();
    expect(await running).toBe(0);
  });

  it("fails without watching when the first build cannot run", async () => {
    const { dir, app } = await project(HELLO);
    const fake = fakeWatch();
    const ctx = real(dir, { watch: fake.watch });
    expect(await run(["build", "automation", "--bundle", app, "--watch"], ctx)).toBe(1);
    expect(fake.watched).toEqual([]);
  });
});
