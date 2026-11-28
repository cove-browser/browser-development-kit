import { mkdir } from "node:fs/promises";
import { join } from "node:path";
import { describe, expect, it } from "vitest";
import { tempDir } from "../../test-support.js";
import {
  hostToolchain,
  inspectSignature,
  parseAuthority,
  parseGatekeeper,
  parseSwiftVersion,
  parseTeam,
  parseXcodeVersion,
} from "./doctor.js";
import { ToolError, type ToolRunner } from "./tools.js";

/**
 * A runner that answers from a table, and fails (as a missing tool does) for anything else. An answer with
 * `exit` is a tool that runs and exits with that status, its `stderr` in the error.
 */
function fake(
  table: Record<string, { stdout?: string; stderr?: string; exit?: number } | "fail">,
): {
  runner: ToolRunner;
  calls: string[];
} {
  const calls: string[] = [];
  const runner: ToolRunner = (command, args) => {
    const line = [command, ...args].join(" ");
    calls.push(line);
    const answer = table[line];
    if (answer === undefined || answer === "fail") {
      return Promise.reject(
        new ToolError(command, args, "failed", answer === undefined ? "ENOENT" : 1),
      );
    }
    if (answer.exit !== undefined) {
      return Promise.reject(new ToolError(command, args, answer.stderr ?? "", answer.exit));
    }
    return Promise.resolve({ stdout: answer.stdout ?? "", stderr: answer.stderr ?? "" });
  };
  return { runner, calls };
}

describe("xcode doctor facts", () => {
  it("parses the Apple tools' version lines", () => {
    expect(parseXcodeVersion("Xcode 27.2\nBuild version 27B52\n")).toBe("27.2");
    expect(
      parseSwiftVersion("swift-driver version: 1.168.6 Apple Swift version 6.4.2 (swiftlang)"),
    ).toBe("6.4.2");
    expect(parseSwiftVersion("Swift version 6.1 (swift-6.1-RELEASE)")).toBe("6.1");
    expect(parseTeam("Identifier=x\nTeamIdentifier=ABCDE12345\n")).toBe("ABCDE12345");
    expect(parseTeam("TeamIdentifier=not set\n")).toBeUndefined();
  });

  it("reads the toolchain, and leaves out what is not installed", async () => {
    const { runner } = fake({
      "sw_vers -productVersion": { stdout: "27.2\n" },
      "sysctl -n hw.optional.arm64": { stdout: "1\n" },
      "xcrun swift --version": { stderr: "Apple Swift version 6.4.2 (swiftlang-6.4.2)\n" },
    });
    expect(await hostToolchain(runner)).toEqual({
      os: "macos",
      osVersion: "27.2",
      arch: "arm64",
      swift: "6.4.2",
    });
  });

  it("verifies the signature, reads each helper's team and whether a ticket is stapled", async () => {
    const app = join(await tempDir(), "Demo.app");
    const helper = join(app, "Contents", "Frameworks", "R.framework", "Helpers", "R Helper.app");
    await mkdir(helper, { recursive: true });
    const { runner, calls } = fake({
      [`codesign --verify --deep --strict ${app}`]: {},
      [`codesign -dvv ${app}`]: {
        stderr:
          "Authority=Developer ID Application: A Company (AAAA)\nAuthority=Developer ID Certification Authority\nTeamIdentifier=AAAA\n",
      },
      [`codesign -dvv ${helper}`]: { stderr: "TeamIdentifier=BBBB\n" },
      [`xcrun stapler validate ${app}`]: "fail",
      [`spctl --assess --type execute -vv ${app}`]: {
        exit: 3,
        stderr: `${app}: rejected\nsource=Unnotarized Developer ID\norigin=Developer ID Application: A Company (AAAA)\n`,
      },
    });
    expect(await inspectSignature(runner, app)).toMatchObject({
      valid: true,
      team: "AAAA",
      authority: "Developer ID Application: A Company (AAAA)",
      helpers: [{ path: helper, team: "BBBB" }],
      stapled: false,
      gatekeeper: { accepted: false, source: "Unnotarized Developer ID" },
    });
    expect(calls[0]).toBe(`codesign --verify --deep --strict ${app}`);
  });

  it("reads Gatekeeper's acceptance, and leaves it out when spctl cannot run", async () => {
    const app = join(await tempDir(), "Demo.app");
    await mkdir(app);
    const accepted = fake({
      [`codesign --verify --deep --strict ${app}`]: {},
      [`spctl --assess --type execute -vv ${app}`]: {
        stderr: `${app}: accepted\nsource=Notarized Developer ID\n`,
      },
    });
    expect((await inspectSignature(accepted.runner, app)).gatekeeper).toEqual({
      accepted: true,
      source: "Notarized Developer ID",
    });
    const missing = fake({ [`codesign --verify --deep --strict ${app}`]: {} });
    const report = await inspectSignature(missing.runner, app);
    expect(report.gatekeeper).toBeUndefined();
    expect(report.authority).toBeUndefined();
  });

  it("parses the certificate and Gatekeeper's words", () => {
    expect(
      parseAuthority("Identifier=x\nAuthority=Apple Development: P (T)\nAuthority=WWDR\n"),
    ).toBe("Apple Development: P (T)");
    expect(parseAuthority("Signature=adhoc\n")).toBeUndefined();
    expect(parseGatekeeper("/a.app: rejected\n", false)).toEqual({
      accepted: false,
      detail: "/a.app: rejected",
    });
  });

  it("reports a signature that does not verify, with codesign's words", async () => {
    const app = join(await tempDir(), "Demo.app");
    await mkdir(app);
    const { runner } = fake({ [`codesign --verify --deep --strict ${app}`]: "fail" });
    expect(await inspectSignature(runner, app)).toMatchObject({ valid: false, detail: "failed" });
  });
});
