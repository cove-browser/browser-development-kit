import { mkdirSync, rmSync, symlinkSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { afterEach, describe, expect, it } from "vitest";
import { tempDir } from "../runtime/testing.js";
import {
  collectNotices,
  licenceAccepted,
  NoticesError,
  nodeNotice,
  noticeProblems,
  renderNotices,
} from "./notices.js";

const cleanup: string[] = [];
afterEach(() => {
  for (const d of cleanup.splice(0)) rmSync(d, { recursive: true, force: true });
});

function pkg(
  dir: string,
  json: Record<string, unknown>,
  files: Record<string, string> = {},
): string {
  mkdirSync(dir, { recursive: true });
  writeFileSync(join(dir, "package.json"), JSON.stringify(json));
  for (const [name, text] of Object.entries(files)) writeFileSync(join(dir, name), text);
  return dir;
}

/** A pnpm-shaped tree: the app links its dependencies into a store where each sees its own. */
function fixture(): string {
  const root = tempDir("notices");
  cleanup.push(root);
  const store = join(root, "node_modules", ".pnpm");
  pkg(
    join(store, "a@1.0.0/node_modules/a"),
    { name: "a", version: "1.0.0", license: "MIT", dependencies: { b: "^2" } },
    { LICENSE: "MIT text A" },
  );
  pkg(
    join(store, "b@2.1.0/node_modules/b"),
    { name: "b", version: "2.1.0", license: "(ISC OR GPL-3.0)" },
    { "LICENCE.md": "ISC text B" },
  );
  symlinkSync(join(store, "b@2.1.0/node_modules/b"), join(store, "a@1.0.0/node_modules/b"));
  pkg(join(store, "dev@1.0.0/node_modules/dev"), {
    name: "dev",
    version: "1.0.0",
    license: "GPL-3.0",
  });
  const ws = pkg(join(root, "packages/lib"), {
    name: "@bdk/lib",
    private: true,
    dependencies: { a: "1.0.0" },
  });
  mkdirSync(join(ws, "node_modules"));
  symlinkSync(join(store, "a@1.0.0/node_modules/a"), join(ws, "node_modules/a"));
  const app = pkg(join(root, "packages/app"), {
    name: "@bdk/app",
    private: true,
    dependencies: { "@bdk/lib": "workspace:*" },
    optionalDependencies: { optional: "1" },
    devDependencies: { dev: "1.0.0" },
  });
  mkdirSync(join(app, "node_modules/@bdk"), { recursive: true });
  symlinkSync(ws, join(app, "node_modules/@bdk/lib"));
  symlinkSync(join(store, "dev@1.0.0/node_modules/dev"), join(app, "node_modules/dev"));
  return app;
}

describe("T-REL-02 third-party notices for what BDK ships besides the runtime", () => {
  it("follows production and workspace dependencies, not devDependencies, and lists only third parties", () => {
    const app = fixture();
    const entries = collectNotices([app]);
    expect(entries.map((e) => `${e.name}@${e.version} ${e.license}`)).toEqual([
      "a@1.0.0 MIT",
      "b@2.1.0 (ISC OR GPL-3.0)",
    ]);
    expect(entries[1]?.text).toBe("ISC text B");
    expect(noticeProblems(entries)).toEqual([]);
    const text = renderNotices("Notices for app", entries, [
      { name: "Node.js", license: "MIT", text: "node" },
    ]);
    expect(text).toMatch(/^Notices for app/);
    expect(text.indexOf("Node.js")).toBeLessThan(text.indexOf("a 1.0.0"));
    expect(text).toContain("MIT text A");
  });

  it("fails a missing dependency and reports licences that need review or ship no text", () => {
    const app = fixture();
    pkg(app, { name: "@bdk/app", private: true, dependencies: { missing: "1" } });
    expect(() => collectNotices([app])).toThrow(NoticesError);
    const problems = noticeProblems([
      { name: "g", version: "1", license: "GPL-3.0-only", text: "t" },
      { name: "n", version: "1", license: "", text: "t" },
      { name: "x", version: "1", license: "MIT", text: "" },
    ]);
    expect(problems).toEqual([
      "g@1: GPL-3.0-only needs a licence review (docs/release/licence-review.md)",
      "n@1: no licence in package.json",
      "x@1: ships no licence file to reproduce",
    ]);
  });

  it("reads SPDX expressions", () => {
    expect(licenceAccepted("MIT")).toBe(true);
    expect(licenceAccepted("(MIT OR Apache-2.0)")).toBe(true);
    expect(licenceAccepted("MIT AND BSD-3-Clause")).toBe(true);
    expect(licenceAccepted("MIT AND GPL-2.0")).toBe(false);
    expect(licenceAccepted("Apache-2.0 WITH LLVM-exception")).toBe(false);
    expect(licenceAccepted("MPL-2.0")).toBe(false);
  });

  it("everything the bdk executable bundles has an accepted licence and its text, and Node's licence is found", () => {
    const cli = join(import.meta.dirname, "..", "..", "..", "packages", "cli");
    const entries = collectNotices([cli]);
    const names = entries.map((e) => e.name);
    expect(names).toContain("ajv");
    expect(names).toContain("smol-toml");
    expect(names.some((n) => n.startsWith("@bdk/"))).toBe(false);
    expect(noticeProblems(entries)).toEqual([]);
    expect(nodeNotice().text).toMatch(/Node\.js/);
  });
});
