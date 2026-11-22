import { describe, expect, it } from "vitest";
import type { Matrix, MatrixRow } from "./model.js";
import { coverage, observe, renderExpectedTests, renderMarkdown } from "./render.js";
import { parseResults } from "./results.js";

const matrix: Matrix = {
  schema: 1,
  rows: [
    {
      id: "api.a",
      group: "api",
      subject: "a",
      expected: "supported",
      reason: "same as Chrome",
      evidence: ["engine_ext_probe_one", "engine_ext_ui_two"],
      source: "s",
    },
    {
      id: "api.b",
      group: "api",
      subject: "b",
      expected: "degraded",
      reason: "differs | with a pipe",
      evidence: ["engine_ext_probe_one"],
      source: "s",
    },
    {
      id: "api.c",
      group: "api",
      subject: "c",
      expected: "absent",
      reason: "not there",
      evidence: ["engine_ext_panel_three"],
      source: "s",
    },
  ],
};

const [rowA, rowB, rowC] = matrix.rows as readonly [MatrixRow, MatrixRow, MatrixRow];

const engineOutput = `[ ok ] engine_ext_probe_one
[FAIL] engine_ext_ui_two
    FAIL tests_engine_ext_ui.c:10: x == 1
[skip] engine_ext_panel_three
[ ok ] engine_ext_probe_unlisted
[ ok ] engine_prompts_something
4 tests, 3 passed, 1 failed
`;

describe("parseResults", () => {
  it("reads pass, fail and skip lines and ignores everything else", () => {
    const { tests } = parseResults(engineOutput);
    expect(tests.get("engine_ext_probe_one")).toBe("pass");
    expect(tests.get("engine_ext_ui_two")).toBe("fail");
    expect(tests.get("engine_ext_panel_three")).toBe("skip");
    expect(tests.size).toBe(5);
  });

  it("keeps the worst outcome of a repeated name", () => {
    expect(parseResults("[ ok ] a\n[FAIL] a\n[ ok ] a\n").tests.get("a")).toBe("fail");
  });
});

describe("observe", () => {
  const results = parseResults(engineOutput);
  it("fails a row when any evidence failed", () => {
    expect(observe(rowA, results)).toBe("failed");
  });
  it("passes a row only when all its evidence passed", () => {
    expect(observe(rowB, results)).toBe("passed");
  });
  it("does not run a row whose evidence was skipped or missing", () => {
    expect(observe(rowC, results)).toBe("not run");
    expect(observe(rowB, parseResults(""))).toBe("not run");
  });
  it("is not run without results", () => {
    expect(observe(rowB, null)).toBe("not run");
  });
});

describe("renderMarkdown", () => {
  it("says not run at the top and in every row when there are no results", () => {
    const md = renderMarkdown(matrix, null);
    expect(md).toContain("**Status: not run.**");
    expect(md).not.toContain("passed |");
    expect(md).not.toContain("| failed");
    expect(md.match(/\| not run \|/g)?.length).toBeGreaterThanOrEqual(3);
  });

  it("lists rows that are not supported as known differences, with the reason, and escapes pipes", () => {
    const md = renderMarkdown(matrix, null);
    const known = md.slice(md.indexOf("## Known differences from Chrome"));
    expect(known).toContain("### Degraded");
    expect(known).toContain("differs \\| with a pipe");
    expect(known).toContain("### Absent");
    expect(known).not.toContain("same as Chrome");
    expect(known).not.toContain("### Gap");
  });

  it("reports the observed results and tests that prove no row", () => {
    const md = renderMarkdown(matrix, parseResults(engineOutput));
    expect(md).toContain("**Status: run.** 3 rows: 1 passed, 1 failed, 1 not run.");
    expect(md).toContain("- `engine_ext_probe_unlisted`");
    expect(md).not.toContain("engine_prompts_something");
  });

  it("is deterministic", () => {
    expect(renderMarkdown(matrix, null)).toBe(renderMarkdown(matrix, null));
  });
});

describe("expected tests and coverage", () => {
  it("lists each name once, sorted", () => {
    expect(renderExpectedTests(matrix)).toBe(
      "engine_ext_panel_three\nengine_ext_probe_one\nengine_ext_ui_two\n",
    );
  });

  it("compares the matrix with an engine --list output", () => {
    const list = "engine_ext_probe_one\nengine_ext_ui_two\nengine_ext_ui_extra\nengine_base_x\n";
    expect(coverage(matrix, list)).toEqual({
      missing: ["engine_ext_panel_three"],
      unlisted: ["engine_ext_ui_extra"],
    });
  });
});
