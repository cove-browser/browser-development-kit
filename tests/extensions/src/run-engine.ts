import { spawnSync } from "node:child_process";
import { mkdirSync, writeFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { isMain, repoRootFromHere } from "../../../tooling/src/runtime/cli.js";
import { generatedFiles } from "./cli.js";
import { parseResults } from "./results.js";

/** Selects every extension test of the engine conformance app (the convention is in tests/extensions/README.md). */
export const ENGINE_SELECTION = "engine_ext_*";
export const ENGINE_SCRIPT = "packages/core/scripts/test-engine.sh";
export const RESULTS_FILE = "tests/extensions/results/engine-extensions.txt";

/** What test-engine.sh prints and exits 0 with when there is no runtime to run against. */
const NO_RUNTIME = /no runtime at .*Skipped\./;

export interface EngineRun {
  readonly status: number;
  readonly stdout: string;
}

export interface Deps {
  readonly root: string;
  readonly platform: NodeJS.Platform;
  readonly runEngine: (root: string) => EngineRun;
  readonly log: (line: string) => void;
}

function runScript(root: string): EngineRun {
  const run = spawnSync("bash", [resolve(root, ENGINE_SCRIPT), "--run", ENGINE_SELECTION], {
    cwd: root,
    encoding: "utf8",
    maxBuffer: 64 * 1024 * 1024,
    stdio: ["ignore", "pipe", "inherit"],
  });
  return { status: run.status ?? 1, stdout: run.stdout ?? "" };
}

/**
 * Runs the engine's extension tests, saves what they print and regenerates the matrix from it. With no engine
 * runtime it says "runtime rebuild needed" and exits 0, as the integration job does (amendments 2.7), leaving the
 * committed matrix alone. Returns the exit status.
 */
export function testExtensions(deps: Deps): number {
  const { root, platform, runEngine, log } = deps;
  const skip = (why: string): number => {
    log(`extensions-suite:test-extensions: runtime rebuild needed: ${why} Skipped.`);
    return 0;
  };
  if (platform !== "darwin") return skip("the engine runs only on macOS.");

  const run = runEngine(root);
  if (NO_RUNTIME.test(run.stdout.replaceAll("\n", " "))) {
    return skip(
      "there is no engine runtime; run pnpm chromium:runtime or set BDK_ENGINE_FRAMEWORK.",
    );
  }

  const results = resolve(root, RESULTS_FILE);
  mkdirSync(dirname(results), { recursive: true });
  writeFileSync(results, run.stdout);
  log(`wrote ${RESULTS_FILE}`);

  const parsed = parseResults(run.stdout);
  for (const [path, text] of generatedFiles(root, parsed)) {
    writeFileSync(resolve(root, path), text);
    log(`wrote ${path}`);
  }
  const failed = [...parsed.tests.values()].filter((outcome) => outcome === "fail").length;
  log(`${parsed.tests.size} extension tests, ${failed} failed`);
  return run.status;
}

if (isMain(import.meta.url)) {
  const root = repoRootFromHere(import.meta.url);
  process.exitCode = testExtensions({
    root,
    platform: process.platform,
    runEngine: runScript,
    log: console.log,
  });
}
