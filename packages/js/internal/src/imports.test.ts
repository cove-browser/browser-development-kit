import { existsSync, readdirSync, readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { describe, expect, it } from "vitest";

const SOURCE = dirname(fileURLToPath(import.meta.url));
const DIST = join(SOURCE, "..", "dist");

/** Every module specifier a file loads at run time: static imports, re-exports and literal import(). */
function runtimeSpecifiers(text: string): string[] {
  const specifiers: string[] = [];
  const statements = /(?:^|[\s;])(import|export)\s+(type\s+)?([^;"']*?)\s*from\s*["']([^"']+)["']/g;
  for (const [, , type, , specifier] of text.matchAll(statements)) {
    if (type === undefined && specifier !== undefined) specifiers.push(specifier);
  }
  for (const [, specifier] of text.matchAll(/(?:^|[\s;])import\s*["']([^"']+)["']/g)) {
    if (specifier !== undefined) specifiers.push(specifier);
  }
  for (const [, specifier] of text.matchAll(/\bimport\(\s*["']([^"']+)["']\s*\)/g)) {
    if (specifier !== undefined) specifiers.push(specifier);
  }
  return specifiers.sort();
}

function filesIn(directory: string, suffix: string, exclude: RegExp): string[] {
  return readdirSync(directory)
    .filter((name) => name.endsWith(suffix) && !exclude.test(name))
    .map((name) => join(directory, name));
}

function bare(files: readonly string[]): string[] {
  return files.flatMap((file) =>
    runtimeSpecifiers(readFileSync(file, "utf8"))
      .filter((specifier) => !/^\.\/[\w-]+\.js$/.test(specifier))
      .map((specifier) => `${file}: ${specifier}`),
  );
}

describe("the page runs these modules unbundled", () => {
  it("scans: a bare or non-relative runtime import is found", () => {
    expect(runtimeSpecifiers('import { a } from "@bdk/protocol";')).toEqual(["@bdk/protocol"]);
    expect(runtimeSpecifiers('import { type A } from "@bdk/protocol";')).toEqual(["@bdk/protocol"]);
    expect(runtimeSpecifiers('import "x";\nexport * from "y";')).toEqual(["x", "y"]);
    expect(runtimeSpecifiers('const m = await import("z");')).toEqual(["z"]);
    expect(runtimeSpecifiers('import type { A } from "@bdk/protocol";')).toEqual([]);
    expect(runtimeSpecifiers('export type { A } from "@bdk/protocol";')).toEqual([]);
  });

  it("imports only sibling modules with a relative specifier in the sources", () => {
    const sources = filesIn(SOURCE, ".ts", /\.test\.ts$/);
    expect(sources.length).toBeGreaterThan(10);
    expect(bare(sources)).toEqual([]);
  });

  it("imports only sibling modules with a relative specifier in the built output", () => {
    if (!existsSync(DIST)) return;
    const built = filesIn(DIST, ".js", /\.test\.js$/);
    expect(bare(built)).toEqual([]);
  });

  it("builds a flat dist: no subdirectory", () => {
    if (!existsSync(DIST)) return;
    const directories = readdirSync(DIST, { withFileTypes: true }).filter((e) => e.isDirectory());
    expect(directories.map((e) => e.name)).toEqual([]);
  });
});
