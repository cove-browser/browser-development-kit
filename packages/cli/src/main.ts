import { createAssets } from "./assets.js";
import { loadBundler } from "./core/automation/loader.js";
import { resolveConfig } from "./core/config/index.js";
import { ExitCode } from "./core/exit-codes.js";
import { run } from "./core/run.js";
import { cliVersion } from "./core/version.js";
import { selectPlatform } from "./platform/select.js";

/** The executable's entry point: the only file that wires the real process, assets and platform. */
async function main(): Promise<number> {
  return run(process.argv.slice(2), {
    cwd: process.cwd(),
    env: process.env,
    stdout: (text) => process.stdout.write(text),
    stderr: (text) => process.stderr.write(text),
    stderrIsTTY: process.stderr.isTTY === true,
    version: cliVersion(),
    assets: createAssets(),
    platform: selectPlatform(),
    resolveConfig,
    loadBundler,
  });
}

main().then(
  (code) => {
    process.exitCode = code;
  },
  (error: unknown) => {
    process.stderr.write(
      `error: [cli.internal] ${error instanceof Error ? error.message : error}\n`,
    );
    process.exitCode = ExitCode.Environment;
  },
);
