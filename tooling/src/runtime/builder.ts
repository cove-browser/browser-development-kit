import { execFileSync } from "node:child_process";
import { existsSync } from "node:fs";
import { delimiter, join } from "node:path";
import type { BuildIdentity } from "./identity.js";

function out(cmd: string, args: string[], cwd?: string): string {
  try {
    return execFileSync(cmd, args, {
      encoding: "utf8",
      stdio: ["ignore", "pipe", "ignore"],
      ...(cwd === undefined ? {} : { cwd }),
    }).trim();
  } catch {
    return "unknown";
  }
}

/** Where depot_tools lives: the directory on PATH that has `gclient`. */
function depotToolsRevision(env: NodeJS.ProcessEnv): string {
  for (const dir of (env.PATH ?? "").split(delimiter)) {
    if (dir !== "" && existsSync(join(dir, "gclient"))) {
      return out("git", ["-C", dir, "rev-parse", "HEAD"]);
    }
  }
  return "unknown";
}

/**
 * Best-effort description of the machine doing the packaging. Recorded in the manifest only; it is
 * never part of the source key. A tool that cannot be queried is recorded as "unknown".
 */
export function detectBuilder(
  checkoutSrc: string,
  env: NodeJS.ProcessEnv = process.env,
  now: Date = new Date(),
): BuildIdentity {
  const chromiumClang = join(
    checkoutSrc,
    "third_party",
    "llvm-build",
    "Release+Asserts",
    "bin",
    "clang",
  );
  const clangBin = existsSync(chromiumClang) ? chromiumClang : "clang";
  const run = env.GITHUB_RUN_ID;
  return {
    xcode: out("xcodebuild", ["-version"]).split("\n").join(" "),
    sdk: out("xcrun", ["--show-sdk-version"]),
    clang: out(clangBin, ["--version"]).split("\n")[0] ?? "unknown",
    os: out("sw_vers", ["-productVersion"]),
    depot_tools: depotToolsRevision(env),
    timestamp: now.toISOString(),
    ci_run:
      run === undefined || run === ""
        ? ""
        : `${env.GITHUB_SERVER_URL ?? ""}/${env.GITHUB_REPOSITORY ?? ""}/actions/runs/${run}`,
  };
}
