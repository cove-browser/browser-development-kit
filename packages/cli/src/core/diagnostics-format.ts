import type { Diagnostic } from "./diagnostics.js";

/** `human` for a terminal or a log; `xcode` for the `file:line: error: message` form Xcode shows inline. */
export type DiagnosticFormat = "human" | "xcode";

export interface FormatOptions {
  format: DiagnosticFormat;
  /** ANSI colour. Only used by the human form. */
  color: boolean;
}

const RED = "\u001b[31m";
const YELLOW = "\u001b[33m";
const DIM = "\u001b[2m";
const RESET = "\u001b[0m";

function location(d: Diagnostic): string {
  if (d.file === undefined) return "";
  const parts = [d.file];
  if (d.line !== undefined) {
    parts.push(String(d.line));
    if (d.column !== undefined) parts.push(String(d.column));
  }
  return `${parts.join(":")}: `;
}

/**
 * Formats one diagnostic as one or more lines, without a trailing newline.
 *
 * Xcode form: `file:line:column: error: [code] message`, then the fix as a `note:` at the same place.
 * Human form: the same location, the severity coloured on a terminal, and the fix on an indented line.
 */
export function formatDiagnostic(d: Diagnostic, options: FormatOptions): string {
  const where = location(d);
  if (options.format === "xcode") {
    const lines = [`${where}${d.severity}: [${d.code}] ${d.message}`];
    if (d.fix !== undefined) lines.push(`${where}note: ${d.fix}`);
    return lines.join("\n");
  }
  const paint = (colour: string, text: string): string =>
    options.color ? `${colour}${text}${RESET}` : text;
  const colour = d.severity === "error" ? RED : d.severity === "warning" ? YELLOW : DIM;
  const severity = paint(colour, d.severity);
  const lines = [`${where}${severity}: ${d.message} ${paint(DIM, `[${d.code}]`)}`];
  if (d.fix !== undefined) lines.push(`  fix: ${d.fix}`);
  return lines.join("\n");
}

export function formatDiagnostics(list: readonly Diagnostic[], options: FormatOptions): string {
  return list.map((d) => `${formatDiagnostic(d, options)}\n`).join("");
}

export function hasErrors(list: readonly Diagnostic[]): boolean {
  return list.some((d) => d.severity === "error");
}
