import { readFile, stat } from "node:fs/promises";
import { join } from "node:path";
import { describe, expect, it } from "vitest";
import { AssetNotFoundError } from "../assets.js";
import type { Platform } from "../platform/platform.js";
import { memoryAssets, tempDir, tempProject, testContext } from "../test-support.js";
import type { Diagnostic } from "./diagnostics.js";
import { ExitCode } from "./exit-codes.js";
import { run } from "./run.js";

const error: Diagnostic = {
  code: "config.type",
  severity: "error",
  message: "bad value",
  file: "bdk.toml",
  line: 3,
  fix: "Quote it.",
};

describe("global behaviour", () => {
  it("prints the version and exits 0", async () => {
    const ctx = testContext();
    expect(await run(["--version"], ctx)).toBe(ExitCode.Success);
    expect(ctx.out.join("")).toBe("1.2.3\n");
  });

  it("prints the version as JSON with --json", async () => {
    const ctx = testContext();
    await run(["--version", "--json"], ctx);
    expect(JSON.parse(ctx.out.join(""))).toEqual({ version: "1.2.3" });
  });

  it("prints help to standard output and exits 0", async () => {
    const ctx = testContext();
    expect(await run(["--help"], ctx)).toBe(0);
    expect(ctx.out.join("")).toContain("Usage:");
    expect(ctx.out.join("")).toContain("Steps: config");
    expect(ctx.err.join("")).toBe("");
  });

  it("accepts --offline", async () => {
    const { start } = await tempProject();
    const ctx = testContext({ cwd: start });
    expect(await run(["build", "config", "--offline"], ctx)).toBe(0);
  });
});

describe("usage errors (exit 2)", () => {
  it.each([
    [[], "No command given"],
    [["frobnicate"], 'Unknown command "frobnicate". Commands: build'],
    [["build"], "needs a step. Steps: config"],
    [["build", "pages"], 'Unknown build step "pages". Steps: config'],
    [["build", "config", "pages"], 'Unknown build step "pages"'],
    [["--bogus"], "Unknown option"],
  ])("%j", async (argv, text) => {
    const ctx = testContext();
    expect(await run(argv, ctx)).toBe(ExitCode.Usage);
    expect(ctx.err.join("")).toContain(text);
  });

  it("emits a JSON document on standard output in --json mode", async () => {
    const ctx = testContext();
    expect(await run(["--json", "--bogus"], ctx)).toBe(2);
    const doc = JSON.parse(ctx.out.join(""));
    expect(doc).toMatchObject({ ok: false, exitCode: 2 });
    expect(doc.diagnostics[0].code).toBe("cli.usage");
  });
});

