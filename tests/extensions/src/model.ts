import { readFileSync } from "node:fs";

/** How a row is expected to behave compared with Chrome. See tests/extensions/README.md. */
export const STATUSES = ["supported", "degraded", "absent", "gap"] as const;
export type Status = (typeof STATUSES)[number];

export const GROUPS = [
  "assertion",
  "api",
  "surface",
  "install",
  "incognito",
  "layout",
  "class",
] as const;
export type Group = (typeof GROUPS)[number];

export const AREAS = ["install", "ui", "panel", "probe"] as const;

/** Engine test names that prove a row: `engine_ext_<area>_<what>`. */
export const EVIDENCE_PATTERN = new RegExp(
  `^engine_ext_(${AREAS.join("|")})_[a-z0-9]+(_[a-z0-9]+)*$`,
);

export interface MatrixRow {
  readonly id: string;
  readonly group: Group;
  readonly subject: string;
  readonly expected: Status;
  readonly reason: string;
  readonly evidence: readonly string[];
  readonly source: string;
}

export interface Matrix {
  readonly schema: 1;
  readonly rows: readonly MatrixRow[];
}

export function loadMatrix(path: string): Matrix {
  return JSON.parse(readFileSync(path, "utf8")) as Matrix;
}
