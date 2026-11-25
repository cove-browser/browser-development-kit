import type { BdkPaths } from "@bdk/tooling/dist/paths.js";
import type { Assets } from "../assets.js";
import type { Platform } from "../platform/platform.js";
import type { Bundler } from "./automation/bundler.js";
import type { resolveConfig } from "./config/index.js";
import type { WatchContext } from "./watch.js";

/** What a program run by `exec` left behind. `code` is null when it was killed by a signal. */
export interface ExecResult {
  code: number | null;
  stdout: string;
  stderr: string;
}

/**
 * Runs a program to completion without a shell (`git init`, a package manager's install). A program
 * that does not exist gives code 127, as a shell would. Never throws on a non-zero exit.
 */
export type Exec = (
  command: string,
  args: readonly string[],
  options: { cwd: string; env?: NodeJS.ProcessEnv },
) => Promise<ExecResult>;

/** One question of `bdk init`. */
export interface Question {
  /** The prompt, without the default or the choices. */
  message: string;
  /** Shown in brackets and taken for an empty answer. */
  default?: string;
  /** The valid answers, shown after the prompt. */
  choices?: readonly string[];
}

/** Asks the person at the terminal; `bdk init` is the only command that asks (sdk/cli.md section 3). */
export interface Prompter {
  ask(question: Question): Promise<string>;
}

/** Everything a run needs from outside, so tests can supply fakes. `src/main.ts` builds the real one. */
export interface Context {
  cwd: string;
  env: NodeJS.ProcessEnv;
  stdout: (text: string) => void;
  stderr: (text: string) => void;
  /** Whether standard error is a terminal: colour is allowed only then. */
  stderrIsTTY: boolean;
  version: string;
  assets: Assets;
  platform: Platform;
  resolveConfig: typeof resolveConfig;
  /** Rolldown, from the project or the CLI's own dependency; undefined when neither has it. */
  loadBundler: (projectDir: string) => Promise<Bundler | undefined>;
  /** For `--watch`: the file watcher, the stop signal and the debounce. */
  watch: WatchContext;
  /** Runs other programs: `git` and the package manager for `bdk init`. */
  exec: Exec;
  /** Questions for `bdk init`; undefined when standard input is not a terminal. */
  prompter?: Prompter;
  /**
   * Runs `bdk <args>` again as a child that holds the managed Chromium checkout's lock, with
   * `BDK_CHECKOUT_LOCKED=1` set, and returns its exit code (`bdk patch`). Throws the tooling's
   * `LockBusyError` when another process holds the lock.
   */
  underCheckoutLock: (paths: BdkPaths, args: readonly string[]) => Promise<number>;
  /** The packaged `bdk` executable that is running, or undefined when the sources run under Node. */
  executable?: string;
}
