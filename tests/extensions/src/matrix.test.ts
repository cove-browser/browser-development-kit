import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import { describe, expect, it } from "vitest";
import { check, generatedFiles, MATRIX_JSON } from "./cli.js";
import { expectedTests, validateMatrix } from "./matrix.js";
import { EVIDENCE_PATTERN, loadMatrix, type Matrix, type MatrixRow } from "./model.js";

const root = resolve(import.meta.dirname, "..", "..", "..");
const matrix = loadMatrix(resolve(root, MATRIX_JSON));

const row = (over: Partial<MatrixRow> = {}): MatrixRow => ({
  id: "api.x",
  group: "api",
  subject: "x",
  expected: "supported",
  reason: "because",
  evidence: ["engine_ext_probe_api_presence"],
  source: "extensions.md 7",
  ...over,
});
const of = (...rows: MatrixRow[]): Matrix => ({ schema: 1, rows });

/** The APIs named in the first column of the table in extensions.md section 7. */
function section7Apis(): string[] {
  const doc = readFileSync(resolve(root, "docs/planning/sdk/extensions.md"), "utf8");
  const section = /^## 7\. .*$([\s\S]*?)^## 8\./m.exec(doc)?.[1] ?? "";
  const apis: string[] = [];
  for (const line of section.split("\n")) {
    if (!line.startsWith("| `")) continue;
    const firstCell = line.split("|")[1] ?? "";
    for (const token of firstCell.matchAll(/`([^`]+)`/g)) {
      apis.push((token[1] ?? "").replace(/\(.*\)$/, ""));
    }
  }
  return apis;
}

describe("the committed matrix", () => {
  it("is valid", () => {
    expect(validateMatrix(matrix)).toEqual([]);
  });

  it("has a row for every API in extensions.md section 7", () => {
    const apis = section7Apis();
    expect(apis.length).toBeGreaterThan(40);
    const subjects = new Set(matrix.rows.filter((r) => r.group === "api").map((r) => r.subject));
    expect(apis.filter((api) => !subjects.has(api))).toEqual([]);
  });

  it("has a row for each of the ten T-EXT-01 assertions, and for both incognito modes", () => {
    const ids = new Set(matrix.rows.map((r) => r.id));
    for (let n = 1; n <= 10; n++) expect(ids).toContain(`assertion.${String(n).padStart(2, "0")}`);
    expect(ids).toContain("incognito.spanning");
    expect(ids).toContain("incognito.split");
  });

  it("names each class the M5 exit criteria name", () => {
    const ids = matrix.rows.map((r) => r.id);
    for (const id of ["class.blocker", "class.passwords", "class.devtool"])
      expect(ids).toContain(id);
  });

  it("is current: the committed generated files match their inputs", () => {
    expect(check(root)).toBe(0);
  });

  it("lists its expected engine tests in sorted order", () => {
    const names = expectedTests(matrix);
    expect(names).toEqual([...names].sort());
    expect(names.every((name) => EVIDENCE_PATTERN.test(name))).toBe(true);
  });
});

describe("validateMatrix", () => {
  it("accepts a good row", () => {
    expect(validateMatrix(of(row()))).toEqual([]);
  });

  it("rejects a row with no reason", () => {
    expect(validateMatrix(of(row({ reason: " " })))).toEqual(["row api.x: no reason"]);
  });

  it("rejects a row with no evidence", () => {
    expect(validateMatrix(of(row({ evidence: [] })))).toEqual(["row api.x: no evidence test name"]);
  });

  it("rejects evidence that breaks the naming convention", () => {
    for (const bad of [
      "engine_ext_other_thing",
      "engine_ext_probe_",
      "ext_probe_thing",
      "engine_ext_probe_Thing",
    ]) {
      expect(validateMatrix(of(row({ evidence: [bad] })))).toHaveLength(1);
    }
  });

  it("rejects a status outside the enum", () => {
    expect(validateMatrix(of(row({ expected: "works" as never })))).toHaveLength(1);
  });

  it("rejects duplicate ids and an id outside its group", () => {
    expect(validateMatrix(of(row(), row()))).toEqual(["row api.x: duplicate id"]);
    expect(validateMatrix(of(row({ id: "surface.x" })))).toHaveLength(1);
  });
});

describe("generatedFiles", () => {
  it("writes the matrix and the expected test list", () => {
    const files = generatedFiles(root, null);
    expect([...files.keys()]).toEqual([
      "docs/extensions/compatibility-matrix.md",
      "tests/extensions/expected-engine-tests.txt",
    ]);
  });
});
