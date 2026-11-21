import { mkdirSync, mkdtempSync, readFileSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { describe, expect, it } from "vitest";
import { referencedFiles, scriptsOf, suiteFixtures, validateFixture } from "./fixtures.js";

const suite = resolve(import.meta.dirname, "..");

function fixture(files: Record<string, string>): string {
  const dir = mkdtempSync(join(tmpdir(), "bdk-fixture-"));
  for (const [name, text] of Object.entries(files)) {
    mkdirSync(join(dir, name, ".."), { recursive: true });
    writeFileSync(join(dir, name), text);
  }
  return dir;
}

const manifest = (extra: object = {}): string =>
  JSON.stringify({ manifest_version: 3, name: "x", version: "1", ...extra });

describe("the suite's fixtures", () => {
  const dirs = suiteFixtures(suite);

  it("include the probe, the incognito pair, the API surface and one of each class", () => {
    const names = dirs.map((dir) => dir.slice(suite.length + 1));
    expect(names).toEqual([
      "probe",
      "fixtures/api-surface",
      "fixtures/blocker",
      "fixtures/devtool",
      "fixtures/incognito-spanning",
      "fixtures/incognito-split",
      "fixtures/passwords",
    ]);
  });

  for (const dir of dirs) {
    it(`${dir.slice(suite.length + 1)} is a loadable Manifest V3 extension`, () => {
      expect(validateFixture(dir)).toEqual([]);
    });
  }

  it("declares the two incognito modes, and the two workers are the same file", () => {
    const read = (name: string, file: string): string =>
      readFileSync(join(suite, "fixtures", name, file), "utf8");
    expect(JSON.parse(read("incognito-spanning", "manifest.json")).incognito).toBe("spanning");
    expect(JSON.parse(read("incognito-split", "manifest.json")).incognito).toBe("split");
    expect(read("incognito-spanning", "sw.js")).toBe(read("incognito-split", "sw.js"));
  });
});

describe("validateFixture", () => {
  it("accepts a minimal extension", () => {
    expect(validateFixture(fixture({ "manifest.json": manifest() }))).toEqual([]);
  });

  it("rejects a missing manifest and invalid JSON", () => {
    expect(validateFixture(fixture({}))).toHaveLength(1);
    expect(validateFixture(fixture({ "manifest.json": "{" }))).toHaveLength(1);
  });

  it("rejects Manifest V2", () => {
    const dir = fixture({
      "manifest.json": JSON.stringify({ manifest_version: 2, name: "x", version: "1" }),
    });
    expect(validateFixture(dir)[0]).toContain("manifest_version must be 3");
  });

  it("rejects a referenced file that does not exist, and a page script that does not", () => {
    const dir = fixture({
      "manifest.json": manifest({
        background: { service_worker: "sw.js" },
        action: { default_popup: "p.html" },
      }),
      "p.html": '<script src="p.js"></script>',
    });
    const issues = validateFixture(dir);
    expect(issues).toHaveLength(2);
    expect(issues.join("\n")).toContain("sw.js");
    expect(issues.join("\n")).toContain("p.js");
  });
});

describe("referencedFiles and scriptsOf", () => {
  it("collects every manifest key that names a file", () => {
    expect(
      referencedFiles({
        background: { service_worker: "sw.js" },
        options_page: "o.html",
        devtools_page: "d.html",
        side_panel: { default_path: "s.html" },
        action: { default_popup: "p.html", default_icon: { 16: "i.png" } },
        content_scripts: [{ js: ["a.js"], css: ["a.css"] }],
        declarative_net_request: { rule_resources: [{ path: "r.json" }] },
      }).sort(),
    ).toEqual([
      "a.css",
      "a.js",
      "d.html",
      "i.png",
      "o.html",
      "p.html",
      "r.json",
      "s.html",
      "sw.js",
    ]);
  });

  it("finds local scripts and ignores remote ones", () => {
    expect(
      scriptsOf(
        '<script src="a.js"></script><script src="https://x/y.js"></script><script>1</script>',
      ),
    ).toEqual(["a.js"]);
  });
});
