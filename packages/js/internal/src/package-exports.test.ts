import { readdirSync, readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { describe, expect, it } from "vitest";

const SOURCE = dirname(fileURLToPath(import.meta.url));
const manifest = JSON.parse(readFileSync(join(SOURCE, "..", "package.json"), "utf8")) as {
  private: boolean;
  exports: Record<string, { types: string; default: string }>;
};

const modules = readdirSync(SOURCE)
  .filter((name) => name.endsWith(".ts") && !name.endsWith(".test.ts"))
  .map((name) => name.slice(0, -3))
  .sort();

describe("package.json", () => {
  it("stays private: nothing is published to npm", () => {
    expect(manifest.private).toBe(true);
  });

  it("exports every module as @bdk/internal/<module>, flat, with its types", () => {
    const expected = Object.fromEntries(
      modules.map((name) => [
        name === "index" ? "." : `./${name}`,
        { types: `./dist/${name}.d.ts`, default: `./dist/${name}.js` },
      ]),
    );
    expect(manifest.exports).toEqual(expected);
  });
});
