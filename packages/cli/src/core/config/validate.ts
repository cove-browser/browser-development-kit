import { parse, TomlError } from "smol-toml";
import type { Diagnostic } from "../diagnostics.js";
import { checkDocument } from "./checks.js";
import { KeyLocator } from "./locator.js";
import { Reporter, sortDiagnostics } from "./reporter.js";
import { checkSchema } from "./schema-check.js";

export const CONFIG_FILE = "bdk.toml";

/** A document that passed every check that needs no file system. */
export interface ParsedConfig {
  doc: Record<string, unknown>;
  locator: KeyLocator;
}

export interface ParseOutcome {
  diagnostics: Diagnostic[];
  /** Present when there is no error diagnostic. */
  config?: ParsedConfig;
}

/**
 * Parses and checks the text of a bdk.toml: TOML syntax, the schema, and the semantic checks that
 * need no files. Diagnostics are sorted and carry the line of the offending key.
 */
export function parseConfig(text: string, schema: object, file = CONFIG_FILE): ParseOutcome {
  let doc: Record<string, unknown>;
  try {
    doc = parse(text);
  } catch (error) {
    if (!(error instanceof TomlError)) throw error;
    const detail = error.message.split("\n")[0]?.replace(/^Invalid TOML document: /, "") ?? "";
    return {
      diagnostics: [
        {
          code: "config.syntax",
          severity: "error",
          message: `${file} is not valid TOML.`,
          file,
          line: error.line,
          column: error.column,
          fix: `Fix the syntax: ${detail}.`,
        },
      ],
    };
  }
  const locator = new KeyLocator(text);
  const reporter = new Reporter(file, locator);
  checkSchema(schema, doc, reporter);
  checkDocument(doc, reporter);
  const diagnostics = sortDiagnostics(reporter.diagnostics);
  return reporter.hasErrors ? { diagnostics } : { diagnostics, config: { doc, locator } };
}

/** Validates the text of a bdk.toml without touching the file system. */
export function validateConfigText(text: string, schema: object, file = CONFIG_FILE): Diagnostic[] {
  return parseConfig(text, schema, file).diagnostics;
}
