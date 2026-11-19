import { describe, expect, it } from "vitest";
import { BdkHomeError, bdkPaths, resolveBdkHome } from "./paths.js";

describe("resolveBdkHome", () => {
  it("defaults to ~/.bdk on macOS and Linux", () => {
    expect(resolveBdkHome({ env: {}, platform: "darwin", home: "/Users/dev" })).toBe(
      "/Users/dev/.bdk",
    );
    expect(resolveBdkHome({ env: {}, platform: "linux", home: "/home/dev" })).toBe(
      "/home/dev/.bdk",
    );
  });

  it("defaults to %LOCALAPPDATA%\\bdk on Windows", () => {
    expect(
      resolveBdkHome({
        env: { LOCALAPPDATA: "C:\\Users\\dev\\AppData\\Local" },
        platform: "win32",
      }),
    ).toBe("C:\\Users\\dev\\AppData\\Local\\bdk");
  });

  it("prefers BDK_HOME", () => {
    expect(resolveBdkHome({ env: { BDK_HOME: "/data/bdk" }, platform: "darwin" })).toBe(
      "/data/bdk",
    );
  });

  it("rejects a path containing spaces", () => {
    expect(() =>
      resolveBdkHome({ env: { BDK_HOME: "/Users/a b/.bdk" }, platform: "darwin" }),
    ).toThrow(/must not contain spaces/);
    expect(() => resolveBdkHome({ env: {}, platform: "darwin", home: "/Users/a b" })).toThrow(
      BdkHomeError,
    );
  });

  it("rejects a relative path", () => {
    expect(() => resolveBdkHome({ env: { BDK_HOME: "rel/bdk" }, platform: "linux" })).toThrow(
      /absolute/,
    );
  });

  it("needs LOCALAPPDATA on Windows without BDK_HOME", () => {
    expect(() => resolveBdkHome({ env: {}, platform: "win32" })).toThrow(BdkHomeError);
  });
});

describe("bdkPaths", () => {
  const paths = bdkPaths("/h/.bdk", "darwin");

  it("lays out the section 7 subpaths", () => {
    expect(paths.runtimes).toBe("/h/.bdk/runtimes");
    expect(paths.runtime("abc123")).toBe("/h/.bdk/runtimes/abc123");
    expect(paths.gitCache).toBe("/h/.bdk/git-cache");
    expect(paths.chromiumManaged).toBe("/h/.bdk/chromium/managed");
    expect(paths.chromiumExperiment("agent-1")).toBe("/h/.bdk/chromium/exp/agent-1");
    expect(paths.chromiumLocks).toBe("/h/.bdk/chromium/locks");
    expect(paths.ccache).toBe("/h/.bdk/ccache");
  });

  it("rejects ids that escape their directory", () => {
    expect(() => paths.chromiumExperiment("../x")).toThrow(BdkHomeError);
    expect(() => paths.runtime("a/b")).toThrow(BdkHomeError);
  });
});
