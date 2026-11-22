import { EVIDENCE_PATTERN, GROUPS, type Matrix, STATUSES } from "./model.js";

/** Problems with the matrix itself, one message each. Empty when it is valid. */
export function validateMatrix(matrix: Matrix): string[] {
  const issues: string[] = [];
  if (matrix.schema !== 1) issues.push(`schema must be 1, got ${String(matrix.schema)}`);
  const ids = new Set<string>();
  for (const row of matrix.rows) {
    const where = `row ${row.id}`;
    if (typeof row.id !== "string" || row.id === "") issues.push("a row has no id");
    if (ids.has(row.id)) issues.push(`${where}: duplicate id`);
    ids.add(row.id);
    if (!(GROUPS as readonly string[]).includes(row.group)) {
      issues.push(`${where}: group "${row.group}" is not one of ${GROUPS.join(", ")}`);
    } else if (!row.id.startsWith(`${row.group}.`)) {
      issues.push(`${where}: id must start with its group "${row.group}."`);
    }
    if (!row.subject?.trim()) issues.push(`${where}: no subject`);
    if (!(STATUSES as readonly string[]).includes(row.expected)) {
      issues.push(`${where}: expected "${row.expected}" is not one of ${STATUSES.join(", ")}`);
    }
    if (!row.reason?.trim()) issues.push(`${where}: no reason`);
    if (!row.source?.trim()) issues.push(`${where}: no source`);
    if (!Array.isArray(row.evidence) || row.evidence.length === 0) {
      issues.push(`${where}: no evidence test name`);
      continue;
    }
    for (const name of row.evidence) {
      if (!EVIDENCE_PATTERN.test(name)) {
        issues.push(`${where}: evidence "${name}" does not match engine_ext_<area>_<what>`);
      }
    }
    if (new Set(row.evidence).size !== row.evidence.length) {
      issues.push(`${where}: evidence lists a name twice`);
    }
  }
  return issues;
}

/** Every engine test name the matrix expects, sorted and without repeats. */
export function expectedTests(matrix: Matrix): string[] {
  return [...new Set(matrix.rows.flatMap((row) => row.evidence))].sort();
}
