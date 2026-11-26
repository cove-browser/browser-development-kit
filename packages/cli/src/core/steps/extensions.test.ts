import { existsSync } from "node:fs";
import { mkdir, readFile, symlink, writeFile } from "node:fs/promises";
import { join } from "node:path";
import { describe, expect, it } from "vitest";
import { createAssets } from "../../assets.js";
import { tempDir, testContext } from "../../test-support.js";
import { resolveConfig } from "../config/index.js";
import { run } from "../run.js";

async function write(root: string, file: string, text: string): Promise<void> {
  await mkdir(join(root, file, ".."), { recursive: true });
  await writeFile(join(root, file), text);
}

function manifest(name: string, extra: object = {}): string {
  return JSON.stringify({ name, version: "1.0", manifest_version: 3, ...extra });
}

const READER = manifest("Reader", {
  permissions: ["storage", "tabs"],
  host_permissions: ["https://*/*"],
  optional_permissions: ["downloads"],
});

/** A project whose `[extensions.bundled]` table is `bundled`, and where its application bundle goes. */
async function project(bundled: string): Promise<{ dir: string; app: string; resources: string }> {
  const dir = await tempDir();
  await write(dir, "bdk.toml", `[browser]\nname = "Demo"\n\n[extensions.bundled]\n${bundled}\n`);
  const app = join(dir, "Build", "Demo.app");
  return { dir, app, resources: join(app, "Contents/Resources") };
}

const real = (dir: string, overrides = {}) =>
  testContext({
    cwd: dir,
    assets: createAssets({ packaged: false }),
    resolveConfig,
    ...overrides,
  });

async function build(dir: string): Promise<void> {
  expect(await run(["build", "config"], real(dir))).toBe(0);
}

async function json(dir: string, app: string) {
  const ctx = real(dir);
  const code = await run(["build", "extensions", "--bundle", app, "--json"], ctx);
  return { code, report: JSON.parse(ctx.out.join("")), err: ctx.err.join("") };
}

