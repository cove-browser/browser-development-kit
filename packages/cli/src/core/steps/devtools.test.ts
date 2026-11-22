import { existsSync } from "node:fs";
import { mkdir, readFile, writeFile } from "node:fs/promises";
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

/** A project whose `[devtools]` table is `devtools`, and where its application bundle goes. */
async function project(devtools: string): Promise<{ dir: string; app: string; root: string }> {
  const dir = await tempDir();
  await write(dir, "bdk.toml", `[browser]\nname = "Demo"\n\n[devtools]\n${devtools}\n`);
  const app = join(dir, "Build", "Demo.app");
  return { dir, app, root: join(app, "Contents/Resources/BDK/DevTools") };
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
  const code = await run(["build", "devtools", "--bundle", app, "--json"], ctx);
  return { code, report: JSON.parse(ctx.out.join("")), err: ctx.err.join("") };
}

describe("bdk build devtools", () => {
  it("copies a replacement front end, nested files included, over what was there", async () => {
    const { dir, app, root } = await project('frontend = "Tools/frontend"');
    await write(dir, "Tools/frontend/index.html", "<html></html>");
    await write(dir, "Tools/frontend/assets/app.js", "console.log(1)");
    await write(dir, "Tools/frontend/assets/deep/style.css", "a{}");
    await write(root, "Frontend/stale.html", "stale");
    await write(root, "Overlay/overlay.js", "stale");
    await build(dir);
    const ctx = real(dir);
    expect(await run(["build", "devtools", "--bundle", app], ctx)).toBe(0);
    expect(ctx.out.join("")).toContain("packaged replacement");
    expect(await readFile(join(root, "Frontend/index.html"), "utf8")).toBe("<html></html>");
    expect(await readFile(join(root, "Frontend/assets/deep/style.css"), "utf8")).toBe("a{}");
    expect(existsSync(join(root, "Frontend/stale.html"))).toBe(false);
    expect(existsSync(join(root, "Overlay"))).toBe(false);
  });

  it("fails when the replacement has no index.html", async () => {
    const { dir, app, root } = await project('frontend = "Tools/frontend"');
    await write(dir, "Tools/frontend/other.html", "x");
    await build(dir);
    const { code, err } = await json(dir, app);
    expect(code).toBe(1);
    expect(err).toContain("devtools.entry_missing");
    expect(existsSync(root)).toBe(false);
  });

  it("fails when the front end is not a directory", async () => {
    const { dir, app } = await project('frontend = "Tools/missing"');
    await build(dir);
    const { code, err } = await json(dir, app);
    expect(code).toBe(1);
    expect(err).toContain("devtools.not_directory");
  });

  it("copies overlay.css and overlay.js and warns about other files", async () => {
    const { dir, app, root } = await project('overlay = "Tools/overlay"');
    await write(dir, "Tools/overlay/overlay.css", "body{}");
    await write(dir, "Tools/overlay/overlay.js", "1;");
    await write(dir, "Tools/overlay/helper.js", "2;");
    await write(dir, "Tools/overlay/logo.png", "3");
    await write(root, "Frontend/index.html", "stale");
    await build(dir);
    const { code, report, err } = await json(dir, app);
    expect(code).toBe(0);
    expect(await readFile(join(root, "Overlay/overlay.css"), "utf8")).toBe("body{}");
    expect(await readFile(join(root, "Overlay/overlay.js"), "utf8")).toBe("1;");
    expect(existsSync(join(root, "Overlay/helper.js"))).toBe(false);
    expect(existsSync(join(root, "Frontend"))).toBe(false);
    expect(report.result.devtools).toMatchObject({
      mode: "overlay",
      packaged: true,
      files: ["Overlay/overlay.css", "Overlay/overlay.js"],
    });
    expect(err.match(/devtools\.overlay_files_ignored/g)).toHaveLength(1);
    expect(err).toContain("helper.js, logo.png");
  });

  it("packages a single overlay file", async () => {
    const { dir, app, root } = await project('overlay = "Tools/overlay"');
    await write(dir, "Tools/overlay/overlay.js", "1;");
    await build(dir);
    expect((await json(dir, app)).code).toBe(0);
    expect(existsSync(join(root, "Overlay/overlay.css"))).toBe(false);
    expect(existsSync(join(root, "Overlay/overlay.js"))).toBe(true);
  });

  it("warns that an overlay is ignored with a replacement front end", async () => {
    const { dir, app, root } = await project(
      'frontend = "Tools/frontend"\noverlay = "Tools/overlay"',
    );
    await write(dir, "Tools/frontend/index.html", "<html></html>");
    await write(dir, "Tools/overlay/overlay.js", "1;");
    await build(dir);
    const { code, err } = await json(dir, app);
    expect(code).toBe(0);
    expect(err).toContain("devtools.overlay_ignored");
    expect(existsSync(join(root, "Overlay"))).toBe(false);
    expect(existsSync(join(root, "Frontend/index.html"))).toBe(true);
  });

  it("fails on an overlay directory with neither file", async () => {
    const { dir, app } = await project('overlay = "Tools/overlay"');
    await write(dir, "Tools/overlay/readme.txt", "x");
    await build(dir);
    const { code, err } = await json(dir, app);
    expect(code).toBe(1);
    expect(err).toContain("devtools.overlay_empty");
  });

  it("removes a stale DevTools directory when nothing is configured", async () => {
    const { dir, app, root } = await project("protocol = true");
    await write(root, "Frontend/index.html", "stale");
    await build(dir);
    const ctx = real(dir);
    expect(await run(["build", "devtools", "--bundle", app], ctx)).toBe(0);
    expect(ctx.out.join("")).toContain("nothing packaged");
    expect(existsSync(root)).toBe(false);
    const { report } = await json(dir, app);
    expect(report.result.devtools).toMatchObject({ packaged: false });
  });

  it("removes it when the front end is disabled", async () => {
    const { dir, app, root } = await project('frontend = "disabled"');
    await write(root, "Overlay/overlay.js", "stale");
    await build(dir);
    expect((await json(dir, app)).code).toBe(0);
    expect(existsSync(root)).toBe(false);
  });

  it("refuses a URL, however validation upstream went", async () => {
    const { dir, app, root } = await project('frontend = "default"');
    await build(dir);
    for (const [key, value] of [
      ["frontend", "https://example.com/devtools"],
      ["overlay", "//cdn.example.com/overlay"],
    ] as const) {
      const file = join(dir, "bdk.resolved.json");
      const resolved = JSON.parse(await readFile(file, "utf8"));
      resolved.devtools[key] = value;
      await writeFile(file, JSON.stringify(resolved));
      const { code, err } = await json(dir, app);
      expect(code).toBe(1);
      expect(err).toContain("devtools.url_not_allowed");
      expect(existsSync(root)).toBe(false);
    }
  });

  it("refuses a path outside the project", async () => {
    const { dir, app } = await project('frontend = "default"');
    await build(dir);
    const file = join(dir, "bdk.resolved.json");
    const resolved = JSON.parse(await readFile(file, "utf8"));
    resolved.devtools.frontend = "../elsewhere";
    await writeFile(file, JSON.stringify(resolved));
    const { code, err } = await json(dir, app);
    expect(code).toBe(1);
    expect(err).toContain("config.path_escape");
  });

  it("does nothing on a second run, and copies again when an input or the output changed", async () => {
    const { dir, app, root } = await project('frontend = "Tools/frontend"');
    await write(dir, "Tools/frontend/index.html", "<html></html>");
    await build(dir);
    expect((await json(dir, app)).report.result.devtools.unchanged).toBe(false);
    expect((await json(dir, app)).report.result.devtools.unchanged).toBe(true);

    await write(dir, "Tools/frontend/index.html", "<html>new</html>");
    expect((await json(dir, app)).report.result.devtools.unchanged).toBe(false);
    expect(await readFile(join(root, "Frontend/index.html"), "utf8")).toBe("<html>new</html>");

    await writeFile(join(root, "Frontend/index.html"), "tampered");
    expect((await json(dir, app)).report.result.devtools.unchanged).toBe(false);
    expect(await readFile(join(root, "Frontend/index.html"), "utf8")).toBe("<html>new</html>");
  });

  it("reports the mode, files and whether anything changed with --json", async () => {
    const { dir, app } = await project('frontend = "Tools/frontend"');
    await write(dir, "Tools/frontend/index.html", "<html></html>");
    await write(dir, "Tools/frontend/a/b.js", "1");
    await build(dir);
    const { code, report } = await json(dir, app);
    expect(code).toBe(0);
    expect(report.result.devtools).toEqual({
      mode: "replacement",
      packaged: true,
      unchanged: false,
      files: ["Frontend/a/b.js", "Frontend/index.html"],
    });
  });

  it("takes the bundle from CODESIGNING_FOLDER_PATH", async () => {
    const { dir, app, root } = await project('frontend = "Tools/frontend"');
    await write(dir, "Tools/frontend/index.html", "<html></html>");
    await build(dir);
    const ctx = real(dir, { env: { CODESIGNING_FOLDER_PATH: app } });
    expect(await run(["build", "devtools"], ctx)).toBe(0);
    expect(existsSync(join(root, "Frontend/index.html"))).toBe(true);
  });

  it("is a usage error without --bundle", async () => {
    const { dir } = await project('frontend = "default"');
    await build(dir);
    const ctx = real(dir);
    expect(await run(["build", "devtools"], ctx)).toBe(2);
    expect(ctx.err.join("")).toContain("--bundle");
  });

  it("needs bdk.resolved.json", async () => {
    const { dir, app } = await project('frontend = "default"');
    const ctx = real(dir);
    expect(await run(["build", "devtools", "--bundle", app], ctx)).toBe(1);
    expect(ctx.err.join("")).toContain("devtools.resolved_missing");
  });
});
