import { mkdir, mkdtemp, realpath, symlink, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { type Manifest, manifestDigest, scanFiles } from "@bdk/tooling/dist/runtime/manifest.js";
import { type Assets, createAssets } from "./assets.js";
import { loadBundler } from "./core/automation/loader.js";
import type { Context } from "./core/context.js";
import { RELEASE_ASSET, type ReleaseFacts } from "./core/doctor/release.js";
import { run } from "./core/run.js";
import type { WatchContext } from "./core/watch.js";
import { genericPlatform } from "./platform/generic/index.js";

/** A `Context` that records output, with fakes for everything outside the process. */
export interface TestContext extends Context {
  out: string[];
  err: string[];
  /** Every program `exec` was asked to run; the fake runs none and reports success. */
  execCalls: { command: string; args: readonly string[]; cwd: string }[];
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
  const execCalls: TestContext["execCalls"] = [];
  const ctx: TestContext = {
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
    exec: (command, args, options) => {
      execCalls.push({ command, args, cwd: options.cwd });
      return Promise.resolve({ code: 0, stdout: "", stderr: "" });
    },
    // The lock is taken by the tooling's own tests; here the locked run happens in this process.
    underCheckoutLock: (_paths, args) =>
      run(args, { ...ctx, env: { ...ctx.env, BDK_CHECKOUT_LOCKED: "1" } }),
    ...overrides,
    out,
    err,
    execCalls,
  };
  return ctx;
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

/** The store entry the fixtures resolve, and the project's pointer to it (`.bdk/runtime.json`). */
export const STORE_ENTRY = "fixture-runtime";
export const FIXTURE_RUNTIME_POINTER = JSON.stringify({ source_key: STORE_ENTRY });

/**
 * A runtime in the test's store that verifies against its manifest (by default it has no files besides the
 * manifest), resolved by the project. By default it is this BDK's pin and ABI. Returns the store directory.
 */
export async function storeRuntime(
  dir: string,
  over: {
    abi?: object;
    chromium?: { tag: string; commit: string };
    minOs?: string;
    /** Files of the runtime, by path, with their contents. */
    files?: Record<string, string>;
    /** Symbolic links of the runtime, by path, with their targets. */
    links?: Record<string, string>;
    /** The store key (default `STORE_ENTRY`). */
    key?: string;
  },
): Promise<string> {
  const release = JSON.parse(createAssets({ packaged: false }).readText("bdk-release.json"));
  const key = over.key ?? STORE_ENTRY;
  const store = join(dir, ".home", "runtimes", key);
  await mkdir(store, { recursive: true });
  for (const [path, content] of Object.entries(over.files ?? {})) {
    await mkdir(dirname(join(store, path)), { recursive: true });
    await writeFile(join(store, path), content);
  }
  for (const [path, target] of Object.entries(over.links ?? {})) {
    await mkdir(dirname(join(store, path)), { recursive: true });
    await symlink(target, join(store, path));
  }
  const files = await scanFiles(store);
  const manifest = {
    schema: 1,
    source_key: key,
    bdk_abi: over.abi ?? release.abi,
    chromium: { ...(over.chromium ?? release.chromium), deps_digest: "d" },
    target: { os: "macos", arch: "arm64", min_os: over.minOs ?? "14.0" },
    files,
  };
  await writeFile(join(store, "manifest.json"), JSON.stringify(manifest));
  await mkdir(join(dir, ".bdk"), { recursive: true });
  await writeFile(
    join(dir, ".bdk", "runtime.json"),
    JSON.stringify({ source_key: key, manifest_sha256: manifestDigest(manifest as Manifest) }),
  );
  return store;
}

/** The release facts of this repository, with `runtime` replaced: the prebuilt runtime a release publishes. */
export function withPublishedRuntime(runtime: ReleaseFacts["runtime"]): Assets {
  const assets = createAssets({ packaged: false });
  return {
    ...assets,
    readText: (name) =>
      name === RELEASE_ASSET
        ? JSON.stringify({ ...JSON.parse(assets.readText(name)), runtime })
        : assets.readText(name),
  };
}
