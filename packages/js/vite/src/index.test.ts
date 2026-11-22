import { mkdir, mkdtemp, readdir, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { build } from "vite";
import { afterEach, describe, expect, it } from "vitest";
import { bdk, internalUrl, pageDirectories } from "./index.js";

const created: string[] = [];

afterEach(async () => {
  delete process.env.BDK_PAGE;
  await Promise.all(created.splice(0).map((dir) => rm(dir, { recursive: true, force: true })));
});

const HTML = (body: string, head = "") =>
  `<!doctype html><html><head><title>t</title>${head}</head><body>${body}</body></html>`;

/** A pages root with two pages, a shared module and a directory that is not a page. */
async function fixture(overrides: Record<string, string> = {}): Promise<string> {
  const root = await mkdtemp(join(tmpdir(), "bdk-vite-"));
  created.push(root);
  const files: Record<string, string> = {
    "Settings/index.html": HTML(
      '<script type="module" src="./main.ts"></script>',
      '<link rel="stylesheet" href="./style.css">',
    ),
    "Settings/main.ts": `import { settings } from "@bdk/internal/settings";\nimport { greet } from "../shared/greet";\nsettings.get(greet("x"));\n`,
    "Settings/style.css": "body { margin: 0 }",
    "NewTab/index.html": HTML('<script type="module" src="./main.ts"></script>'),
    "NewTab/main.ts": `import { greet } from "../shared/greet";\nconsole.log(greet("tab"));\n`,
    "shared/greet.ts":
      'export const greet = (name: string): string => ["hello", name].join(" ");\n',
    ".hidden/index.html": HTML(""),
    ...overrides,
  };
  for (const [name, text] of Object.entries(files)) {
    await mkdir(join(root, name, ".."), { recursive: true });
    await writeFile(join(root, name), text);
  }
  return root;
}

async function listing(dir: string): Promise<string[]> {
  return (await readdir(dir, { recursive: true, withFileTypes: true }))
    .filter((e) => e.isFile())
    .map((e) => join(e.parentPath, e.name).slice(dir.length + 1))
    .sort();
}

async function run(root: string, out: string) {
  await build({
    root,
    configFile: false,
    logLevel: "silent",
    plugins: [bdk()],
    build: { outDir: out, emptyOutDir: true },
  });
}

describe("pageDirectories", () => {
  it("lists directories with an index.html, skipping dot-directories and node_modules", async () => {
    const root = await fixture({ "node_modules/x/index.html": HTML("") });
    expect(pageDirectories(root)).toEqual(["NewTab", "Settings"]);
  });
});

describe("internalUrl", () => {
  it("maps @bdk/internal/<module> to the runtime's path", () => {
    expect(internalUrl("@bdk/internal/settings")).toBe("/@bdk/internal/settings.js");
    expect(internalUrl("@bdk/internal/settings.js")).toBe("/@bdk/internal/settings.js");
    expect(internalUrl("@bdk/automation")).toBeUndefined();
    expect(internalUrl("react")).toBeUndefined();
  });
});

describe("bdk() with Vite", () => {
  it("emits one directory per page, with relative URLs and the SDK import left to the runtime", async () => {
    const root = await fixture();
    const out = join(root, "out");
    await run(root, out);
    const files = await listing(out);
    expect(files).toContain("Settings/index.html");
    expect(files).toContain("NewTab/index.html");
    expect(files.some((f) => f.startsWith("shared/") || f.startsWith(".hidden/"))).toBe(false);
    const html = await readFile(join(out, "Settings/index.html"), "utf8");
    expect(html).toMatch(/src="\.\/assets\/|src="\.\.\/assets\//);
    expect(html).not.toContain('src="/');
    expect(html).not.toContain("modulepreload-polyfill");
    const scripts = files.filter((f) => f.endsWith(".js"));
    const sources = await Promise.all(scripts.map((f) => readFile(join(out, f), "utf8")));
    expect(sources.some((s) => s.includes('"/@bdk/internal/settings.js"'))).toBe(true);
    expect(sources.some((s) => s.includes('@bdk/internal/settings"'))).toBe(false);
  });

  it("builds one page alone, with index.html and its files at the top of the output", async () => {
    const root = await fixture();
    const out = join(root, "out");
    process.env.BDK_PAGE = "Settings";
    await run(root, out);
    const files = await listing(out);
    expect(files).toContain("index.html");
    expect(files.filter((f) => f.endsWith(".js") || f.endsWith(".css"))).toHaveLength(2);
    expect(files.every((f) => f === "index.html" || f.startsWith("assets/"))).toBe(true);
    const html = await readFile(join(out, "index.html"), "utf8");
    expect(html).toContain('src="./assets/');
    expect(html).toContain('href="./assets/');
  });

  it("does not inline assets", async () => {
    const root = await fixture({
      "Settings/index.html": HTML(
        '<img src="./dot.svg"><script type="module" src="./main.ts"></script>',
      ),
      "Settings/dot.svg": "<svg xmlns='http://www.w3.org/2000/svg'/>",
    });
    const out = join(root, "out");
    process.env.BDK_PAGE = "Settings";
    await run(root, out);
    const html = await readFile(join(out, "index.html"), "utf8");
    expect(html).not.toContain("data:image");
    expect(await listing(out)).toContainEqual(expect.stringMatching(/^assets\/dot-.*\.svg$/));
  });

  it("fails the build for a page with an inline script, naming the file and the fix", async () => {
    const root = await fixture({
      "NewTab/index.html": HTML('<script type="module">console.log(1)</script>'),
    });
    const failure = run(root, join(root, "out"));
    await expect(failure).rejects.toThrow(/NewTab\/index\.html:1: An inline <script>/);
    await expect(failure).rejects.toThrow(/Move the code to a file/);
  });

  it("fails the build for a remote script", async () => {
    const root = await fixture({
      "NewTab/index.html": HTML('<script src="https://cdn.example/x.js"></script>'),
    });
    await expect(run(root, join(root, "out"))).rejects.toThrow(/another origin/);
  });

  it("fails clearly when BDK_PAGE names no page", async () => {
    const root = await fixture();
    process.env.BDK_PAGE = "Nope";
    await expect(run(root, join(root, "out"))).rejects.toThrow(/BDK_PAGE="Nope" names no page/);
  });
});
