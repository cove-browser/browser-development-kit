import {
  mkdir,
  readdir,
  readFile,
  realpath,
  stat,
  symlink,
  truncate,
  writeFile,
} from "node:fs/promises";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { describe, expect, it } from "vitest";
import { memoryAssets, tempDir, testContext } from "../../test-support.js";
import { MAX_PAGE_FILE_BYTES } from "../pages/files.js";
import { run } from "../run.js";

const SDK = {
  "js/internal/index.js": "export const index = 1;\n",
  "js/internal/settings.js": "export const settings = 2;\n",
};
const PAGE =
  '<!doctype html><html><body><script type="module" src="./main.js"></script></body></html>';

type Routes = Record<string, { mode: "bundled"; path: string } | { mode: "chromium" }>;

interface Project {
  dir: string;
  app: string;
}

/** A project with the given files, a `bdk.resolved.json` holding `routes`, and an empty application path. */
async function project(
  files: Record<string, string>,
  routes: Routes,
  toml = '[browser]\nname = "Demo"\n',
): Promise<Project> {
  const dir = await tempDir();
  await writeFile(join(dir, "bdk.toml"), toml);
  await writeFile(join(dir, "bdk.resolved.json"), JSON.stringify({ routes }));
  for (const [name, text] of Object.entries(files)) {
    await mkdir(dirname(join(dir, name)), { recursive: true });
    await writeFile(join(dir, name), text);
  }
  return { dir, app: join(await tempDir(), "Demo.app") };
}

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

const context = (p: Project, extra: Parameters<typeof testContext>[0] = {}) =>
  testContext({ cwd: p.dir, assets: memoryAssets(SDK), ...extra });

