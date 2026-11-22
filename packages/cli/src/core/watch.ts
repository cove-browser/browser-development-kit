import { watch } from "node:fs";

/** A running watch; `close` stops it. */
export interface WatchHandle {
  close(): void;
}

/** Calls `onChange` whenever something under `directory` changes. */
export type Watcher = (directory: string, onChange: () => void) => WatchHandle;

/** What `--watch` needs from outside the process, so tests can supply fakes. `src/main.ts` builds the real one. */
export interface WatchContext {
  watcher: Watcher;
  /** Settles when the user asks to stop (Ctrl-C). Called once, when watching begins. */
  stopped: () => Promise<void>;
  /** How long changes must be quiet before a rebuild starts. */
  debounceMs: number;
}

/** Watches with `node:fs`, which reports changes at any depth on macOS, Windows and Linux. */
export const fsWatcher: Watcher = (directory, onChange) => {
  const watcher = watch(directory, { recursive: true }, () => onChange());
  return { close: () => watcher.close() };
};

/** A promise that settles on the first SIGINT. */
export function untilInterrupted(): Promise<void> {
  return new Promise((resolve) => process.once("SIGINT", () => resolve()));
}

/**
 * Runs `rebuild` each time something under `directory` changes, until `watch.stopped()` settles.
 * Changes are debounced, and one that arrives during a rebuild starts another when it ends, so the
 * last change is never missed and two rebuilds never overlap. `rebuild` reports its own failures; the
 * loop keeps watching either way. Shared by every step that has a `--watch` (sdk/cli.md section 5).
 */
export async function watchLoop(options: {
  watch: WatchContext;
  directory: string;
  rebuild: () => Promise<void>;
}): Promise<void> {
  const { watch: context, directory, rebuild } = options;
  let stopping = false;
  let timer: NodeJS.Timeout | undefined;
  let running: Promise<void> | undefined;
  let again = false;

  const start = (): void => {
    if (stopping) return;
    if (running !== undefined) {
      again = true;
      return;
    }
    running = (async () => {
      do {
        again = false;
        await rebuild();
      } while (again && !stopping);
      running = undefined;
    })();
  };
  const handle = context.watcher(directory, () => {
    clearTimeout(timer);
    timer = setTimeout(start, context.debounceMs);
  });
  try {
    await context.stopped();
  } finally {
    stopping = true;
    clearTimeout(timer);
    handle.close();
    await running;
  }
}
