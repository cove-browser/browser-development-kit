import { writeFile } from "node:fs/promises";
import { join } from "node:path";
import { describe, expect, it } from "vitest";
import { tempDir } from "../../test-support.js";
import { loadVite } from "./vite.js";

describe("loadVite", () => {
  it("tells the developer to install when Vite is not in the pages directory", async () => {
    const dir = await tempDir();
    const failure = () =>
      loadVite(dir, `${dir}/vite.config.ts`, () => {
        throw new Error("Cannot find module");
      });
    expect(failure).toThrow(
      expect.objectContaining({
        exitCode: 1,
        diagnostic: expect.objectContaining({
          code: "pages.vite_missing",
          file: `${dir}/vite.config.ts`,
          fix: expect.stringContaining("install"),
        }),
      }),
    );
  });

  it("loads the build function of a Vite found in the project", async () => {
    const dir = await tempDir();
    await writeFile(join(dir, "fake-vite.cjs"), "exports.build = () => 1;");
    expect(loadVite(dir, `${dir}/vite.config.ts`, () => join(dir, "fake-vite.cjs"))).toBeTypeOf(
      "function",
    );
  });

  it("refuses a module that is not Vite", async () => {
    const dir = await tempDir();
    await writeFile(join(dir, "other.cjs"), "exports.x = 1;");
    const failure = () => loadVite(dir, `${dir}/vite.config.ts`, () => join(dir, "other.cjs"));
    expect(failure).toThrow(
      expect.objectContaining({
        diagnostic: expect.objectContaining({ code: "pages.vite_missing" }),
      }),
    );
  });
});
