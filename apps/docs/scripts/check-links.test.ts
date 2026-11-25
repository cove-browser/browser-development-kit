import { mkdirSync, mkdtempSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { describe, expect, it } from "vitest";
import { checkLinks } from "./check-links.js";

/** A fake export: `files` maps site paths to HTML. */
function site(files: Record<string, string>): string {
  const out = mkdtempSync(join(tmpdir(), "bdk-docs-out-"));
  for (const [path, html] of Object.entries(files)) {
    mkdirSync(dirname(join(out, path)), { recursive: true });
    writeFileSync(join(out, path), html);
  }
  return out;
}

describe("checkLinks", () => {
  it("accepts links to pages, files and fragments that exist, and skips external ones", () => {
    const out = site({
      "index.html": '<a href="/docs/cli/#bdk-doctor">x</a><a href="https://example.com/">y</a>',
      "docs/cli/index.html": '<h2 id="bdk-doctor">bdk doctor</h2><img src="/logo.svg">',
      "logo.svg": "<svg/>",
    });
    expect(checkLinks(out)).toEqual({ pages: 2, checked: 2, broken: [] });
  });

  it("reports a missing page, a missing fragment and a relative link", () => {
    const out = site({
      "index.html": '<a href="/docs/gone/">a</a><a href="/#nowhere">b</a><a href="cli.mdx">c</a>',
    });
    expect(checkLinks(out).broken).toEqual([
      '/index.html: "/docs/gone/" names nothing in the export',
      '/index.html: "/#nowhere" names no element with id "nowhere"',
      '/index.html: relative link "cli.mdx" (links in the export are site-absolute)',
    ]);
  });

  it("checks links under the base path the site was built with", () => {
    const out = site({
      "index.html": '<a href="/bdk/docs/">a</a><a href="/docs/">b</a>',
      "docs/index.html": "",
    });
    expect(checkLinks(out, "/bdk").broken).toEqual([
      '/index.html: "/docs/" is outside the base path /bdk',
    ]);
  });
});
