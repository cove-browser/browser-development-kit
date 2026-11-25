import { mkdir } from "node:fs/promises";
import { join } from "node:path";
import { describe, expect, it } from "vitest";
import { tempDir } from "../../test-support.js";
import {
  hostToolchain,
  inspectSignature,
  parseSwiftVersion,
  parseTeam,
  parseXcodeVersion,
} from "./doctor.js";
import { ToolError, type ToolRunner } from "./tools.js";

/** A runner that answers from a table, and fails (as a missing tool does) for anything else. */
function fake(table: Record<string, { stdout?: string; stderr?: string } | "fail">): {
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
      [`codesign -dv ${app}`]: { stderr: "TeamIdentifier=AAAA\n" },
      [`codesign -dv ${helper}`]: { stderr: "TeamIdentifier=BBBB\n" },
      [`xcrun stapler validate ${app}`]: "fail",
    });
    expect(await inspectSignature(runner, app)).toEqual({
      valid: true,
      team: "AAAA",
      helpers: [{ path: helper, team: "BBBB" }],
      stapled: false,
    });
    expect(calls[0]).toBe(`codesign --verify --deep --strict ${app}`);
  });

  it("reports a signature that does not verify, with codesign's words", async () => {
    const app = join(await tempDir(), "Demo.app");
    await mkdir(app);
    const { runner } = fake({ [`codesign --verify --deep --strict ${app}`]: "fail" });
    expect(await inspectSignature(runner, app)).toMatchObject({ valid: false, detail: "failed" });
  });
});
