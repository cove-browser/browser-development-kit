import { spawnSync } from "node:child_process";
import { existsSync, readFileSync } from "node:fs";
import { resolve } from "node:path";
import {
  checkNodeVersion,
  checkPackageManager,
  probeTool,
  readNodePin,
  repoRoot,
  tools,
} from "./toolchain.js";

interface Pin {
  readonly runtime?: unknown;
}

function run(command: string, args: readonly string[]): boolean {
  return spawnSync(command, [...args], { cwd: repoRoot, stdio: "inherit" }).status === 0;
}

function main(): number {
  let failed = false;
  const fail = (message: string): void => {
    failed = true;
    console.error(`error: ${message}`);
  };

  // 1. Node must match .node-version exactly.
  const node = checkNodeVersion(process.versions.node, readNodePin());
  if (node.ok) console.log(`ok: ${node.message}`);
  else fail(node.message);

  // 2. pnpm must match the packageManager field.
  const manifest = JSON.parse(readFileSync(resolve(repoRoot, "package.json"), "utf8")) as {
    packageManager: string;
  };
  const pnpm = spawnSync("pnpm", ["--version"], { cwd: repoRoot, encoding: "utf8" });
  if (pnpm.status === 0) {
    const check = checkPackageManager(pnpm.stdout.trim(), manifest.packageManager);
    if (check.ok) console.log(`ok: ${check.message}`);
    else fail(check.message);
  } else {
    fail("pnpm is not on PATH.");
  }

  // 3. Other tools: report versions, warn when missing.
  const missing: string[] = [];
  for (const tool of tools) {
    const version = probeTool(tool);
    if (version === undefined) {
      missing.push(`${tool.name} (${tool.purpose})`);
    } else {
      console.log(`ok: ${tool.name} ${version}`);
    }
  }
  if (missing.length > 0) {
    console.warn(`warning: not found on PATH: ${missing.join(", ")}`);
  }

  // 4. Git hooks.
  if (probeTool({ name: "lefthook", args: ["version"], purpose: "" }) === undefined) {
    fail(
      "lefthook is not on PATH, so the git hooks could not be installed. Install it (brew install lefthook).",
    );
  } else if (run("lefthook", ["install"])) {
    console.log("ok: lefthook hooks installed");
  } else {
    fail("lefthook install failed.");
  }

  // 5. Resolve the pinned runtime. Never fetches Chromium.
  const pinPath = resolve(repoRoot, "chromium", "pin.json");
  const pin = JSON.parse(readFileSync(pinPath, "utf8")) as Pin;
  if (pin.runtime === null || pin.runtime === undefined) {
    console.log(
      "note: no published runtime in chromium/pin.json yet; skipping runtime resolution.",
    );
  } else if (!existsSync(resolve(repoRoot, "tooling", "src", "chromium", "runtime.ts"))) {
    console.log("note: chromium:runtime is not available yet; skipping runtime resolution.");
  } else if (run("pnpm", ["exec", "nx", "run", "chromium:runtime"])) {
    console.log("ok: runtime resolved");
  } else {
    fail("chromium:runtime failed to resolve the pinned runtime.");
  }

  return failed ? 1 : 0;
}

process.exit(main());
