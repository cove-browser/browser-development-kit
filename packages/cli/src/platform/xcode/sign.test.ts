import { readFile, writeFile } from "node:fs/promises";
import { join } from "node:path";
import { describe, expect, it } from "vitest";
import { CliError, ExitCode } from "../../core/exit-codes.js";
import { fakeApp } from "../../fake-app.js";
import { tempDir } from "../../test-support.js";
import { codesignArgs, discoverLayout, signBundle } from "./sign.js";
import { ToolError, type ToolRunner } from "./tools.js";

interface Call {
  command: string;
  args: string[];
}

/** A runner that records calls, answers plutil's id lookup, and fails what `fail` says. */
function fakeRunner(options: {
  appId?: string;
  entitlements?: Record<string, unknown>;
  fail?: (call: Call) => ToolError | undefined;
  team?: string;
}): { runner: ToolRunner; calls: Call[]; entitlementFiles: Map<string, string> } {
  const calls: Call[] = [];
  const entitlementFiles = new Map<string, string>();
  const runner: ToolRunner = async (command, args) => {
    const call = { command, args: [...args] };
    calls.push(call);
    const failure = options.fail?.(call);
    if (failure !== undefined) throw failure;
    if (command === "plutil" && args[0] === "-extract") {
      return { stdout: `${options.appId ?? "com.acme.App"}\n`, stderr: "" };
    }
    if (command === "plutil" && args[0] === "-convert") {
      return { stdout: JSON.stringify(options.entitlements ?? {}), stderr: "" };
    }
    if (command === "codesign") {
      const at = args.indexOf("--entitlements");
      const file = args[at + 1];
      if (at >= 0 && file !== undefined && file.includes("bdk-sign-")) {
        entitlementFiles.set(file, await readFile(file, "utf8"));
      }
      if (args[0] === "--display") {
        return { stdout: "", stderr: `TeamIdentifier=${options.team ?? "TEAM123"}\n` };
      }
    }
    return { stdout: "", stderr: "" };
  };
  return { runner, calls, entitlementFiles };
}

describe("discoverLayout", () => {
  it("finds the framework, its libraries, executables and helper applications by real path", async () => {
    const { appPath, versionDir } = await fakeApp();
    const layout = await discoverLayout(appPath, fakeRunner({}).runner);
    expect(layout.appBundleId).toBe("com.acme.App");
    expect(layout.frameworkPath.endsWith("Fake Framework.framework")).toBe(true);
    expect(layout.frameworkPlist).toBe(join(versionDir, "Resources", "Info.plist"));
    expect(layout.libraries.map((p) => p.split("/").pop())).toEqual([
      "libaperitif.dylib",
      "libvulkan.dylib",
    ]);
    expect(layout.executables.map((p) => p.split("/").pop())).toEqual([
      "app_mode_loader",
      "chrome_crashpad_handler",
      "web_app_shortcut_copier",
    ]);
    expect(layout.helpers).toHaveLength(8);
    expect(layout.helpers.every((p) => p.startsWith(versionDir))).toBe(true);
  });

  it("fails with sign.no_runtime when no framework is embedded", async () => {
    const dir = await tempDir();
    const app = join(dir, "A.app");
    const { mkdir } = await import("node:fs/promises");
    await mkdir(join(app, "Contents"), { recursive: true });
    await expect(discoverLayout(app, fakeRunner({}).runner)).rejects.toMatchObject({
      diagnostic: { code: "sign.no_runtime" },
    });
  });

  it("finds the runtime among other embedded frameworks", async () => {
    const { appPath } = await fakeApp();
    const { mkdir } = await import("node:fs/promises");
    const other = join(appPath, "Contents", "Frameworks", "Another.framework", "Versions", "A");
    await mkdir(join(other, "Resources"), { recursive: true });
    const layout = await discoverLayout(appPath, fakeRunner({}).runner);
    expect(layout.frameworkPath.endsWith("Fake Framework.framework")).toBe(true);
  });

  it("fails with sign.no_runtime when the only framework has no helpers", async () => {
    const dir = await tempDir();
    const app = join(dir, "A.app");
    const { mkdir } = await import("node:fs/promises");
    await mkdir(join(app, "Contents", "Frameworks", "Another.framework", "Resources"), {
      recursive: true,
    });
    await expect(discoverLayout(app, fakeRunner({}).runner)).rejects.toMatchObject({
      diagnostic: { code: "sign.no_runtime" },
    });
  });

  it("fails with sign.bundle_missing for a path that is not a directory", async () => {
    await expect(discoverLayout("/nonexistent/A.app", fakeRunner({}).runner)).rejects.toMatchObject(
      {
        diagnostic: { code: "sign.bundle_missing" },
      },
    );
  });

  it("fails with sign.bundle_id_missing when plutil finds no identifier", async () => {
    const { appPath } = await fakeApp();
    const runner = fakeRunner({ appId: "" }).runner;
    await expect(discoverLayout(appPath, runner)).rejects.toMatchObject({
      diagnostic: { code: "sign.bundle_id_missing" },
    });
  });
});

