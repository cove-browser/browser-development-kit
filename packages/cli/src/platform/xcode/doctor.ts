import { readdir } from "node:fs/promises";
import { join } from "node:path";
import type { HostToolchain, SignatureReport } from "../platform.js";
import { ToolError, type ToolRunner } from "./tools.js";

/** Runs a tool and returns its output, or undefined when it is missing or fails. */
async function output(runner: ToolRunner, command: string, args: readonly string[]) {
  try {
    return await runner(command, args);
  } catch (error) {
    if (error instanceof ToolError) return undefined;
    throw error;
  }
}

/** `Xcode 27.2` in `xcodebuild -version`. */
export function parseXcodeVersion(text: string): string | undefined {
  return /^Xcode (\d+(?:\.\d+)*)/m.exec(text)?.[1];
}

/** `Apple Swift version 6.4.2 (...)`, or `Swift version 6.1` for a toolchain from swift.org. */
export function parseSwiftVersion(text: string): string | undefined {
  return /Swift version (\d+(?:\.\d+)*)/.exec(text)?.[1];
}

/** `TeamIdentifier=ABCDE12345` in `codesign -dv`; undefined for `not set` (ad hoc). */
export function parseTeam(text: string): string | undefined {
  const team = /^TeamIdentifier=(.+)$/m.exec(text)?.[1]?.trim();
  return team === undefined || team === "not set" ? undefined : team;
}

/**
 * macOS, its version, the architecture (Apple Silicon even when Node runs under Rosetta), Xcode and
 * Swift. Every value comes from an Apple tool run as a subprocess.
 */
export async function hostToolchain(runner: ToolRunner): Promise<HostToolchain> {
  const [os, arm64, xcode, swift] = await Promise.all([
    output(runner, "sw_vers", ["-productVersion"]),
    output(runner, "sysctl", ["-n", "hw.optional.arm64"]),
    output(runner, "xcodebuild", ["-version"]),
    output(runner, "xcrun", ["swift", "--version"]),
  ]);
  const toolchain: HostToolchain = {
    os: "macos",
    arch: arm64?.stdout.trim() === "1" ? "arm64" : process.arch,
  };
  const osVersion = os?.stdout.trim();
  if (osVersion) toolchain.osVersion = osVersion;
  const ide = xcode === undefined ? undefined : parseXcodeVersion(xcode.stdout);
  if (ide !== undefined) toolchain.ide = ide;
  const swiftVersion =
    swift === undefined ? undefined : parseSwiftVersion(`${swift.stdout}\n${swift.stderr}`);
  if (swiftVersion !== undefined) toolchain.swift = swiftVersion;
  return toolchain;
}

/** Every `.app` under `dir`, at any depth, without descending into one. */
async function nestedApps(dir: string): Promise<string[]> {
  let entries: import("node:fs").Dirent[];
  try {
    entries = await readdir(dir, { withFileTypes: true });
  } catch {
    return [];
  }
  const found: string[] = [];
  for (const entry of entries.sort((a, b) => (a.name < b.name ? -1 : 1))) {
    if (!entry.isDirectory()) continue;
    const path = join(dir, entry.name);
    if (entry.name.endsWith(".app")) found.push(path);
    else found.push(...(await nestedApps(path)));
  }
  return found;
}

/** The application's signature, its helpers' teams and the stapled ticket, through `codesign` and `stapler`. */
export async function inspectSignature(
  runner: ToolRunner,
  appPath: string,
): Promise<SignatureReport> {
  const report: SignatureReport = { valid: true, helpers: [], stapled: false };
  try {
    await runner("codesign", ["--verify", "--deep", "--strict", appPath]);
  } catch (error) {
    if (!(error instanceof ToolError)) throw error;
    report.valid = false;
    report.detail = error.stderr.trim() || error.message;
  }
  const teamOf = async (path: string) => {
    const shown = await output(runner, "codesign", ["-dv", path]);
    return shown === undefined ? undefined : parseTeam(`${shown.stdout}\n${shown.stderr}`);
  };
  const team = await teamOf(appPath);
  if (team !== undefined) report.team = team;
  for (const helper of await nestedApps(join(appPath, "Contents", "Frameworks"))) {
    const helperTeam = await teamOf(helper);
    report.helpers.push(
      helperTeam === undefined ? { path: helper } : { path: helper, team: helperTeam },
    );
  }
  report.stapled = (await output(runner, "xcrun", ["stapler", "validate", appPath])) !== undefined;
  return report;
}