describe("bdk build config", () => {
  it("writes bdk.resolved.json next to bdk.toml and exits 0", async () => {
    const { dir, start } = await tempProject("deep");
    const ctx = testContext({ cwd: start });
    expect(await run(["build", "config"], ctx)).toBe(0);
    expect(JSON.parse(await readFile(join(dir, "bdk.resolved.json"), "utf8"))).toEqual({
      ok: true,
    });
  });

  it("writes to --out relative to the working directory", async () => {
    const { dir } = await tempProject();
    const out = await tempDir();
    const ctx = testContext({ cwd: out });
    expect(await run(["build", "config", "--project", dir, "--out", "r/x.json"], ctx)).toBe(0);
    expect(JSON.parse(await readFile(join(out, "r", "x.json"), "utf8"))).toEqual({ ok: true });
  });

  it("passes the schema, the project directory and the platform's bundle facts to the resolver", async () => {
    const { dir } = await tempProject();
    let seen: unknown;
    const ctx = testContext({
      cwd: dir,
      platform: {
        name: "fake",
        isIdeBuild: () => false,
        bundleFacts: () => Promise.resolve({ infoPlist: { CFBundleName: "App" } }),
      },
      resolveConfig: (options) => {
        seen = options;
        return Promise.resolve({ diagnostics: [], resolved: {} });
      },
    });
    await run(["build", "config"], ctx);
    expect(seen).toEqual({
      projectDir: dir,
      schema: { type: "object" },
      bundleFacts: { infoPlist: { CFBundleName: "App" } },
    });
  });

  it("exits 1, prints diagnostics and writes nothing when there are errors", async () => {
    const { dir } = await tempProject();
    const ctx = testContext({
      cwd: dir,
      resolveConfig: () => Promise.resolve({ diagnostics: [error], resolved: { ignored: true } }),
    });
    expect(await run(["build", "config"], ctx)).toBe(ExitCode.ProjectFailure);
    expect(ctx.err.join("")).toBe("bdk.toml:3: error: bad value [config.type]\n  fix: Quote it.\n");
    await expect(stat(join(dir, "bdk.resolved.json"))).rejects.toThrow();
  });

  it("writes the file and exits 0 when there are only warnings", async () => {
    const { dir } = await tempProject();
    const warning: Diagnostic = { ...error, severity: "warning" };
    const ctx = testContext({
      cwd: dir,
      resolveConfig: () => Promise.resolve({ diagnostics: [warning], resolved: { a: 1 } }),
    });
    expect(await run(["build", "config"], ctx)).toBe(0);
    expect(ctx.err.join("")).toContain("warning: bad value");
    expect(JSON.parse(await readFile(join(dir, "bdk.resolved.json"), "utf8"))).toEqual({ a: 1 });
  });

  it("exits 1 with config.missing outside a project", async () => {
    const ctx = testContext({ cwd: await tempDir("bdk-none-") });
    expect(await run(["build", "config"], ctx)).toBe(1);
    expect(ctx.err.join("")).toContain("[config.missing]");
  });

  it("exits 3 when the schema asset is missing", async () => {
    const { dir } = await tempProject();
    const ctx = testContext({
      cwd: dir,
      assets: {
        readText(name) {
          throw new AssetNotFoundError(name);
        },
      },
    });
    expect(await run(["build", "config"], ctx)).toBe(ExitCode.Environment);
    expect(ctx.err.join("")).toContain("[cli.asset_missing]");
  });

  it("exits 3 with cli.internal when the resolver throws", async () => {
    const { dir } = await tempProject();
    const ctx = testContext({
      cwd: dir,
      resolveConfig: () => Promise.reject(new Error("boom")),
    });
    expect(await run(["build", "config"], ctx)).toBe(3);
    expect(ctx.err.join("")).toContain("boom");
  });

  it("exits 3 when the resolver returns neither errors nor a result", async () => {
    const { dir } = await tempProject();
    const ctx = testContext({
      cwd: dir,
      resolveConfig: () => Promise.resolve({ diagnostics: [] }),
    });
    expect(await run(["build", "config"], ctx)).toBe(3);
  });

  it("reads the schema through the assets adapter", async () => {
    const { dir } = await tempProject();
    const ctx = testContext({
      cwd: dir,
      assets: memoryAssets({ "bdk.schema.json": "not json" }),
    });
    expect(await run(["build", "config"], ctx)).toBe(3);
  });

  it("prints one JSON document on stdout with --json, and diagnostics on stderr", async () => {
    const { dir } = await tempProject();
    const ctx = testContext({
      cwd: dir,
      resolveConfig: () => Promise.resolve({ diagnostics: [error] }),
    });
    expect(await run(["build", "config", "--json"], ctx)).toBe(1);
    expect(ctx.out).toHaveLength(1);
    const doc = JSON.parse(ctx.out[0] ?? "");
    expect(doc).toEqual({
      ok: false,
      exitCode: 1,
      command: "build config",
      diagnostics: [error],
      result: { config: { wrote: false } },
    });
    expect(ctx.err.join("")).toContain("error: bad value");
  });

  it("includes the output path in the JSON document on success", async () => {
    const { dir } = await tempProject();
    const ctx = testContext({ cwd: dir });
    await run(["build", "config", "--json"], ctx);
    const doc = JSON.parse(ctx.out[0] ?? "");
    expect(doc).toMatchObject({ ok: true, result: { config: { wrote: true } } });
    expect(doc.result.config.out).toBe(join(dir, "bdk.resolved.json"));
  });

  it("rejects --out on a build that has no step using it", async () => {
    const ctx = testContext();
    expect(await run(["build", "--out", "x"], ctx)).toBe(2);
  });
});

const ideAwarePlatform: Platform = {
  name: "fake-ide",
  isIdeBuild: (env) => env.XCODE_VERSION_ACTUAL !== undefined,
  bundleFacts: () => Promise.resolve(undefined),
};

describe("diagnostic form selection", () => {
  async function failing(env: NodeJS.ProcessEnv, extra: string[] = []) {
    const { dir } = await tempProject();
    const ctx = testContext({
      cwd: dir,
      env,
      platform: ideAwarePlatform,
      resolveConfig: () => Promise.resolve({ diagnostics: [error] }),
      stderrIsTTY: true,
    });
    await run(["build", "config", ...extra], ctx);
    return ctx.err.join("");
  }

  it("uses the Xcode form inside an Xcode build", async () => {
    const text = await failing({ XCODE_VERSION_ACTUAL: "2600" });
    expect(text).toBe("bdk.toml:3: error: [config.type] bad value\nbdk.toml:3: note: Quote it.\n");
  });

  it("uses the human form, with colour on a terminal, otherwise", async () => {
    expect(await failing({})).toContain("\u001b[31merror\u001b[0m");
  });

  it("drops colour when NO_COLOR is set", async () => {
    expect(await failing({ NO_COLOR: "1" })).not.toContain("\u001b");
  });

  it("forces a form with --diagnostics", async () => {
    expect(await failing({}, ["--diagnostics", "xcode"])).toContain("error: [config.type]");
    expect(
      await failing({ XCODE_VERSION_ACTUAL: "2600", NO_COLOR: "1" }, ["--diagnostics", "human"]),
    ).toContain("[config.type]\n  fix:");
  });
});
