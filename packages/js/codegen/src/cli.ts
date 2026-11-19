import { resolve } from "node:path";
import { check, generate } from "./generate.js";
import { InterfaceError } from "./issue.js";

const USAGE = "usage: bdk-codegen <generate|check> [core-directory]  (default: packages/core)";

function main(argv: readonly string[]): number {
  const [mode, directory = "packages/core"] = argv;
  if (mode !== "generate" && mode !== "check") {
    console.error(USAGE);
    return 2;
  }
  const coreDir = resolve(directory);
  try {
    if (mode === "generate") {
      for (const path of generate(coreDir)) console.log(`wrote ${path}`);
      return 0;
    }
    const stale = check(coreDir);
    if (stale.length === 0) return 0;
    for (const path of stale) console.error(`${path} is out of date`);
    console.error("Regenerate with: pnpm exec nx run js-codegen:codegen");
    return 1;
  } catch (error) {
    if (error instanceof InterfaceError) {
      console.error(error.message);
      return 1;
    }
    throw error;
  }
}

process.exitCode = main(process.argv.slice(2));
