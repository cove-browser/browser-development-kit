import { mkdir, mkdtemp, realpath, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import type { Assets } from "./assets.js";
import { loadBundler } from "./core/automation/loader.js";
import type { Context } from "./core/context.js";
import type { WatchContext } from "./core/watch.js";
import { genericPlatform } from "./platform/generic/index.js";

/** A `Context` that records output, with fakes for everything outside the process. */
export interface TestContext extends Context {
  out: string[];
  err: string[];
}

export function memoryAssets(files: Record<string, string>): Assets {
  return {
    readText(name) {
      const text = files[name];
      if (text === undefined) throw new Error(`no test asset ${name}`);
      return text;
    },
    list: (prefix) =>
      Object.keys(files)
        .filter((name) => name.startsWith(prefix))
        .sort(),
  };
}

export function testContext(overrides: Partial<Context> = {}): TestContext {
  const out: string[] = [];
  const err: string[] = [];
  return {
    cwd: process.cwd(),
    env: {},
    stdout: (text) => out.push(text),
    stderr: (text) => err.push(text),
    stderrIsTTY: false,
    version: "1.2.3",
    assets: memoryAssets({ "bdk.schema.json": '{"type":"object"}' }),
    platform: genericPlatform,
    resolveConfig: () => Promise.resolve({ diagnostics: [], resolved: { ok: true } }),
    loadBundler,
    // A watcher that never fires, and a run that is stopped at once.
    watch: { watcher: () => ({ close() {} }), stopped: () => Promise.resolve(), debounceMs: 0 },
    ...overrides,
    out,
    err,
  };
}

/** A temporary directory with real paths (macOS `/var` is a link to `/private/var`). */
export async function tempDir(prefix = "bdk-cli-test-"): Promise<string> {
  return realpath(await mkdtemp(join(tmpdir(), prefix)));
}

/**
 * A temporary project: a directory holding a valid minimal `bdk.toml`, plus an optional nested
 * directory to start from.
 */
export async function tempProject(nested?: string): Promise<{ dir: string; start: string }> {
  const dir = await tempDir();
  await writeFile(join(dir, "bdk.toml"), '[browser]\nname = "Demo"\n');
  const start = nested === undefined ? dir : join(dir, nested);
  await mkdir(start, { recursive: true });
  return { dir, start };
}

/** A `--watch` context driven by hand: `change()` is a file change, `stop()` is Ctrl-C. */
export interface FakeWatch {
  watch: WatchContext;
  /** The directories being watched now. */
  watched: string[];
  change(): void;
  stop(): void;
}

export function fakeWatch(): FakeWatch {
  const watched: string[] = [];
  const listeners = new Set<() => void>();
  let stop: () => void = () => {};
  const stopped = new Promise<void>((resolve) => {
    stop = resolve;
  });
  return {
    watched,
    change: () => {
      for (const listener of listeners) listener();
    },
    stop,
    watch: {
      debounceMs: 0,
      stopped: () => stopped,
      watcher: (directory, onChange) => {
        watched.push(directory);
        listeners.add(onChange);
        return {
          close: () => {
            watched.splice(watched.indexOf(directory), 1);
            listeners.delete(onChange);
          },
        };
      },
    },
  };
}
