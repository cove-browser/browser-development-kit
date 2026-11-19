import { currentBdkPaths } from "../paths.js";
import { isMain, repoRootFromHere } from "../runtime/cli.js";
import { modeFromEnv, RuntimeResolveError, resolveRuntime } from "../runtime/resolver.js";

/**
 * Part of `pnpm test:release` (chromium:test-release): resolves the runtime and, when one is present,
 * is where the ABI conformance suite runs against it. The suite itself is future work (it needs the
 * Swift/C harness from M2), so a present runtime is reported and the suite step says it is not wired.
 * A missing runtime is a skip; a damaged one is a failure.
 */
async function main(): Promise<number> {
  try {
    const r = await resolveRuntime({
      repoRoot: repoRootFromHere(import.meta.url),
      paths: currentBdkPaths(),
      mode: modeFromEnv(process.env),
    });
    console.log(`conformance: runtime ${r.sourceKey} verified at ${r.runtimeDir}`);
    console.log(
      "conformance: SKIPPED the ABI conformance suite against the real runtime: not implemented yet (M2).",
    );
    return 0;
  } catch (error) {
    if (error instanceof RuntimeResolveError && error.code === "missing") {
      console.log(
        "conformance: SKIPPED: no runtime for this source key (nothing is published yet).",
      );
      console.log(error.message);
      return 0;
    }
    console.error(`conformance: ${(error as Error).message}`);
    return 1;
  }
}

if (isMain(import.meta.url)) process.exitCode = await main();
