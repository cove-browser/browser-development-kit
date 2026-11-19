import { describe, expect, it } from "vitest";
import type { Diagnostic } from "./diagnostics.js";
import { formatDiagnostic, formatDiagnostics, hasErrors } from "./diagnostics-format.js";

const full: Diagnostic = {
  code: "config.type",
  severity: "error",
  message: "browser.scheme must be a string",
  file: "/p/bdk.toml",
  line: 4,
  column: 3,
  fix: "Write the scheme as a quoted string.",
};

describe("formatDiagnostic, xcode form", () => {
  it("prints file:line:column: error: message, with the fix as a note", () => {
    expect(formatDiagnostic(full, { format: "xcode", color: false })).toBe(
      "/p/bdk.toml:4:3: error: [config.type] browser.scheme must be a string\n" +
        "/p/bdk.toml:4:3: note: Write the scheme as a quoted string.",
    );
  });

  it("omits the column and the fix when absent", () => {
    const d: Diagnostic = { code: "x.y", severity: "warning", message: "m", file: "f", line: 2 };
    expect(formatDiagnostic(d, { format: "xcode", color: false })).toBe("f:2: warning: [x.y] m");
  });

  it("prints no location for a diagnostic without a file", () => {
    const d: Diagnostic = { code: "cli.usage", severity: "error", message: "bad" };
    expect(formatDiagnostic(d, { format: "xcode", color: true })).toBe("error: [cli.usage] bad");
  });

  it("never uses colour", () => {
    expect(formatDiagnostic(full, { format: "xcode", color: true })).not.toContain("\u001b");
  });
});

describe("formatDiagnostic, human form", () => {
  it("prints the location, message, code and fix without colour", () => {
    expect(formatDiagnostic(full, { format: "human", color: false })).toBe(
      "/p/bdk.toml:4:3: error: browser.scheme must be a string [config.type]\n" +
        "  fix: Write the scheme as a quoted string.",
    );
  });

  it("colours the severity only when asked", () => {
    const coloured = formatDiagnostic(full, { format: "human", color: true });
    expect(coloured).toContain("\u001b[31merror\u001b[0m");
    const warning = formatDiagnostic(
      { ...full, severity: "warning" },
      { format: "human", color: true },
    );
    expect(warning).toContain("\u001b[33mwarning\u001b[0m");
  });
});

describe("formatDiagnostics and hasErrors", () => {
  it("ends every diagnostic with a newline", () => {
    const text = formatDiagnostics([full, { ...full, line: 9 }], { format: "xcode", color: false });
    expect(text.endsWith("\n")).toBe(true);
    expect(text.split("\n").filter((l) => l.includes("error:"))).toHaveLength(2);
  });

  it("reports whether any diagnostic is an error", () => {
    expect(hasErrors([])).toBe(false);
    expect(hasErrors([{ ...full, severity: "warning" }])).toBe(false);
    expect(hasErrors([{ ...full, severity: "warning" }, full])).toBe(true);
  });
});
