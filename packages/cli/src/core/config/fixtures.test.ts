import { existsSync, readdirSync, readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { describe, expect, it } from "vitest";
import type { Diagnostic } from "../diagnostics.js";
import { resolveConfig, serialiseResolved, validateConfigText } from "./index.js";

const repoRoot = fileURLToPath(new URL("../../../../../", import.meta.url));
const fixtures = `${repoRoot}tests/config`;
const schema: unknown = JSON.parse(
  readFileSync(new URL("../../../assets/bdk.schema.json", import.meta.url), "utf8"),
);

interface Sidecar {
  diagnostics: { code: string; severity: string; message: string; file?: string; line?: number }[];
}

/** The language-neutral view of a diagnostic that fixtures assert. */
function view(diagnostics: Diagnostic[]): Sidecar["diagnostics"] {
  return diagnostics.map(({ code, severity, message, file, line }) => ({
    code,
    severity,
    message,
    ...(file === undefined ? {} : { file }),
    ...(line === undefined ? {} : { line }),
  }));
}

function names(directory: string, suffix: string): string[] {
  return readdirSync(`${fixtures}/${directory}`)
    .filter((name) => name.endsWith(suffix))
    .sort();
}

function readJson<T>(path: string): T {
  return JSON.parse(readFileSync(path, "utf8")) as T;
}

describe("valid fixtures", () => {
  const files = names("valid", ".toml");
  it("has fixtures", () => expect(files.length).toBeGreaterThan(0));
  it.each(files)("%s validates with no diagnostics", (name) => {
    const text = readFileSync(`${fixtures}/valid/${name}`, "utf8");
    expect(validateConfigText(text, schema as object)).toEqual([]);
  });
});

describe("invalid fixtures", () => {
  const files = names("invalid", ".toml");
  it("has fixtures", () => expect(files.length).toBeGreaterThan(0));
  it.each(files)("%s produces exactly its expected diagnostics", (name) => {
    const text = readFileSync(`${fixtures}/invalid/${name}`, "utf8");
    const expected = readJson<Sidecar>(
      `${fixtures}/invalid/${name.replace(/\.toml$/, ".expected.json")}`,
    );
    expect(expected.diagnostics.length).toBeGreaterThan(0);
    expect(view(validateConfigText(text, schema as object))).toEqual(expected.diagnostics);
  });

  it("covers every checkable code of configuration.md section 9 at least once", () => {
    const seen = new Set(
      files.flatMap((name) =>
        readJson<Sidecar>(
          `${fixtures}/invalid/${name.replace(/\.toml$/, ".expected.json")}`,
        ).diagnostics.map((d) => d.code),
      ),
    );
    for (const code of [
      "config.syntax",
      "config.unknown_key",
      "config.type",
      "config.required",
      "config.internal_url",
      "config.secret",
      "config.path_escape",
      "config.source_only",
      "config.denied_switch",
      "browser.scheme_invalid",
      "pages.alias_collision",
      "pages.alias_chain",
      "pages.reserved",
      "pages.name_invalid",
      "pages.no_builtin",
      "pages.capability_unknown",
      "pages.capability_denied",
      "ui.surface_unknown",
      "ui.surface_mode",
      "extensions.store_unknown",
      "services.provider_unknown",
    ]) {
      expect(seen, code).toContain(code);
    }
  });
});

describe("resolve fixtures", () => {
  const cases = readdirSync(`${fixtures}/resolve`).sort();
  it("has fixtures", () => expect(cases.length).toBeGreaterThan(0));
  it.each(cases)("%s", async (name) => {
    const dir = `${fixtures}/resolve/${name}`;
    const factsFile = `${dir}/bundle-facts.json`;
    const result = await resolveConfig({
      projectDir: dir,
      schema,
      ...(existsSync(factsFile) ? { bundleFacts: readJson(factsFile) } : {}),
    });
    const resolvedFile = `${dir}/expected.resolved.json`;
    const diagnosticsFile = `${dir}/expected.diagnostics.json`;
    if (existsSync(resolvedFile)) {
      expect(result.resolved).toBeDefined();
      expect(serialiseResolved(result.resolved ?? {})).toBe(readFileSync(resolvedFile, "utf8"));
    } else {
      expect(result.resolved).toBeUndefined();
    }
    const expected = existsSync(diagnosticsFile)
      ? readJson<Sidecar>(diagnosticsFile).diagnostics
      : [];
    expect(view(result.diagnostics)).toEqual(expected);
  });
});
