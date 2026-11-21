import { spawnSync } from "node:child_process";
import {
  existsSync,
  mkdirSync,
  readFileSync,
  realpathSync,
  symlinkSync,
  writeFileSync,
} from "node:fs";
import { delimiter, dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { describe, expect, it } from "vitest";
import { tempDir, tempProject } from "./test-support.js";

const cliRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const exe = join(cliRoot, "dist", "bdk");
const version: string = JSON.parse(readFileSync(join(cliRoot, "package.json"), "utf8")).version;

/** PATH with every directory that holds a `node` binary removed, as the CI smoke job does. */
function pathWithoutNode(): string {
  return (process.env.PATH ?? "")
    .split(delimiter)
    .filter((dir) => dir !== "" && !existsSync(join(dir, "node")))
    .join(delimiter);
}

function bdk(args: string[], cwd = cliRoot) {
  const result = spawnSync(exe, args, {
    cwd,
    encoding: "utf8",
    env: { PATH: pathWithoutNode(), HOME: process.env.HOME ?? "" },
  });
  return { status: result.status, stdout: result.stdout, stderr: result.stderr };
}

const packaged = existsSync(exe);
if (!packaged) {
  console.warn(
    `packaged executable tests SKIPPED: ${exe} does not exist. Run \`pnpm cli:package\` first.`,
  );
}

describe.skipIf(!packaged)("packaged executable (skipped when dist/bdk is absent)", () => {
  it("is not given a node on PATH", () => {
    const result = spawnSync("/bin/sh", ["-c", "command -v node"], {
      env: { PATH: pathWithoutNode() },
    });
    expect(result.status).not.toBe(0);
  });

  it("prints its embedded version", () => {
    expect(bdk(["--version"])).toMatchObject({ status: 0, stdout: `${version}\n` });
  });

  it("prints help", () => {
    const result = bdk(["--help"]);
    expect(result.status).toBe(0);
    expect(result.stdout).toContain("Steps: config, pages, sign");
  });

  it("exits 2 for an unknown command", () => {
    expect(bdk(["frobnicate"]).status).toBe(2);
  });

  it("resolves a project with the embedded schema (or reports it missing, exit 3)", async () => {
    const project = await tempProject();
    const result = bdk(["build", "config"], project.dir);
    if (existsSync(join(cliRoot, "assets", "bdk.schema.json"))) {
      expect(result).toMatchObject({ status: 0, stderr: "" });
      const resolved = JSON.parse(readFileSync(join(project.dir, "bdk.resolved.json"), "utf8"));
      expect(resolved).toMatchObject({ bdk_version: version, browser: { name: "Demo" } });
      expect(resolved.config_digest).toMatch(/^sha256:[0-9a-f]{64}$/);
    } else {
      expect(result.status).toBe(3);
      expect(result.stderr).toContain("[cli.asset_missing]");
    }
  });

  it("packages a plain page and the SDK with `build config pages`", async () => {
    const project = await tempProject();
    mkdirSync(join(project.dir, "Pages", "Settings"), { recursive: true });
    writeFileSync(
      join(project.dir, "Pages", "Settings", "index.html"),
      '<!doctype html><title>Settings</title><script type="module" src="./main.js"></script>',
    );
    writeFileSync(join(project.dir, "Pages", "Settings", "main.js"), "export {};\n");
    const app = join(await tempDir(), "Demo.app");
    const result = bdk(["build", "config", "pages", "--bundle", app], project.dir);
    if (!existsSync(join(cliRoot, "assets", "bdk.schema.json"))) {
      expect(result.status).toBe(3);
      return;
    }
    expect(result).toMatchObject({ status: 0, stderr: "" });
    const resources = join(app, "Contents", "Resources");
    expect(existsSync(join(resources, "Pages", "Settings", "index.html"))).toBe(true);
    expect(existsSync(join(resources, "BDK", "js", "internal", "index.js"))).toBe(true);
  });

  it("builds a Vite page with the project's own Vite and no node on PATH", async () => {
    const vite = join(cliRoot, "..", "js", "vite");
    const project = await tempProject();
    const pages = join(project.dir, "Pages");
    mkdirSync(join(pages, "Settings"), { recursive: true });
    mkdirSync(join(pages, "node_modules", "@bdk"), { recursive: true });
    symlinkSync(
      realpathSync(join(vite, "node_modules", "vite")),
      join(pages, "node_modules", "vite"),
    );
    symlinkSync(realpathSync(vite), join(pages, "node_modules", "@bdk", "vite"));
    writeFileSync(join(pages, "package.json"), '{"type":"module"}');
    writeFileSync(
      join(pages, "vite.config.ts"),
      'import { bdk } from "@bdk/vite";\nexport default { plugins: [bdk()] };\n',
    );
    writeFileSync(
      join(pages, "Settings", "index.html"),
      '<!doctype html><title>Settings</title><script type="module" src="./main.ts"></script>',
    );
    writeFileSync(join(pages, "Settings", "main.ts"), 'console.log("settings");\n');
    const app = join(await tempDir(), "Demo.app");
    const result = bdk(["build", "config", "pages", "--bundle", app], project.dir);
    if (!existsSync(join(cliRoot, "assets", "bdk.schema.json"))) {
      expect(result.status).toBe(3);
      return;
    }
    expect(result).toMatchObject({ status: 0, stderr: "" });
    const html = readFileSync(
      join(app, "Contents", "Resources", "Pages", "Settings", "index.html"),
      "utf8",
    );
    expect(html).toMatch(/src="\.\/assets\/Settings-[\w-]+\.js"/);
  });

  it("finds no project outside one, exit 1", async () => {
    const result = bdk(["build", "config"], await tempDir());
    expect(result.status).toBe(1);
    expect(result.stderr).toContain("[config.missing]");
  });
});
