import { writeFile } from "node:fs/promises";
import { join } from "node:path";
import { describe, expect, it } from "vitest";
import { tempDir, tempProject } from "../test-support.js";
import { CliError, ExitCode } from "./exit-codes.js";
import { findProject } from "./project.js";

async function failure(promise: Promise<unknown>): Promise<CliError> {
  try {
    await promise;
  } catch (error) {
    if (error instanceof CliError) return error;
    throw error;
  }
  throw new Error("expected a CliError");
}

describe("findProject", () => {
  it("finds bdk.toml in the starting directory", async () => {
    const { dir } = await tempProject();
    expect(await findProject(dir)).toEqual({ dir, configFile: join(dir, "bdk.toml") });
  });

  it("walks up to the nearest ancestor", async () => {
    const { dir, start } = await tempProject("App/Pages/home");
    expect((await findProject(start)).dir).toBe(dir);
  });

  it("prefers the nearest of two nested projects", async () => {
    const { dir, start } = await tempProject("inner");
    expect((await findProject(start)).dir).toBe(dir);
    await writeFile(join(start, "bdk.toml"), "");
    expect((await findProject(start)).dir).toBe(start);
  });

  it("uses --project instead of searching", async () => {
    const { dir } = await tempProject();
    const elsewhere = await tempDir();
    expect((await findProject(elsewhere, dir)).dir).toBe(dir);
  });

  it("resolves a relative --project against the working directory", async () => {
    const { dir } = await tempProject("sub");
    expect((await findProject(join(dir, "sub"), "..")).dir).toBe(dir);
  });

  it("fails with config.missing (exit 1) when there is no project", async () => {
    const error = await failure(findProject(await tempDir("bdk-none-")));
    expect(error.exitCode).toBe(ExitCode.ProjectFailure);
    expect(error.diagnostic.code).toBe("config.missing");
    expect(error.diagnostic.fix).toContain("--project");
  });

  it("fails with config.missing when --project has no bdk.toml", async () => {
    const empty = await tempDir();
    const error = await failure(findProject(process.cwd(), empty));
    expect(error.diagnostic.code).toBe("config.missing");
    expect(error.diagnostic.file).toBe(join(empty, "bdk.toml"));
  });
});
