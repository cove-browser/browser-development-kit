import type { Diagnostic } from "../diagnostics.js";

/** The four groups of sdk/cli.md section 6, in the order they are reported. */
export const DOCTOR_GROUPS = ["machine", "project", "compatibility", "bundle"] as const;
export type DoctorGroup = (typeof DOCTOR_GROUPS)[number];

/** A diagnostic and the group it is reported in. */
export interface Finding {
  group: DoctorGroup;
  diagnostic: Diagnostic;
}

/** Collects findings for one group. */
export class Findings {
  readonly list: Finding[] = [];

  constructor(private readonly group: DoctorGroup) {}

  add(diagnostic: Diagnostic): void {
    this.list.push({ group: this.group, diagnostic });
  }

  error(code: string, message: string, fix: string, file?: string): void {
    this.add({ code, severity: "error", message, fix, ...(file === undefined ? {} : { file }) });
  }

  warning(code: string, message: string, fix: string, file?: string): void {
    this.add({ code, severity: "warning", message, fix, ...(file === undefined ? {} : { file }) });
  }

  note(code: string, message: string, file?: string): void {
    this.add({ code, severity: "note", message, ...(file === undefined ? {} : { file }) });
  }
}
