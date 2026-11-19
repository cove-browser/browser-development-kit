import { spawnSync } from "node:child_process";
import { existsSync, readFileSync } from "node:fs";
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
    expect(result.stdout).toContain("Steps: config");
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

  it("finds no project outside one, exit 1", async () => {
    const result = bdk(["build", "config"], await tempDir());
    expect(result.status).toBe(1);
    expect(result.stderr).toContain("[config.missing]");
  });
});
