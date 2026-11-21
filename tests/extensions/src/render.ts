import { expectedTests } from "./matrix.js";
import type { Matrix, MatrixRow, Status } from "./model.js";
import type { Results } from "./results.js";

export type Observed = "passed" | "failed" | "not run";

/**
 * A row failed when any of its evidence failed. It passed when all of it passed. Anything else, including a
 * test that was skipped or never printed, is not run.
 */
export function observe(row: MatrixRow, results: Results | null): Observed {
  if (results === null) return "not run";
  const outcomes = row.evidence.map((name) => results.tests.get(name));
  if (outcomes.includes("fail")) return "failed";
  if (outcomes.every((outcome) => outcome === "pass")) return "passed";
  return "not run";
}

const GROUP_TITLES: Record<string, string> = {
  assertion: "T-EXT-01 assertions",
  api: "Extension APIs that need the host",
  surface: "UI surfaces",
  install: "Installation sources and policy",
  incognito: "Private profiles",
  layout: "Host mapping",
  class: "Representative extensions",
};

const DIFFERENCE_TITLES: Record<Exclude<Status, "supported">, string> = {
  degraded: "Degraded: present, but behaves differently from Chrome",
  gap: "Gap: Chrome has it, BDK does not yet, or it is unverified",
  absent: "Absent: not in the extension's chrome object by design",
};

const cell = (text: string): string => text.replaceAll("|", "\\|").replaceAll("\n", " ");
/** An API name is code; the other rows are sentences. */
const subject = (row: MatrixRow): string =>
  row.group === "api" ? `\`${cell(row.subject)}\`` : cell(row.subject);
const code = (names: readonly string[]): string => names.map((name) => `\`${name}\``).join("<br>");

function table(header: readonly string[], lines: readonly (readonly string[])[]): string[] {
  return [
    `| ${header.join(" | ")} |`,
    `|${header.map(() => "---").join("|")}|`,
    ...lines.map((line) => `| ${line.join(" | ")} |`),
  ];
}

/**
 * The compatibility matrix as Markdown. With `results` null the state is "not run" everywhere and says so. The
 * output depends only on its inputs, so a stale committed copy is detectable by regenerating it.
 */
export function renderMarkdown(matrix: Matrix, results: Results | null): string {
  const out: string[] = [
    "# Extension compatibility matrix",
    "",
    "Generated from `tests/extensions/matrix.json` by `pnpm exec nx run extensions-suite:matrix`. Do not edit by hand.",
    "",
  ];

  const observed = matrix.rows.map((row) => observe(row, results));
  if (results === null) {
    out.push(
      '**Status: not run.** No engine results were supplied, so every observed result below is "not run". ' +
        "The expected statuses are the plan from `docs/planning/sdk/extensions.md`, not test results. " +
        "`pnpm test:release` runs the engine's extension tests and regenerates this file with the results.",
    );
  } else {
    const count = (value: Observed): number => observed.filter((o) => o === value).length;
    out.push(
      `**Status: run.** ${matrix.rows.length} rows: ${count("passed")} passed, ${count("failed")} failed, ` +
        `${count("not run")} not run.`,
    );
  }
  out.push(
    "",
    "Expected status: `supported` behaves as in Chrome. `degraded` exists and differs. `absent` is missing from the " +
      "extension's `chrome` object. `gap` is a known missing or unverified capability. Observed: `passed` when every " +
      "engine test listed for the row passed, `failed` when any failed, `not run` otherwise.",
    "",
  );

  const groups = [...new Set(matrix.rows.map((row) => row.group))];
  for (const group of groups) {
    out.push(`## ${GROUP_TITLES[group] ?? group}`, "");
    const lines: string[][] = [];
    matrix.rows.forEach((row, index) => {
      if (row.group !== group) return;
      lines.push([subject(row), row.expected, observed[index] ?? "not run", code(row.evidence)]);
    });
    out.push(...table(["Row", "Expected", "Observed", "Evidence"], lines), "");
  }

  out.push(
    "## Known differences from Chrome",
    "",
    "Every row whose expected status is not `supported`.",
    "",
  );
  for (const status of ["degraded", "gap", "absent"] as const) {
    const lines: string[][] = [];
    matrix.rows.forEach((row, index) => {
      if (row.expected !== status) return;
      lines.push([subject(row), cell(row.reason), observed[index] ?? "not run"]);
    });
    if (lines.length === 0) continue;
    out.push(
      `### ${DIFFERENCE_TITLES[status]}`,
      "",
      ...table(["Row", "What differs", "Observed"], lines),
      "",
    );
  }

  if (results !== null) {
    const known = new Set(expectedTests(matrix));
    const extra = [...results.tests.keys()].filter(
      (name) => name.startsWith("engine_ext_") && !known.has(name),
    );
    if (extra.length > 0) {
      out.push(
        "## Engine tests not in the matrix",
        "",
        "These ran but prove no row. Add a row to `tests/extensions/matrix.json` or rename the test.",
        "",
        ...extra.sort().map((name) => `- \`${name}\``),
        "",
      );
    }
  }
  return `${out.join("\n").trimEnd()}\n`;
}

/** One name per line, for the coordinator to compare with the engine's `--list` output. */
export function renderExpectedTests(matrix: Matrix): string {
  return `${expectedTests(matrix).join("\n")}\n`;
}

/** Names the matrix expects that an engine test list does not have, and `engine_ext_*` names it has that no row uses. */
export function coverage(
  matrix: Matrix,
  engineList: string,
): { missing: string[]; unlisted: string[] } {
  const present = new Set(
    engineList
      .split(/\r?\n/)
      .map((line) => line.trim())
      .filter(Boolean),
  );
  const expected = new Set(expectedTests(matrix));
  return {
    missing: [...expected].filter((name) => !present.has(name)).sort(),
    unlisted: [...present]
      .filter((name) => name.startsWith("engine_ext_") && !expected.has(name))
      .sort(),
  };
}
