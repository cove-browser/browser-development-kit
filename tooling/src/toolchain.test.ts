import { describe, expect, it } from "vitest";
import { checkNodeVersion, checkPackageManager, readNodePin } from "./toolchain.js";

describe("checkNodeVersion", () => {
  it("accepts the pinned release", () => {
    expect(checkNodeVersion("26.9.0", "26.9.0").ok).toBe(true);
    expect(checkNodeVersion("v26.9.0", "26.9.0").ok).toBe(true);
  });

  it("rejects any other release and names the pin", () => {
    for (const other of ["22.22.1", "26.9.1", "26.8.0", "27.0.0"]) {
      const result = checkNodeVersion(other, "26.9.0");
      expect(result.ok).toBe(false);
      expect(result.message).toContain("26.9.0");
      expect(result.message).toContain(".node-version");
    }
  });
});

describe("checkPackageManager", () => {
  it("compares against the packageManager field", () => {
    expect(checkPackageManager("12.21.0", "pnpm@12.21.0").ok).toBe(true);
    expect(checkPackageManager("11.1.2", "pnpm@12.21.0").ok).toBe(false);
  });
});

describe("readNodePin", () => {
  it("reads .node-version from the repository", () => {
    expect(readNodePin()).toMatch(/^\d+\.\d+\.\d+$/);
  });
});
