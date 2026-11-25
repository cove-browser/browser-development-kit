import type { Diagnostic } from "../diagnostics.js";
import type { KeyLocator, KeyPath } from "./locator.js";

export type Json = null | boolean | number | string | Json[] | { [key: string]: Json };

export function isRecord(value: unknown): value is Record<string, unknown> {
  return (
    typeof value === "object" && value !== null && !Array.isArray(value) && !(value instanceof Date)
  );
}

/** The TOML-facing name of a value's type, for messages. */
export function typeName(value: unknown): string {
  if (value === null) return "null";
  if (Array.isArray(value)) return "array";
  if (value instanceof Date) return "date";
  if (typeof value === "number") return Number.isInteger(value) ? "integer" : "float";
  if (typeof value === "bigint") return "integer";
  if (typeof value === "object") return "table";
  return typeof value;
}

/** A key path as written in documentation: `pages.overrides.history`. */
export function dotted(path: KeyPath): string {
  return path.map(String).join(".");
}

/** Collects diagnostics for one file, attaching the line of the key a finding is about. */
export class Reporter {
  readonly diagnostics: Diagnostic[] = [];

  constructor(
    readonly file: string,
    private readonly locator: KeyLocator | undefined,
  ) {}

  error(code: string, message: string, path: KeyPath, fix?: string): void {
    this.add("error", code, message, path, fix);
  }

  warning(code: string, message: string, path: KeyPath, fix?: string): void {
    this.add("warning", code, message, path, fix);
  }

  /** Information that needs no action, about a key. */
  note(code: string, message: string, path: KeyPath, fix?: string): void {
    this.add("note", code, message, path, fix);
  }

  /** A finding about a file or directory in the project rather than a key. */
  errorAt(code: string, message: string, file: string, fix?: string): void {
    this.diagnostics.push({ code, severity: "error", message, file, ...(fix ? { fix } : {}) });
  }

  get hasErrors(): boolean {
    return this.diagnostics.some((d) => d.severity === "error");
  }

  private add(
    severity: Diagnostic["severity"],
    code: string,
    message: string,
    path: KeyPath,
    fix: string | undefined,
  ): void {
    const position = this.locator?.findNearest(path);
    this.diagnostics.push({
      code,
      severity,
      message,
      file: this.file,
      ...(position ? { line: position.line, column: position.column } : {}),
      ...(fix ? { fix } : {}),
    });
  }
}

/** The stable order of diagnostics: file, line, column, code, message. */
export function sortDiagnostics(diagnostics: Diagnostic[]): Diagnostic[] {
  return [...diagnostics].sort(
    (a, b) =>
      (a.file ?? "").localeCompare(b.file ?? "", "en") ||
      (a.line ?? 0) - (b.line ?? 0) ||
      (a.column ?? 0) - (b.column ?? 0) ||
      a.code.localeCompare(b.code, "en") ||
      a.message.localeCompare(b.message, "en"),
  );
}
