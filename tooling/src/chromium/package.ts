import { execFileSync } from "node:child_process";
import { existsSync } from "node:fs";
import { isAbsolute, join } from "node:path";
import { fileURLToPath } from "node:url";
import { currentBdkPaths } from "../paths.js";
import { detectBuilder } from "../runtime/builder.js";
import { isMain, repoRootFromHere, takeValue } from "../runtime/cli.js";
import { packageRuntime } from "../runtime/packager.js";
import { readPin } from "../runtime/pin.js";
import { checkReproducible } from "../runtime/reproducible.js";
import { verifyCheckout } from "./package-checkout.js";

const USAGE =
  "usage: package.ts [--config release] [--experiment <id>] [--app-patches <dir>] [--reproducible-check]\n" +
  "  Assembles out/<config> of the managed (or experiment) checkout into runtimes/<source-key>.\n" +
  "  --app-patches: an application's series, applied after BDK's (source mode); the manifest records it.";

/** The slice of lock.ts this command uses (the checkout lock is owned by another module). */
interface LockModule {
  runUnderCheckoutLock(
    checkout: "managed" | { experiment: string },
    argv: string[],
    opts?: { cwd?: string; env?: NodeJS.ProcessEnv },
  ): Promise<number>;
  checkoutRoot(checkout: "managed" | { experiment: string }): string;
}

async function loadLock(): Promise<LockModule> {
  const specifier = "../lock.js";
  try {
    return (await import(specifier)) as LockModule;
  } catch (error) {
    throw new Error(
      `the checkout lock module (tooling/src/lock.ts) is unavailable: ${(error as Error).message}`,
    );
  }
}

function git(args: string[], cwd: string): string {
  return execFileSync("git", args, { cwd, encoding: "utf8" }).trim();
}

async function main(argv: string[]): Promise<number> {
  let config = "release";
  let experiment: string | undefined;
  let reproducibleCheck = false;
  let appPatchesDir: string | undefined;
  try {
    for (let i = 0; i < argv.length; i++) {
      const a = argv[i];
      if (a === "--config") config = takeValue(argv, i++, a);
      else if (a === "--experiment") experiment = takeValue(argv, i++, a);
      else if (a === "--app-patches") {
        // The command re-runs itself under the lock from the repository root, so a relative path
        // would change meaning.
        appPatchesDir = takeValue(argv, i++, a);
        if (!isAbsolute(appPatchesDir)) throw new Error("--app-patches needs an absolute path.");
      } else if (a === "--reproducible-check") reproducibleCheck = true;
      else throw new Error(`unknown argument "${a}"\n${USAGE}`);
    }
    const repoRoot = repoRootFromHere(import.meta.url);
    const lock = await loadLock();
    const checkout = experiment === undefined ? "managed" : { experiment };

    if (process.env.BDK_PACKAGE_LOCKED !== "1") {
      // Reading out/ must not race a build: re-run this command under the checkout lock.
      return await lock.runUnderCheckoutLock(
        checkout,
        ["pnpm", "exec", "tsx", fileURLToPath(import.meta.url), ...argv],
        { cwd: repoRoot, env: { ...process.env, BDK_PACKAGE_LOCKED: "1" } },
      );
    }

    const src = join(lock.checkoutRoot(checkout), "src");
    const outDir = join(src, "out", config);
    if (!existsSync(outDir)) {
      throw new Error(
        `no build output at ${outDir}. Run pnpm chromium:sync and pnpm chromium:build${experiment === undefined ? "" : ` (for experiment ${experiment})`} first.`,
      );
    }
    const options = {
      repoRoot,
      outDir,
      depsFile: join(src, "DEPS"),
      // HEAD is the pin plus the patch series, never the pin: verify that, then name the pin.
      checkoutBase: await verifyCheckout({
        srcDir: src,
        patchesDir: join(repoRoot, "chromium", "patches"),
        ...(appPatchesDir === undefined ? {} : { appPatchesDir }),
        pinCommit: readPin(repoRoot).commit,
      }),
      ...(appPatchesDir === undefined ? {} : { appPatchesDir }),
      config,
      builder: detectBuilder(src),
      repoCommit: git(["rev-parse", "HEAD"], repoRoot),
    };
    if (reproducibleCheck) {
      const report = await checkReproducible(options);
      if (report.reproducible) {
        console.log(
          `chromium:package: reproducible (source key ${report.sourceKey}): identical file digests and manifests apart from builder.timestamp`,
        );
        return 0;
      }
      console.error(
        `chromium:package: NOT reproducible (source key ${report.sourceKey}):\n  ${report.differences.join("\n  ")}`,
      );
      return 1;
    }
    const result = await packageRuntime({ ...options, paths: currentBdkPaths() });
    console.log(`chromium:package: ${result.status} ${result.sourceKey} -> ${result.runtimeDir}`);
    console.log(`chromium:package: manifest sha256 ${result.manifestSha256}`);
    return 0;
  } catch (error) {
    console.error(`chromium:package: ${(error as Error).message}`);
    return 1;
  }
}

if (isMain(import.meta.url)) process.exitCode = await main(process.argv.slice(2));
