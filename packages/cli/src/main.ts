import { createInterface } from "node:readline/promises";
import { run as runProgram } from "@bdk/tooling/dist/git.js";
import { runUnderCheckoutLock } from "@bdk/tooling/dist/lock.js";
import { createAssets, isPackaged } from "./assets.js";
import { loadBundler } from "./core/automation/loader.js";
import { resolveConfig } from "./core/config/index.js";
import type { Exec, Prompter } from "./core/context.js";
import { ExitCode } from "./core/exit-codes.js";
import { formatQuestion } from "./core/init/prompts.js";
import { run } from "./core/run.js";
import { cliVersion } from "./core/version.js";
import { fsWatcher, untilInterrupted } from "./core/watch.js";
import { selectPlatform } from "./platform/select.js";

/** Runs a program without a shell; one that cannot be started gives 127, as a shell would. */
const exec: Exec = async (command, args, options) => {
  try {
    return await runProgram(command, args, { cwd: options.cwd, env: options.env ?? process.env });
  } catch (error) {
    return {
      code: 127,
      stdout: "",
      stderr: error instanceof Error ? error.message : String(error),
    };
  }
};

/** Questions on the terminal, only when standard input is one. */
function terminalPrompter(): Prompter | undefined {
  if (process.stdin.isTTY !== true) return undefined;
  return {
    async ask(question) {
      const terminal = createInterface({ input: process.stdin, output: process.stderr });
      try {
        return (await terminal.question(formatQuestion(question))).trim();
      } finally {
        terminal.close();
      }
    },
  };
}

/** The command line that starts this `bdk` again: the executable, or Node with its loader and the script. */
function selfCommand(): string[] {
  if (isPackaged()) return [process.execPath];
  return [process.execPath, ...process.execArgv, process.argv[1] ?? ""];
}

/** The executable's entry point: the only file that wires the real process, assets and platform. */
async function main(): Promise<number> {
  const prompter = terminalPrompter();
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
    watch: { watcher: fsWatcher, stopped: untilInterrupted, debounceMs: 100 },
    exec,
    ...(prompter === undefined ? {} : { prompter }),
    underCheckoutLock: (paths, args) =>
      runUnderCheckoutLock("managed", [...selfCommand(), ...args], {
        paths,
        env: { ...process.env, BDK_CHECKOUT_LOCKED: "1" },
      }),
    ...(isPackaged() ? { executable: process.execPath } : {}),
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