describe("bdk build pages, plain pages", () => {
  const settings: Routes = { settings: { mode: "bundled", path: "Pages/Settings" } };

  it("copies a page as it is, skipping dot-files, and copies the SDK", async () => {
    const p = await project(
      {
        "Pages/Settings/index.html": PAGE,
        "Pages/Settings/main.js": "import '/@bdk/internal/settings.js';\n",
        "Pages/Settings/css/a.css": "a{}",
        "Pages/Settings/.DS_Store": "x",
        "Pages/Other/index.html": PAGE,
      },
      { ...settings, history: { mode: "chromium" } },
    );
    const ctx = context(p);
    expect(await run(["build", "pages", "--bundle", p.app, "--json"], ctx)).toBe(0);
    const resources = join(p.app, "Contents", "Resources");
    expect(await tree(resources)).toEqual([
      "BDK/js/internal/index.js",
      "BDK/js/internal/settings.js",
      "Pages/Settings/css/a.css",
      "Pages/Settings/index.html",
      "Pages/Settings/main.js",
    ]);
    expect(await readFile(join(resources, "BDK/js/internal/settings.js"), "utf8")).toBe(
      SDK["js/internal/settings.js"],
    );
    const document = JSON.parse(ctx.out.join(""));
    expect(document.result.pages).toEqual({
      bundle: p.app,
      wrote: true,
      pages: [{ route: "settings", path: "Pages/Settings", files: 3 }],
      sdkModules: 2,
    });
  });

  it("removes what an earlier build left in the page and SDK directories", async () => {
    const p = await project({ "Pages/Settings/index.html": PAGE }, settings);
    const resources = join(p.app, "Contents", "Resources");
    await mkdir(join(resources, "Pages/Settings"), { recursive: true });
    await mkdir(join(resources, "BDK/js/internal"), { recursive: true });
    await writeFile(join(resources, "Pages/Settings/stale.js"), "old");
    await writeFile(join(resources, "BDK/js/internal/gone.js"), "old");
    await writeFile(join(resources, "Pages/Keep.txt"), "kept");
    expect(await run(["build", "pages", "--bundle", p.app], context(p))).toBe(0);
    expect(await tree(resources)).toEqual([
      "BDK/js/internal/index.js",
      "BDK/js/internal/settings.js",
      "Pages/Keep.txt",
      "Pages/Settings/index.html",
    ]);
  });

  it("copies a route that an override points outside the pages directory", async () => {
    const p = await project(
      {
        "Pages/Settings/index.html": PAGE,
        "Custom/Agents/index.html": PAGE,
        "Custom/Agents/a.js": "1",
      },
      { agents: { mode: "bundled", path: "Custom/Agents" } },
    );
    expect(await run(["build", "pages", "--bundle", p.app], context(p))).toBe(0);
    expect(await tree(join(p.app, "Contents", "Resources", "Custom"))).toEqual([
      "Agents/a.js",
      "Agents/index.html",
    ]);
  });

  it("does nothing, successfully, when no route is bundled", async () => {
    const p = await project({}, { history: { mode: "chromium" } });
    expect(await run(["build", "pages", "--bundle", p.app], context(p))).toBe(0);
    expect(await tree(p.app)).toEqual([]);
  });

  it("reads the resolved file named by --resolved", async () => {
    const p = await project({ "Pages/Settings/index.html": PAGE }, {});
    const other = join(p.dir, "elsewhere.json");
    await writeFile(other, JSON.stringify({ routes: settings }));
    expect(
      await run(["build", "pages", "--bundle", p.app, "--resolved", "elsewhere.json"], context(p)),
    ).toBe(0);
    expect(await tree(join(p.app, "Contents", "Resources", "Pages"))).toEqual([
      "Settings/index.html",
    ]);
  });

  it("fails and points at `bdk build config` when the resolved file is missing", async () => {
    const p = await project({}, settings);
    const ctx = context(p);
    const code = await run(["build", "pages", "--bundle", p.app, "--resolved", "nope.json"], ctx);
    expect(code).toBe(1);
    const err = ctx.err.join("");
    expect(err).toContain("[pages.resolved_missing]");
    expect(err).toContain("nope.json");
    expect(err).toContain("bdk build config");
  });

  it("rejects a resolved file that is not JSON or has a bundled route with no path", async () => {
    const p = await project({}, settings);
    await writeFile(join(p.dir, "bdk.resolved.json"), "{");
    const a = context(p);
    expect(await run(["build", "pages", "--bundle", p.app], a)).toBe(1);
    expect(a.err.join("")).toContain("[pages.resolved_invalid]");
    await writeFile(
      join(p.dir, "bdk.resolved.json"),
      '{"routes":{"x":{"mode":"bundled","path":"../x"}}}',
    );
    const b = context(p);
    expect(await run(["build", "pages", "--bundle", p.app], b)).toBe(1);
    expect(b.err.join("")).toContain("no usable `path`");
  });

  it("fails on content the policy blocks, naming the file, line and fix, and writes nothing", async () => {
    const p = await project(
      { "Pages/Settings/index.html": '<p>\n<button onclick="go()">x</button>\n<script>1</script>' },
      settings,
    );
    const ctx = context(p);
    expect(await run(["build", "pages", "--bundle", p.app], ctx)).toBe(1);
    const err = ctx.err.join("");
    const file = join(p.dir, "Pages/Settings/index.html");
    expect(err).toContain(`${file}:2`);
    expect(err).toContain(`${file}:3`);
    expect(err).toContain("[pages.csp]");
    expect(err).toContain("addEventListener");
    expect(await tree(p.app)).toEqual([]);
  });

  it("fails on a file over the size limit with its own code", async () => {
    const p = await project({ "Pages/Settings/index.html": PAGE }, settings);
    const big = join(p.dir, "Pages/Settings/model.bin");
    await writeFile(big, "");
    await truncate(big, MAX_PAGE_FILE_BYTES + 1);
    const ctx = context(p);
    expect(await run(["build", "pages", "--bundle", p.app], ctx)).toBe(1);
    expect(ctx.err.join("")).toContain("[pages.too_large]");
    expect(ctx.err.join("")).toContain(big);
    await truncate(big, MAX_PAGE_FILE_BYTES);
    expect(await run(["build", "pages", "--bundle", p.app], context(p))).toBe(0);
  });

  it("exits 3 with cli.asset_missing when the executable carries no SDK modules", async () => {
    const p = await project({ "Pages/Settings/index.html": PAGE }, settings);
    const ctx = context(p, { assets: memoryAssets({}) });
    expect(await run(["build", "pages", "--bundle", p.app], ctx)).toBe(3);
    expect(ctx.err.join("")).toContain("[cli.asset_missing]");
  });
});

describe("bdk build pages, the application bundle", () => {
  const settings: Routes = { settings: { mode: "bundled", path: "Pages/Settings" } };

  it("defaults to CODESIGNING_FOLDER_PATH in an Xcode build", async () => {
    const p = await project({ "Pages/Settings/index.html": PAGE }, settings);
    const ctx = context(p, { env: { CODESIGNING_FOLDER_PATH: p.app } });
    expect(await run(["build", "pages"], ctx)).toBe(0);
    expect(await stat(join(p.app, "Contents/Resources/Pages/Settings/index.html"))).toBeTruthy();
  });

  it("falls back to TARGET_BUILD_DIR and WRAPPER_NAME", async () => {
    const p = await project({ "Pages/Settings/index.html": PAGE }, settings);
    const ctx = context(p, { env: { TARGET_BUILD_DIR: dirname(p.app), WRAPPER_NAME: "Demo.app" } });
    expect(await run(["build", "pages"], ctx)).toBe(0);
    expect(await stat(join(p.app, "Contents/Resources/BDK/js/internal/index.js"))).toBeTruthy();
  });

  it("is a usage error without a bundle", async () => {
    const p = await project({}, settings);
    expect(await run(["build", "pages"], context(p))).toBe(2);
  });

  it("rejects a pages flag on another step", async () => {
    const ctx = testContext();
    expect(await run(["build", "config", "--resolved", "x.json"], ctx)).toBe(2);
    expect(ctx.err.join("")).toContain("applies to `bdk build codegen`, `bdk build pages` only");
  });
});

