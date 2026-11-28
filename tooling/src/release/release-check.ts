import { isMain, repoRootFromHere, takeValue } from "../runtime/cli.js";
import { type CheckResult, releaseCheck } from "./checks.js";

const USAGE =
  "usage: release-check.ts [--version X.Y.Z] [--swift-archive <zip>] [--skip-swift-resolve]\n" +
  "  The release dry run (docs/release/procedure.md): clean tree, tag, pin record, ABI hash, source key,\n" +
  "  the published runtime's digests (fetched into a scratch store), notices, and the Swift package.\n" +
  "  Builds, signs and uploads nothing.";

export function render(results: readonly CheckResult[]): string {
  const width = Math.max(...results.map((r) => r.name.length));
  return results
    .map((r) => `${r.status.toUpperCase().padEnd(4)}  ${r.name.padEnd(width)}  ${r.detail}`)
    .join("\n");
}

async function main(argv: string[]): Promise<number> {
  let version: string | undefined;
  let swiftArchive: string | undefined;
  let skipSwiftResolve = false;
  try {
    for (let i = 0; i < argv.length; i++) {
      const a = argv[i];
      if (a === "--version") version = takeValue(argv, i++, a);
      else if (a === "--swift-archive") swiftArchive = takeValue(argv, i++, a);
      else if (a === "--skip-swift-resolve") skipSwiftResolve = true;
      else throw new Error(`unknown argument "${a}"\n${USAGE}`);
    }
    const results = await releaseCheck({
      repoRoot: repoRootFromHere(import.meta.url),
      ...(version === undefined ? {} : { version }),
      ...(swiftArchive === undefined ? {} : { swiftArchive }),
      skipSwiftResolve,
    });
    console.log(render(results));
    const failed = results.filter((r) => r.status === "fail").length;
    console.log(
      failed === 0
        ? "release-check: ready to sign, notarize and publish (docs/release/procedure.md)"
        : `release-check: ${failed} check${failed === 1 ? "" : "s"} failed; this tree cannot be released`,
    );
    return failed === 0 ? 0 : 1;
  } catch (error) {
    console.error(`release-check: ${(error as Error).message}`);
    return 2;
  }
}

if (isMain(import.meta.url)) process.exitCode = await main(process.argv.slice(2));
