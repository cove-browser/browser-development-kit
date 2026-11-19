import { mkdir, writeFile } from "node:fs/promises";
import { join } from "node:path";
import { describe, expect, it } from "vitest";
import { CliError, ExitCode } from "../../core/exit-codes.js";
import { tempDir } from "../../test-support.js";
import { createXcodePlatform, type PlistToJson } from "./index.js";

const fakePlutil: PlistToJson = (file) =>
  Promise.resolve(JSON.stringify({ file: file.split("/").pop() }));

describe("xcode platform", () => {
  it("detects an Xcode build from its environment", () => {
    const platform = createXcodePlatform(fakePlutil);
    expect(platform.isIdeBuild({ XCODE_VERSION_ACTUAL: "2600" })).toBe(true);
    expect(platform.isIdeBuild({})).toBe(false);
  });

  it("returns undefined when Xcode names no property lists", async () => {
    expect(await createXcodePlatform(fakePlutil).bundleFacts({}, "/p")).toBeUndefined();
  });

  it("reads Info.plist and the entitlements relative to SRCROOT", async () => {
    const src = await tempDir();
    await mkdir(join(src, "App"));
    await writeFile(join(src, "App", "Info.plist"), "x");
    await writeFile(join(src, "App", "App.entitlements"), "x");
    const facts = await createXcodePlatform(fakePlutil).bundleFacts(
      {
        SRCROOT: src,
        INFOPLIST_FILE: "App/Info.plist",
        CODE_SIGN_ENTITLEMENTS: "App/App.entitlements",
      },
      "/elsewhere",
    );
    expect(facts).toEqual({
      infoPlist: { file: "Info.plist" },
      entitlements: { file: "App.entitlements" },
    });
  });

  it("falls back to the project directory when SRCROOT is unset, and skips files that do not exist", async () => {
    const dir = await tempDir();
    await writeFile(join(dir, "Info.plist"), "x");
    const facts = await createXcodePlatform(fakePlutil).bundleFacts(
      { INFOPLIST_FILE: "Info.plist", CODE_SIGN_ENTITLEMENTS: "Missing.entitlements" },
      dir,
    );
    expect(facts).toEqual({ infoPlist: { file: "Info.plist" } });
  });

  it("reports an unreadable property list as config.plist_unreadable (exit 1)", async () => {
    const dir = await tempDir();
    await writeFile(join(dir, "Info.plist"), "x");
    const platform = createXcodePlatform(() => Promise.reject(new Error("bad plist")));
    const error = await platform.bundleFacts({ INFOPLIST_FILE: "Info.plist" }, dir).catch((e) => e);
    expect(error).toBeInstanceOf(CliError);
    expect(error.diagnostic.code).toBe("config.plist_unreadable");
    expect(error.exitCode).toBe(ExitCode.ProjectFailure);
  });

  it("reports a missing plutil as an environment error (exit 3)", async () => {
    const dir = await tempDir();
    await writeFile(join(dir, "Info.plist"), "x");
    const missing = Object.assign(new Error("spawn plutil ENOENT"), { code: "ENOENT" });
    const platform = createXcodePlatform(() => Promise.reject(missing));
    const error = await platform.bundleFacts({ INFOPLIST_FILE: "Info.plist" }, dir).catch((e) => e);
    expect(error.exitCode).toBe(ExitCode.Environment);
    expect(error.diagnostic.code).toBe("cli.tool_missing");
  });

  it("rejects a property list whose top level is not a dictionary", async () => {
    const dir = await tempDir();
    await writeFile(join(dir, "Info.plist"), "x");
    const platform = createXcodePlatform(() => Promise.resolve("[1]"));
    const error = await platform.bundleFacts({ INFOPLIST_FILE: "Info.plist" }, dir).catch((e) => e);
    expect(error.diagnostic.code).toBe("config.plist_unreadable");
  });

  it.skipIf(process.platform !== "darwin")("reads a real plist with plutil", async () => {
    const dir = await tempDir();
    await writeFile(
      join(dir, "Info.plist"),
      '<?xml version="1.0" encoding="UTF-8"?><plist version="1.0"><dict><key>CFBundleName</key><string>Demo</string></dict></plist>',
    );
    const facts = await createXcodePlatform().bundleFacts({ INFOPLIST_FILE: "Info.plist" }, dir);
    expect(facts).toEqual({ infoPlist: { CFBundleName: "Demo" } });
  });
});