describe("bdk build pages, a Vite project", () => {
  const workspace = resolve(
    dirname(fileURLToPath(import.meta.url)),
    "..",
    "..",
    "..",
    "..",
    "js",
    "vite",
  );
  const config = 'import { bdk } from "@bdk/vite";\nexport default { plugins: [bdk()] };\n';
  const routes: Routes = {
    settings: { mode: "bundled", path: "Pages/Settings" },
    "new-tab": { mode: "bundled", path: "Pages/NewTab" },
  };

  /** Links the workspace's own Vite and preset into the pages directory's node_modules. */
  async function linkVite(dir: string): Promise<void> {
    const modules = join(dir, "Pages", "node_modules");
    await mkdir(join(modules, "@bdk"), { recursive: true });
    await symlink(await realpath(join(workspace, "node_modules", "vite")), join(modules, "vite"));
    await symlink(workspace, join(modules, "@bdk", "vite"));
  }

  const files = (script: string) => ({
    "Pages/package.json": '{"type":"module"}',
    "Pages/vite.config.ts": config,
    "Pages/Settings/index.html":
      '<!doctype html><html><body><script type="module" src="./main.ts"></script></body></html>',
    "Pages/Settings/main.ts":
      'import { settings } from "@bdk/internal/settings";\nimport { name } from "../shared/name";\nconsole.log(settings, name);\n',
    "Pages/NewTab/index.html": `<!doctype html><html><body>${script}</body></html>`,
    "Pages/NewTab/main.ts": 'import { name } from "../shared/name";\nconsole.log(name);\n',
    "Pages/shared/name.ts": 'export const name = "demo";\n',
  });
  const ok = '<script type="module" src="./main.ts"></script>';

  it("builds each page alone with the project's Vite and copies its output", async () => {
    const p = await project(files(ok), routes);
    await linkVite(p.dir);
    const ctx = context(p);
    expect(await run(["build", "pages", "--bundle", p.app], ctx)).toBe(0);
    const resources = join(p.app, "Contents", "Resources");
    const built = await tree(join(resources, "Pages"));
    expect(built).toContain("Settings/index.html");
    expect(built).toContain("NewTab/index.html");
    expect(built.filter((f) => f.startsWith("Settings/assets/") && f.endsWith(".js"))).toHaveLength(
      1,
    );
    expect(built.some((f) => f.endsWith(".ts") || f.startsWith("shared/"))).toBe(false);
    const html = await readFile(join(resources, "Pages/Settings/index.html"), "utf8");
    expect(html).toContain('src="./assets/');
    const js = built.find((f) => f.startsWith("Settings/assets/") && f.endsWith(".js")) ?? "";
    expect(await readFile(join(resources, "Pages", js), "utf8")).toContain(
      '"/@bdk/internal/settings.js"',
    );
    expect(await tree(join(resources, "BDK"))).toEqual([
      "js/internal/index.js",
      "js/internal/settings.js",
    ]);
    expect(await readdir(p.dir)).not.toContain("dist");
  });

  it("fails with pages.vite_failed when the preset stops the build", async () => {
    const p = await project(files('<script type="module">console.log(1)</script>'), routes);
    await linkVite(p.dir);
    const ctx = context(p);
    expect(await run(["build", "pages", "--bundle", p.app], ctx)).toBe(1);
    const err = ctx.err.join("");
    expect(err).toContain("[pages.vite_failed]");
    expect(err).toContain("NewTab");
    expect(err).toContain("An inline <script> is blocked");
    expect(await tree(p.app)).toEqual([]);
  });

  it("uses the pages directory named in bdk.toml", async () => {
    const p = await project(
      { ...files(ok) },
      { settings: { mode: "bundled", path: "Pages/Settings" } },
      '[pages]\ndirectory = "Pages"\n',
    );
    await linkVite(p.dir);
    expect(await run(["build", "pages", "--bundle", p.app], context(p))).toBe(0);
  });
});
