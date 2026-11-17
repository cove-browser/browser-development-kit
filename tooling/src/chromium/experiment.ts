import { existsSync, mkdirSync, readdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { dirname, join, relative } from "node:path";
import { fileURLToPath } from "node:url";
import { parseArgs } from "node:util";
import { git, run } from "../git.js";
import {
  type CheckoutId,
  LockBusyError,
  probeCheckoutLock,
  runUnderCheckoutLock,
} from "../lock.js";
import { type BdkPaths, currentBdkPaths } from "../paths.js";
import {
  checkoutContext,
  chromiumLayout,
  fail,
  isMain,
  LOCKED_FLAG,
  loadSeries,
  readPin,
  relaunchUnderLock,
} from "./common.js";
import { assertSafeToSync, UnsafeCheckoutError } from "./patch-apply.js";

export type ExperimentMethod = "clone" | "worktree";

export interface ExperimentMeta {
  readonly id: string;
  readonly from: string;
  readonly method: ExperimentMethod;
  readonly created: string;
  readonly pin: string;
}

const META = ".bdk-experiment.json";
const EXP_LOCKED_FLAG = "--exp-locked";

function sourceCheckout(from: string): CheckoutId {
  return from === "managed" ? "managed" : { experiment: from };
}

export function readMeta(root: string): ExperimentMeta | undefined {
  try {
    return JSON.parse(readFileSync(join(root, META), "utf8")) as ExperimentMeta;
  } catch {
    return undefined;
  }
}

/**
 * `cp -c` (clonefile) needs APFS and a single volume. Probe it with a real file copy from the
 * source tree into the destination's parent, so the answer is about this pair of directories.
 */
export async function assertCanClone(fromRoot: string, destParent: string): Promise<void> {
  if (process.platform !== "darwin") {
    throw new Error(
      "--method clone uses APFS clones (cp -c), which exist only on macOS. Use --method worktree.",
    );
  }
  mkdirSync(destParent, { recursive: true });
  const a = join(fromRoot, ".bdk-clone-probe");
  const b = join(destParent, ".bdk-clone-probe");
  writeFileSync(a, "probe");
  try {
    const r = await run("cp", ["-c", a, b]);
    if (r.code !== 0) {
      throw new Error(
        `Cannot APFS-clone from ${fromRoot} to ${destParent}: ${r.stderr.trim() || "cp -c failed"}. ` +
          "Both must be on the same APFS volume. Use --method worktree instead, or move BDK_HOME.",
      );
    }
  } finally {
    rmSync(a, { force: true });
    rmSync(b, { force: true });
  }
}

/** Clones the source tree (never `out/`) with copy-on-write. */
export async function cloneCheckoutTree(fromRoot: string, destRoot: string): Promise<void> {
  await assertCanClone(fromRoot, dirname(destRoot));
  mkdirSync(join(destRoot, "src"), { recursive: true });
  const top = readdirSync(fromRoot).filter(
    (n) => n !== "src" && n !== META && n !== ".bdk-overlay.json",
  );
  if (top.length > 0) {
    const r = await run("cp", ["-cR", ...top.map((n) => join(fromRoot, n)), destRoot]);
    if (r.code !== 0) throw new Error(`cp -cR failed: ${r.stderr.trim()}`);
  }
  const entries = readdirSync(join(fromRoot, "src")).filter((n) => n !== "out");
  if (entries.length > 0) {
    const r = await run("cp", [
      "-cR",
      ...entries.map((n) => join(fromRoot, "src", n)),
      join(destRoot, "src"),
    ]);
    if (r.code !== 0) throw new Error(`cp -cR failed: ${r.stderr.trim()}`);
  }
}

export interface CreateOptions {
  readonly id: string;
  readonly from: string;
  readonly method: ExperimentMethod;
  readonly paths?: BdkPaths;
  readonly log?: (line: string) => void;
}

/**
 * Creates `chromium/exp/<id>`. The caller holds the source checkout's lock (the tree must not
 * change while it is cloned) and the experiment's own lock.
 */
export async function createExperiment(opts: CreateOptions): Promise<ExperimentMeta> {
  const log = opts.log ?? ((l: string) => console.log(l));
  const paths = opts.paths ?? currentBdkPaths();
  const layout = chromiumLayout();
  const dest = paths.chromiumExperiment(opts.id);
  const fromCtx = checkoutContext(sourceCheckout(opts.from), paths, layout);
  if (existsSync(dest)) throw new Error(`Experiment "${opts.id}" already exists at ${dest}`);
  if (!existsSync(join(fromCtx.src, ".git")))
    throw new Error(`Source checkout ${fromCtx.root} has no src/.git; run chromium:sync first.`);
  mkdirSync(dirname(dest), { recursive: true });
  const started = Date.now();
  try {
    if (opts.method === "clone") {
      log(`experiment: APFS-cloning ${fromCtx.root} (without out/) to ${dest}`);
      await cloneCheckoutTree(fromCtx.root, dest);
    } else {
      const script = join(fromCtx.src, "tools", "git", "worktree.py");
      if (!existsSync(script))
        throw new Error(`${script} not found; the source checkout is incomplete.`);
      log(`experiment: git worktree via ${script}`);
      const r = await run(
        "python3",
        [
          script,
          fromCtx.root,
          "--name",
          relative(dirname(fromCtx.root), dest),
          "--branch",
          `bdk-exp-${opts.id}`,
          "--light",
        ],
        { inherit: true },
      );
      if (r.code !== 0) throw new Error(`worktree.py failed (exit ${r.code})`);
      log(
        "experiment: the worktree has no third-party dependencies yet; run chromium:sync --experiment " +
          opts.id,
      );
    }
    const meta: ExperimentMeta = {
      id: opts.id,
      from: opts.from,
      method: opts.method,
      created: new Date().toISOString(),
      pin: readPin(layout).commit,
    };
    writeFileSync(join(dest, META), `${JSON.stringify(meta, null, 2)}\n`);
    log(`experiment: created ${dest} in ${((Date.now() - started) / 1000).toFixed(1)}s`);
    return meta;
  } catch (e) {
    rmSync(dest, { recursive: true, force: true });
    throw e;
  }
}

/** Removes an experiment. The caller holds its lock. */
export async function removeExperiment(
  id: string,
  paths: BdkPaths = currentBdkPaths(),
  log: (l: string) => void = console.log,
  force = false,
): Promise<void> {
  const dest = paths.chromiumExperiment(id);
  if (!existsSync(dest)) throw new Error(`Experiment "${id}" does not exist (${dest})`);
  const meta = readMeta(dest);
  const src = join(dest, "src");
  if (!force && existsSync(join(src, ".git"))) {
    if (!meta?.pin) {
      throw new UnsafeCheckoutError(
        `${src} has no recorded pin, so unsaved work cannot be ruled out. Pass --force to remove it anyway.`,
      );
    }
    const layout = chromiumLayout();
    await assertSafeToSync(src, meta.pin, loadSeries(layout.patchesDir).patches, {
      action: "removing the experiment",
      exportHint: `chromium:patch-export --experiment ${id}`,
    });
  }
  if (meta?.method === "worktree") {
    const from = checkoutContext(sourceCheckout(meta.from), paths, chromiumLayout());
    if (existsSync(join(from.src, ".git"))) {
      await git(from.src, ["worktree", "remove", "--force", join(dest, "src")]).catch(
        () => undefined,
      );
      await git(from.src, ["branch", "-D", `bdk-exp-${id}`]).catch(() => undefined);
    }
  }
  rmSync(dest, { recursive: true, force: true });
  log(`experiment: removed ${dest}`);
}

export interface ExperimentListing extends ExperimentMeta {
  readonly path: string;
  readonly locked: boolean;
  readonly owner?: { pid: number; command: string; worktree: string };
}

export async function listExperiments(
  paths: BdkPaths = currentBdkPaths(),
): Promise<ExperimentListing[]> {
  const dir = join(paths.home, "chromium", "exp");
  if (!existsSync(dir)) return [];
  const out: ExperimentListing[] = [];
  for (const id of readdirSync(dir).sort()) {
    const root = join(dir, id);
    const meta = readMeta(root) ?? {
      id,
      from: "?",
      method: "clone" as const,
      created: "",
      pin: "",
    };
    const probe = await probeCheckoutLock({ experiment: id }, paths);
    out.push({
      ...meta,
      path: root,
      locked: probe.locked,
      ...(probe.locked && probe.owner
        ? {
            owner: {
              pid: probe.owner.pid,
              command: probe.owner.command,
              worktree: probe.owner.worktree,
            },
          }
        : {}),
    });
  }
  return out;
}

function reexecArgs(extra: string[]): string[] {
  return [
    process.execPath,
    ...process.execArgv,
    fileURLToPath(import.meta.url),
    ...process.argv.slice(2),
    ...extra,
  ];
}

async function main(): Promise<void> {
  const args = process.argv.slice(2);
  const { values, positionals } = parseArgs({
    args,
    allowPositionals: true,
    options: {
      from: { type: "string", default: "managed" },
      method: { type: "string", default: "clone" },
      json: { type: "boolean", default: false },
      force: { type: "boolean", default: false },
      [LOCKED_FLAG.slice(2)]: { type: "boolean", default: false },
      [EXP_LOCKED_FLAG.slice(2)]: { type: "boolean", default: false },
    },
    strict: true,
  });
  const [cmd, id] = positionals;
  const paths = currentBdkPaths();
  try {
    if (cmd === "list") {
      const list = await listExperiments(paths);
      if (values.json) console.log(JSON.stringify(list, null, 2));
      else if (list.length === 0) console.log("no experiments");
      else {
        for (const e of list) {
          console.log(
            `${e.id}\t${e.method}\tfrom ${e.from}\t${e.created}\t${e.locked ? `LOCKED${e.owner ? ` by pid ${e.owner.pid}` : ""}` : "idle"}\t${e.path}`,
          );
        }
      }
      return;
    }
    if (!id || (cmd !== "create" && cmd !== "remove")) {
      fail(
        "usage: chromium:experiment create <id> [--from managed|<id>] [--method clone|worktree]\n       chromium:experiment remove <id>\n       chromium:experiment list [--json]",
        2,
      );
    }
    paths.chromiumExperiment(id); // validates the id
    if (cmd === "create") {
      if (values.method !== "clone" && values.method !== "worktree")
        fail(`--method must be clone or worktree, got "${values.method}"`, 2);
      const from = values.from as string;
      if (!values.locked)
        process.exit(await relaunchUnderLock(sourceCheckout(from), import.meta.url, args, paths));
      if (!values["exp-locked"]) {
        // Source lock held; now take the new experiment's own lock for its creation.
        process.exit(
          await runUnderCheckoutLock({ experiment: id }, reexecArgs([EXP_LOCKED_FLAG]), { paths }),
        );
      }
      await createExperiment({ id, from, method: values.method as ExperimentMethod, paths });
    } else {
      if (!values.locked)
        process.exit(await relaunchUnderLock({ experiment: id }, import.meta.url, args, paths));
      await removeExperiment(id, paths, console.log, values.force);
    }
  } catch (e) {
    if (e instanceof LockBusyError) fail(e.message, 75);
    fail(e instanceof Error ? e.message : String(e));
  }
}

if (isMain(import.meta.url)) await main();
