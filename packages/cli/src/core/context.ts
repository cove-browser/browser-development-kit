import type { Assets } from "../assets.js";
import type { Platform } from "../platform/platform.js";
import type { Bundler } from "./automation/bundler.js";
import type { resolveConfig } from "./config/index.js";
import type { WatchContext } from "./watch.js";

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
}
