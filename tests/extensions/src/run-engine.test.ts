import { existsSync, mkdirSync, mkdtempSync, readFileSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { describe, expect, it } from "vitest";
import { MATRIX_JSON } from "./cli.js";
import { RESULTS_FILE, testExtensions } from "./run-engine.js";

const realRoot = resolve(import.meta.dirname, "..", "..", "..");

/** A workspace holding only the matrix, so a run cannot touch the real generated files. */
function workspace(): string {
  const root = mkdtempSync(join(tmpdir(), "bdk-ext-"));
  mkdirSync(join(root, "tests", "extensions"), { recursive: true });
  writeFileSync(join(root, MATRIX_JSON), readFileSync(join(realRoot, MATRIX_JSON)));
  mkdirSync(join(root, "docs", "extensions"), { recursive: true });
  return root;
}

const SKIPPED =
  "core:test-engine: no runtime at /x/Frameworks; run pnpm chromium:runtime (or set BDK_ENGINE_FRAMEWORK). Skipped.\n";

describe("testExtensions", () => {
  it("skips with a runtime-rebuild message when there is no runtime, and writes nothing", () => {
    const root = workspace();
    const lines: string[] = [];
    const status = testExtensions({
      root,
      platform: "darwin",
      runEngine: () => ({ status: 0, stdout: SKIPPED }),
      log: (line) => lines.push(line),
    });
    expect(status).toBe(0);
    expect(lines.join("\n")).toContain("runtime rebuild needed");
    expect(existsSync(join(root, RESULTS_FILE))).toBe(false);
    expect(existsSync(join(root, "docs/extensions/compatibility-matrix.md"))).toBe(false);
  });

  it("skips off macOS without running the engine", () => {
    let ran = false;
    const lines: string[] = [];
    const status = testExtensions({
      root: workspace(),
      platform: "linux",
      runEngine: () => {
        ran = true;
        return { status: 0, stdout: "" };
      },
      log: (line) => lines.push(line),
    });
    expect(status).toBe(0);
    expect(ran).toBe(false);
    expect(lines.join("\n")).toContain("runtime rebuild needed");
  });

  it("saves the output, regenerates the matrix with it and returns the engine's status", () => {
    const root = workspace();
    const stdout =
      "[ ok ] engine_ext_probe_worker_and_content_ack\n[FAIL] engine_ext_ui_popup_closes_on_blur\n";
    const status = testExtensions({
      root,
      platform: "darwin",
      runEngine: () => ({ status: 1, stdout }),
      log: () => {},
    });
    expect(status).toBe(1);
    expect(readFileSync(join(root, RESULTS_FILE), "utf8")).toBe(stdout);
    const md = readFileSync(join(root, "docs/extensions/compatibility-matrix.md"), "utf8");
    expect(md).toContain("**Status: run.**");
    expect(md).toContain("failed");
  });
});