describe("bdk build extensions", () => {
  it("copies each bundled extension to the same path under Contents/Resources and prints its permissions", async () => {
    const { dir, app, resources } = await project(
      'reader = { path = "Extensions/Reader", required = true }\nnotes = "Extensions/Notes"',
    );
    await write(dir, "Extensions/Reader/manifest.json", READER);
    await write(dir, "Extensions/Reader/js/deep/worker.js", "1;");
    await write(dir, "Extensions/Reader/.DS_Store", "x");
    await write(dir, "Extensions/Notes/manifest.json", manifest("Notes"));
    await build(dir);
    const ctx = real(dir);
    expect(await run(["build", "extensions", "--bundle", app], ctx)).toBe(0);
    expect(await readFile(join(resources, "Extensions/Reader/manifest.json"), "utf8")).toBe(READER);
    expect(await readFile(join(resources, "Extensions/Reader/js/deep/worker.js"), "utf8")).toBe(
      "1;",
    );
    expect(existsSync(join(resources, "Extensions/Reader/.DS_Store"))).toBe(false);
    expect(existsSync(join(resources, "Extensions/Notes/manifest.json"))).toBe(true);
    expect(ctx.out.join("")).toBe(
      [
        "bdk build extensions: copied 2 bundled extension(s), 0 unchanged.",
        "  notes (Extensions/Notes): declares no permissions",
        "  reader (Extensions/Reader, required): permissions: storage, tabs; optional permissions: downloads; host permissions: https://*/*",
        "",
      ].join("\n"),
    );
  });

  it("reports the extensions and their permissions with --json", async () => {
    const { dir, app } = await project('reader = "Extensions/Reader"');
    await write(dir, "Extensions/Reader/manifest.json", READER);
    await build(dir);
    const { code, report } = await json(dir, app);
    expect(code).toBe(0);
    expect(report.result.extensions).toEqual({
      copied: true,
      bundle: app,
      extensions: [
        {
          name: "reader",
          path: "Extensions/Reader",
          required: false,
          unchanged: false,
          permissions: {
            permissions: ["storage", "tabs"],
            optional_permissions: ["downloads"],
            host_permissions: ["https://*/*"],
            optional_host_permissions: [],
          },
        },
      ],
      removed: [],
      kept: [],
    });
  });

  it("replaces what was at the path, and does nothing on a second run until the copy changes", async () => {
    const { dir, app, resources } = await project('reader = "Extensions/Reader"');
    await write(dir, "Extensions/Reader/manifest.json", READER);
    await write(resources, "Extensions/Reader/stale.js", "stale");
    await build(dir);
    expect((await json(dir, app)).report.result.extensions.extensions[0].unchanged).toBe(false);
    expect(existsSync(join(resources, "Extensions/Reader/stale.js"))).toBe(false);
    expect((await json(dir, app)).report.result.extensions.extensions[0].unchanged).toBe(true);

    await writeFile(join(resources, "Extensions/Reader/manifest.json"), "tampered");
    expect((await json(dir, app)).report.result.extensions.extensions[0].unchanged).toBe(false);
    expect(await readFile(join(resources, "Extensions/Reader/manifest.json"), "utf8")).toBe(READER);
  });

  it("fails when the extension changed since bdk build config, and copies nothing", async () => {
    const { dir, app, resources } = await project(
      'notes = "Extensions/Notes"\nreader = "Extensions/Reader"',
    );
    await write(dir, "Extensions/Notes/manifest.json", manifest("Notes"));
    await write(dir, "Extensions/Reader/manifest.json", READER);
    await build(dir);
    await write(dir, "Extensions/Reader/background.js", "added later");
    const { code, err } = await json(dir, app);
    expect(code).toBe(1);
    expect(err).toContain("extensions.resolved_stale");
    expect(err).toContain("Run `bdk build config` again");
    expect(existsSync(join(resources, "Extensions"))).toBe(false);
  });

  it("copies a link that stays inside the extension as what it points at", async () => {
    const { dir, app, resources } = await project('reader = "Extensions/Reader"');
    await write(dir, "Extensions/Reader/manifest.json", READER);
    await write(dir, "Extensions/Reader/lib/shared.js", "2;");
    await symlink("lib", join(dir, "Extensions/Reader/vendor"));
    await build(dir);
    expect((await json(dir, app)).code).toBe(0);
    expect(await readFile(join(resources, "Extensions/Reader/vendor/shared.js"), "utf8")).toBe(
      "2;",
    );
  });

  it.each([
    [
      "leaves the extension",
      "../../Other",
      "config.path_escape",
      /leaves the extension's directory/,
    ],
    ["points nowhere", "missing", "extensions.bad_link", /a link to nothing/],
    ["leads back to a directory it is in", "..", "extensions.bad_link", /links back/],
  ])("refuses a link that %s", async (_, target, code, message) => {
    const { dir, app, resources } = await project('reader = "Extensions/Reader"');
    await write(dir, "Extensions/Reader/manifest.json", READER);
    await write(dir, "Extensions/Other/secret.txt", "secret");
    await mkdir(join(dir, "Extensions/Reader/lib"), { recursive: true });
    await symlink(target, join(dir, "Extensions/Reader/lib/link"));
    // The resolved file as `bdk build config` writes it: the step checks links before any digest.
    await write(
      dir,
      "bdk.resolved.json",
      JSON.stringify({
        extensions: {
          bundled: { reader: { path: "Extensions/Reader", required: false, digest: "sha256:x" } },
        },
      }),
    );
    const { code: exit, err } = await json(dir, app);
    expect(exit).toBe(1);
    expect(err).toContain(code);
    expect(err).toMatch(message);
    expect(existsSync(join(resources, "Extensions"))).toBe(false);
  });

  it("refuses a resolved path outside the project, however validation upstream went", async () => {
    const { dir, app } = await project('reader = "Extensions/Reader"');
    await write(dir, "Extensions/Reader/manifest.json", READER);
    await build(dir);
    const file = join(dir, "bdk.resolved.json");
    const resolved = JSON.parse(await readFile(file, "utf8"));
    resolved.extensions.bundled.reader.path = "../elsewhere";
    await writeFile(file, JSON.stringify(resolved));
    const { code, err } = await json(dir, app);
    expect(code).toBe(1);
    expect(err).toContain("config.path_escape");
  });

  it("removes its own copy of an extension the configuration no longer lists", async () => {
    const { dir, app, resources } = await project(
      'notes = "Extensions/Notes"\nreader = "Extensions/Reader"',
    );
    await write(dir, "Extensions/Notes/manifest.json", manifest("Notes"));
    await write(dir, "Extensions/Reader/manifest.json", READER);
    await build(dir);
    expect((await json(dir, app)).code).toBe(0);

    await write(
      dir,
      "bdk.toml",
      '[browser]\nname = "Demo"\n\n[extensions.bundled]\nnotes = "Extensions/Notes"\n',
    );
    await build(dir);
    const { code, report, err } = await json(dir, app);
    expect(code).toBe(0);
    expect(report.result.extensions.removed).toEqual(["Extensions/Reader"]);
    expect(existsSync(join(resources, "Extensions/Reader"))).toBe(false);
    expect(existsSync(join(resources, "Extensions/Notes/manifest.json"))).toBe(true);
    expect(err).toBe("");

    // With none left, the empty parent goes too.
    await write(dir, "bdk.toml", '[browser]\nname = "Demo"\n');
    await build(dir);
    const ctx = real(dir);
    expect(await run(["build", "extensions", "--bundle", app], ctx)).toBe(0);
    expect(ctx.out.join("")).toContain("removed 1 no longer listed");
    expect(existsSync(join(resources, "Extensions"))).toBe(false);
    expect(existsSync(resources)).toBe(true);
  });

  it("leaves a copy it cannot tell is its own, with a warning when it changed since", async () => {
    const { dir, app, resources } = await project('reader = "Extensions/Reader"');
    await write(dir, "Extensions/Reader/manifest.json", READER);
    await write(resources, "Extensions/Unknown/manifest.json", manifest("Unknown"));
    await build(dir);
    expect((await json(dir, app)).code).toBe(0);
    await write(resources, "Extensions/Reader/added.js", "changed in the bundle");

    await write(dir, "bdk.toml", '[browser]\nname = "Demo"\n');
    await build(dir);
    const { code, report, err } = await json(dir, app);
    expect(code).toBe(0);
    expect(report.result.extensions).toMatchObject({ removed: [], kept: ["Extensions/Reader"] });
    expect(err).toContain("extensions.stale_copy_kept");
    expect(existsSync(join(resources, "Extensions/Reader/added.js"))).toBe(true);
    // Never copied by the step, so never removed by it.
    expect(existsSync(join(resources, "Extensions/Unknown/manifest.json"))).toBe(true);
  });

  it("does nothing without bundled extensions", async () => {
    const dir = await tempDir();
    await write(dir, "bdk.toml", '[browser]\nname = "Demo"\n');
    await build(dir);
    const ctx = real(dir);
    expect(await run(["build", "extensions", "--bundle", join(dir, "Demo.app")], ctx)).toBe(0);
    expect(ctx.out.join("")).toContain("nothing to do, there are no bundled extensions");
  });

  it("takes the bundle from CODESIGNING_FOLDER_PATH", async () => {
    const { dir, app, resources } = await project('reader = "Extensions/Reader"');
    await write(dir, "Extensions/Reader/manifest.json", READER);
    await build(dir);
    const ctx = real(dir, { env: { CODESIGNING_FOLDER_PATH: app } });
    expect(await run(["build", "extensions"], ctx)).toBe(0);
    expect(existsSync(join(resources, "Extensions/Reader/manifest.json"))).toBe(true);
  });

  it("is a usage error without --bundle", async () => {
    const { dir } = await project('reader = "Extensions/Reader"');
    await write(dir, "Extensions/Reader/manifest.json", READER);
    await build(dir);
    const ctx = real(dir);
    expect(await run(["build", "extensions"], ctx)).toBe(2);
    expect(ctx.err.join("")).toContain("--bundle");
  });

  it("needs bdk.resolved.json", async () => {
    const { dir, app } = await project('reader = "Extensions/Reader"');
    const ctx = real(dir);
    expect(await run(["build", "extensions", "--bundle", app], ctx)).toBe(1);
    expect(ctx.err.join("")).toContain("extensions.resolved_missing");
  });
});
