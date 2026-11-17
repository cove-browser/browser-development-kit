import { currentBdkPaths } from "../paths.js";
import { isMain, repoRootFromHere, takeValue } from "../runtime/cli.js";
import { modeFromEnv, resolveRuntime } from "../runtime/resolver.js";

const USAGE =
  "usage: runtime.ts [--config release] [--allow-source-build]   (env BDK_RUNTIME_MODE=prebuilt|source)";

/** `chromium:runtime`: resolve, verify and link the runtime for this worktree. Never cached. */
async function main(argv: string[]): Promise<number> {
  let config = "release";
  let allowSourceBuild = false;
  try {
    for (let i = 0; i < argv.length; i++) {
      const a = argv[i];
      if (a === "--config") config = takeValue(argv, i++, a);
      else if (a === "--allow-source-build") allowSourceBuild = true;
      else throw new Error(`unknown argument "${a}"\n${USAGE}`);
    }
    const result = await resolveRuntime({
      repoRoot: repoRootFromHere(import.meta.url),
      paths: currentBdkPaths(),
      config,
      mode: modeFromEnv(process.env),
      allowSourceBuild,
    });
    console.log(`chromium:runtime: ${result.sourceKey} (${result.origin}) -> ${result.runtimeDir}`);
    console.log(`chromium:runtime: manifest sha256 ${result.manifestSha256}`);
    return 0;
  } catch (error) {
    console.error(`chromium:runtime: ${(error as Error).message}`);
    return 1;
  }
}

if (isMain(import.meta.url)) process.exitCode = await main(process.argv.slice(2));
