import { describe, expect, it } from "vitest";
import {
  cacheDir,
  loadExtensions,
  type RealExtension,
  resolved,
  sha256Hex,
  verifyDigest,
} from "./fetch.js";

const entry = (over: Partial<RealExtension> = {}): RealExtension => ({
  id: "x",
  class: "c",
  name: "X",
  upstream: "https://github.com/o/x",
  licence: "MIT",
  version: "1.0.0",
  assetName: "x-1.0.0.zip",
  assetUrl: "https://github.com/o/x/releases/download/v1.0.0/x-1.0.0.zip",
  sha256: null,
  note: "n",
  ...over,
});

describe("real/extensions.json", () => {
  const entries = loadExtensions();

  it("lists the three classes of the M5 exit criteria", () => {
    expect(entries.map((e) => e.id)).toEqual([
      "ublock-origin-lite",
      "bitwarden",
      "react-developer-tools",
    ]);
    expect(entries.map((e) => e.class)).toEqual([
      "content blocker",
      "password manager",
      "developer tool",
    ]);
  });

  it("names an upstream project and a licence for each, and records no digest before the first fetch", () => {
    for (const e of entries) {
      expect(e.upstream).toMatch(/^https:\/\/github\.com\/[^/]+\/[^/]+$/);
      expect(e.licence).not.toBe("");
      expect(e.sha256).toBeNull();
    }
  });

  it("keeps version, asset name and URL together: all set or all null, and a GitHub release when set", () => {
    for (const e of entries) {
      const set = [e.version, e.assetName, e.assetUrl].map((v) => v !== null);
      expect(new Set(set).size).toBe(1);
      if (e.assetUrl !== null)
        expect(e.assetUrl).toMatch(/^https:\/\/github\.com\/.+\/releases\/download\//);
    }
  });
});

describe("verifyDigest", () => {
  const digest = sha256Hex(new TextEncoder().encode("abc"));

  it("hashes with SHA-256", () => {
    expect(digest).toBe("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  });

  it("accepts a matching digest, in either case", () => {
    expect(verifyDigest({ id: "x", sha256: digest }, digest)).toBe("verified");
    expect(verifyDigest({ id: "x", sha256: digest.toUpperCase() }, digest)).toBe("verified");
  });

  it("refuses a mismatch", () => {
    expect(() => verifyDigest({ id: "x", sha256: "0".repeat(64) }, digest)).toThrow(
      /sha256 mismatch/,
    );
  });

  it("reports an unrecorded digest", () => {
    expect(verifyDigest({ id: "x", sha256: null }, digest)).toBe("unrecorded");
  });
});

describe("resolved and cacheDir", () => {
  it("refuses an entry whose version or URL is still null", () => {
    expect(() => resolved(entry({ version: null }))).toThrow(/is null/);
    expect(() => resolved(entry({ assetUrl: null }))).toThrow(/is null/);
  });

  it("refuses an asset URL outside GitHub", () => {
    expect(() => resolved(entry({ assetUrl: "https://example.com/x.zip" }))).toThrow(/GitHub/);
  });

  it("places the cache under BDK_HOME/extensions-cache/<id>/<version>", () => {
    expect(cacheDir("/home/bdk", { id: "x", version: "1.0.0" })).toBe(
      "/home/bdk/extensions-cache/x/1.0.0",
    );
  });
});
