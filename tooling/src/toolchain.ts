import { spawnSync } from "node:child_process";
import { readFileSync } from "node:fs";
import { resolve } from "node:path";

export interface Check {
  readonly ok: boolean;
  readonly message: string;
}

/** Repository root, two levels above `tooling/src`. */
export const repoRoot = resolve(import.meta.dirname, "..", "..");

/** The pinned Node release from `.node-version`. */
export function readNodePin(root: string = repoRoot): string {
  return readFileSync(resolve(root, ".node-version"), "utf8").trim().replace(/^v/, "");
}

/** Node must match the pin exactly: one Node release builds the repository and ships in `bdk`. */
export function checkNodeVersion(actual: string, pin: string): Check {
  const have = actual.replace(/^v/, "");
  if (have === pin) {
    return { ok: true, message: `node ${have} matches .node-version (${pin})` };
  }
  return {
    ok: false,
    message:
      `node ${have} does not match the pinned release ${pin} in .node-version. ` +
      `Install it (fnm install ${pin} && fnm use) and run setup again.`,
  };
}

/** `packageManager` in package.json is `pnpm@x.y.z`; the running pnpm must match. */
export function checkPackageManager(actual: string, declared: string): Check {
  const want = declared.replace(/^pnpm@/, "");
  if (actual === want) {
    return { ok: true, message: `pnpm ${actual} matches packageManager (${declared})` };
  }
  return {
    ok: false,
    message: `pnpm ${actual} does not match packageManager ${declared} in package.json.`,
  };
}

export interface ToolSpec {
  readonly name: string;
  readonly args: readonly string[];
  readonly purpose: string;
}

export const tools: readonly ToolSpec[] = [
  { name: "cmake", args: ["--version"], purpose: "packages/core build" },
  { name: "ninja", args: ["--version"], purpose: "packages/core build" },
  { name: "swift", args: ["--version"], purpose: "packages/swift build and swift format" },
  { name: "clang-format", args: ["--version"], purpose: "C and C++ formatting" },
  { name: "swiftlint", args: ["--version"], purpose: "Swift lint" },
  { name: "shellcheck", args: ["--version"], purpose: "shell lint" },
  { name: "shfmt", args: ["--version"], purpose: "shell formatting" },
  { name: "actionlint", args: ["-version"], purpose: "workflow lint" },
  { name: "taplo", args: ["--version"], purpose: "TOML formatting" },
  { name: "gitleaks", args: ["version"], purpose: "secret scan in the pre-commit hook" },
  { name: "lefthook", args: ["version"], purpose: "git hooks" },
];

/** Returns the first line of the tool's version output, or undefined when it is not on PATH. */
export function probeTool(spec: ToolSpec): string | undefined {
  const result = spawnSync(spec.name, [...spec.args], { encoding: "utf8" });
  if (result.error !== undefined || result.status !== 0) {
    return undefined;
  }
  return (result.stdout || result.stderr).split("\n")[0]?.trim() ?? "";
}
