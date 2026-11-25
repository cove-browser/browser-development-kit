import { isMain, takeValue } from "../runtime/cli.js";
import { compareManifests, type FilesMode, loadManifest } from "../runtime/compare.js";

const USAGE =
  "usage: compare-runtimes.ts <runtime dir or manifest> <runtime dir or manifest> [--files strict|paths]\n" +
  "  Two builds from the same inputs must report the same ABI hash and equivalent manifests (T-CHR-09).\n" +
  "  --files paths (default) compares the inventory only; strict also compares every file digest.";

/** `pnpm exec nx run chromium:compare-runtimes -- <a> <b>`. Reads two manifests; builds nothing. */
export async function main(argv: string[]): Promise<number> {
  const positional: string[] = [];
  let files: FilesMode = "paths";
  try {
    for (let i = 0; i < argv.length; i++) {
      const a = argv[i] as string;
      if (a === "--files") {
        const v = takeValue(argv, i++, a);
        if (v !== "strict" && v !== "paths")
          throw new Error(`--files must be strict or paths\n${USAGE}`);
        files = v;
      } else if (a.startsWith("--")) throw new Error(`unknown argument "${a}"\n${USAGE}`);
      else positional.push(a);
    }
    const [first, second] = positional;
    if (first === undefined || second === undefined || positional.length !== 2)
      throw new Error(USAGE);
    const r = compareManifests(await loadManifest(first), await loadManifest(second), files);
    console.log(`compare-runtimes: source key ${r.sourceKey.a} vs ${r.sourceKey.b}`);
    console.log(`compare-runtimes: ABI hash   ${r.abiHash.a} vs ${r.abiHash.b}`);
    for (const n of r.notes) console.log(`compare-runtimes: note: ${n}`);
    if (r.equivalent) {
      console.log(`compare-runtimes: equivalent (files: ${files})`);
      return 0;
    }
    console.error(`compare-runtimes: NOT equivalent:\n  ${r.differences.join("\n  ")}`);
    return 1;
  } catch (error) {
    console.error(`compare-runtimes: ${(error as Error).message}`);
    return 2;
  }
}

if (isMain(import.meta.url)) process.exitCode = await main(process.argv.slice(2));