describe("codesignArgs", () => {
  const action = {
    kind: "sign",
    role: "helper",
    path: "/x.app",
    options: ["restrict", "runtime", "kill"],
  } as const;

  it("signs ad hoc without a timestamp", () => {
    expect(codesignArgs(action, "-", "/e.plist")).toEqual([
      "--sign",
      "-",
      "--force",
      "--options",
      "restrict,runtime,kill",
      "--entitlements",
      "/e.plist",
      "/x.app",
    ]);
  });

  it("adds a secure timestamp for a real identity, and an explicit identifier", () => {
    expect(
      codesignArgs({ ...action, options: [], identifier: "i" }, "Developer ID", undefined),
    ).toEqual(["--sign", "Developer ID", "--force", "--timestamp", "--identifier", "i", "/x.app"]);
  });
});

describe("signBundle", () => {
  it("rewrites identifiers, signs inside-out, then verifies the application", async () => {
    const { appPath } = await fakeApp();
    const { runner, calls, entitlementFiles } = fakeRunner({});
    const outcome = await signBundle({ appPath, identity: "-" }, runner);

    const kinds = calls.map((c) => `${c.command}:${c.args[0]}`);
    const firstSign = kinds.indexOf("codesign:--sign");
    // plutil -extract (app id), 9 rewrites, then signatures, then verify.
    expect(kinds.slice(0, firstSign)).toEqual([
      "plutil:-extract",
      ...Array(9).fill("plutil:-replace"),
    ]);
    expect(kinds.slice(firstSign, -1).every((k) => k === "codesign:--sign")).toBe(true);
    expect(kinds.at(-1)).toBe("codesign:--verify");
    expect(calls.at(-1)?.args).toEqual(["--verify", "--deep", "--strict", "--verbose=2", appPath]);

    const signedNames = outcome.signed.map((p) => p.split("/").pop());
    expect(signedNames.slice(0, 2)).toEqual(["libaperitif.dylib", "libvulkan.dylib"]);
    expect(signedNames.slice(-2)).toEqual(["Fake Framework.framework", "Demo.app"]);
    expect(outcome.signed).toHaveLength(2 + 3 + 8 + 2);

    const replace = calls.filter((c) => c.args[0] === "-replace");
    expect(
      replace.every((c) => c.args.slice(0, 3).join(" ") === "-replace CFBundleIdentifier -string"),
    ).toBe(true);
    expect(replace.map((c) => c.args[3]).sort()).toEqual([
      "com.acme.App.framework",
      "com.acme.App.framework.AlertNotificationService",
      "com.acme.App.framework.AlertNotificationService",
      "com.acme.App.helper",
      "com.acme.App.helper",
      "com.acme.App.helper",
      "com.acme.App.helper",
      "com.acme.App.helper.renderer",
      "com.acme.App.helper.renderer",
    ]);

    // Entitlement files were written for the four JIT helpers and removed afterwards.
    expect([...entitlementFiles.values()]).toHaveLength(4);
    for (const text of entitlementFiles.values()) {
      expect(text).toContain("com.apple.security.cs.allow-jit");
    }
  });

  it("copies the runtime's third-party notices into the application before signing", async () => {
    const { appPath, versionDir } = await fakeApp();
    await writeFile(join(versionDir, "Resources", "THIRD_PARTY_NOTICES.html"), "<p>libfoo</p>");
    const { runner } = fakeRunner({});
    const outcome = await signBundle({ appPath, identity: "-" }, runner);
    expect(outcome.diagnostics).toEqual([]);
    expect(
      await readFile(join(appPath, "Contents", "Resources", "THIRD_PARTY_NOTICES.html"), "utf8"),
    ).toBe("<p>libfoo</p>");
  });

  it("warns when the runtime has no notices, and still signs", async () => {
    const { appPath } = await fakeApp();
    const { runner } = fakeRunner({});
    const outcome = await signBundle({ appPath, identity: "-" }, runner);
    expect(outcome.diagnostics.map((d) => d.code)).toEqual(["sign.notices_missing"]);
    expect(outcome.signed.at(-1)?.split("/").pop()).toBe("Demo.app");
  });

  it("passes the application's entitlements file to codesign for the app only", async () => {
    const { appPath } = await fakeApp();
    const dir = await tempDir();
    const file = join(dir, "App.entitlements");
    await writeFile(file, "x");
    const { runner, calls } = fakeRunner({
      entitlements: { "com.apple.security.device.camera": true },
    });
    await signBundle({ appPath, identity: "-", entitlementsFile: file }, runner);
    const withFile = calls.filter((c) => c.command === "codesign" && c.args.includes(file));
    expect(withFile).toHaveLength(1);
    expect(withFile[0]?.args.at(-1)).toBe(appPath);
  });

  it("refuses a denied application entitlement and runs no signing or rewriting", async () => {
    const { appPath } = await fakeApp();
    const dir = await tempDir();
    const file = join(dir, "App.entitlements");
    await writeFile(file, "x");
    const { runner, calls } = fakeRunner({
      entitlements: { "com.apple.security.app-sandbox": true },
    });
    const outcome = await signBundle({ appPath, identity: "-", entitlementsFile: file }, runner);
    expect(outcome.signed).toEqual([]);
    expect(outcome.diagnostics.map((d) => d.code)).toEqual(["sign.entitlement_denied"]);
    expect(calls.every((c) => c.args[0] === "-convert")).toBe(true);
  });

  it("fails when the entitlements file does not exist", async () => {
    const { appPath } = await fakeApp();
    await expect(
      signBundle(
        { appPath, identity: "-", entitlementsFile: "/nope.plist" },
        fakeRunner({}).runner,
      ),
    ).rejects.toMatchObject({ diagnostic: { code: "sign.entitlements_missing" } });
  });

  it("reports a failed verification as an integrity failure (exit 4)", async () => {
    const { appPath } = await fakeApp();
    const { runner } = fakeRunner({
      fail: (c) =>
        c.args[0] === "--verify"
          ? new ToolError("codesign", c.args, "a sealed resource is missing", 1)
          : undefined,
    });
    const error = await signBundle({ appPath, identity: "-" }, runner).catch((e) => e);
    expect(error).toBeInstanceOf(CliError);
    expect(error.exitCode).toBe(ExitCode.Integrity);
    expect(error.diagnostic.code).toBe("sign.verify_failed");
    expect(error.diagnostic.message).toContain("a sealed resource is missing");
  });

  it("reports a missing codesign as an environment error (exit 3)", async () => {
    const { appPath } = await fakeApp();
    const { runner } = fakeRunner({
      fail: (c) =>
        c.command === "codesign" ? new ToolError("codesign", c.args, "", "ENOENT") : undefined,
    });
    const error = await signBundle({ appPath, identity: "-" }, runner).catch((e) => e);
    expect(error.exitCode).toBe(ExitCode.Environment);
    expect(error.diagnostic.code).toBe("cli.tool_missing");
  });

  it("checks the team after signing with a real identity", async () => {
    const { appPath } = await fakeApp();
    const ok = await signBundle(
      { appPath, identity: "ID", team: "TEAM123" },
      fakeRunner({}).runner,
    );
    expect(ok.diagnostics.filter((d) => d.severity === "error")).toEqual([]);
    const error = await signBundle(
      { appPath, identity: "ID", team: "OTHER" },
      fakeRunner({}).runner,
    ).catch((e) => e);
    expect(error.diagnostic.code).toBe("sign.team_mismatch");
    expect(error.exitCode).toBe(ExitCode.Integrity);
  });

  it("does not check the team for an ad hoc signature", async () => {
    const { appPath } = await fakeApp();
    const { runner, calls } = fakeRunner({});
    await signBundle({ appPath, identity: "-", team: "TEAM123" }, runner);
    expect(calls.some((c) => c.args[0] === "--display")).toBe(false);
  });
});
