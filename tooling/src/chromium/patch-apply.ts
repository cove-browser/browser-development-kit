import { existsSync } from "node:fs";
import { isAbsolute, join } from "node:path";
import { parseArgs } from "node:util";
import { bdkIdentityArgs, git, gitRaw } from "../git.js";
import {
  type CheckoutContext,
  checkoutContext,
  checkoutFromFlag,
  chromiumLayout,
  fail,
  isMain,
  LOCKED_FLAG,
  loadSeries,
  type PatchFile,
  readPin,
  relaunchUnderLock,
} from "./common.js";
import { patchIdOf, patchIdsForCommits } from "./patch-export.js";

/** A patch did not apply. `files` names every conflicting (or missing) file. */
export class PatchConflictError extends Error {
  override readonly name = "PatchConflictError";
  constructor(
    readonly patch: string,
    readonly files: readonly string[],
    readonly detail: string,
  ) {
    super(
      `Patch ${patch} does not apply. Conflicting files:\n${
        files.length > 0
          ? files.map((f) => `  ${f}`).join("\n")
          : "  (git reported no file; see below)"
      }\n${detail.trim()}`,
    );
  }
}

/** The checkout cannot be touched safely (dirty tree, unexported commits, not at the pin). */
export class UnsafeCheckoutError extends Error {
  override readonly name = "UnsafeCheckoutError";
}

export interface AmResult {
  readonly ok: boolean;
  readonly files: readonly string[];
  readonly output: string;
}

/**
 * `git am --3way` for one patch. On failure the `git am` is aborted (the tree returns to its state
 * before this patch) and the conflicting files are returned: unmerged paths plus every file git
 * said it could not patch.
 */
export async function amPatch(srcDir: string, patchPath: string): Promise<AmResult> {
  const r = await gitRaw(srcDir, [
    ...bdkIdentityArgs,
    "am",
    "--3way",
    "--whitespace=nowarn",
    "--committer-date-is-author-date",
    patchPath,
  ]);
  if (r.code === 0) return { ok: true, files: [], output: r.stdout };
  const output = `${r.stdout}${r.stderr}`;
  const files = new Set<string>();
  const unmerged = await gitRaw(srcDir, ["diff", "--name-only", "--diff-filter=U"]);
  for (const f of unmerged.stdout.split("\n")) if (f) files.add(f);
  for (const line of output.split("\n")) {
    const m =
      /^error: patch failed: (.+?):\d+$/.exec(line) ??
      /^error: (.+?): (?:does not exist in index|patch does not apply|already exists in working directory)$/.exec(
        line,
      ) ??
      /^CONFLICT \(.+?\): Merge conflict in (.+)$/.exec(line) ??
      /^CONFLICT \(.+?\): (\S+) (?:deleted|renamed|added) /.exec(line);
    if (m?.[1]) files.add(m[1]);
  }
  // --abort resets HEAD and writes the reflog, which needs an identity; CI runners have none configured.
  await gitRaw(srcDir, [...bdkIdentityArgs, "am", "--abort"]);
  return { ok: false, files: [...files].sort(), output };
}

async function amInProgress(srcDir: string): Promise<boolean> {
  const dir = (await git(srcDir, ["rev-parse", "--git-path", "rebase-apply"])).trim();
  return existsSync(isAbsolute(dir) ? dir : join(srcDir, dir));
}

export interface SeriesState {
  readonly head: string;
  readonly dirty: readonly string[];
  /** HEAD is the pin plus exactly the series, no local edits. */
  readonly applied: boolean;
  /** Commits between the pin and HEAD (empty if HEAD is not a descendant of the pin). */
  readonly extraCommits: number;
  readonly atOrAbovePin: boolean;
}

/** Inspects `src`: where HEAD is relative to the pin and whether it equals the series. */
export async function seriesState(
  srcDir: string,
  pinCommit: string,
  patches: readonly PatchFile[],
): Promise<SeriesState> {
  const head = (await git(srcDir, ["rev-parse", "HEAD"])).trim();
  const dirty = (await git(srcDir, ["status", "--porcelain", "--untracked-files=no"]))
    .split("\n")
    .filter(Boolean);
  const pinKnown = (await gitRaw(srcDir, ["cat-file", "-e", `${pinCommit}^{commit}`])).code === 0;
  const atOrAbovePin =
    pinKnown &&
    (await gitRaw(srcDir, ["merge-base", "--is-ancestor", pinCommit, "HEAD"])).code === 0;
  let applied = false;
  let extraCommits = 0;
  if (atOrAbovePin) {
    const commits = (await git(srcDir, ["rev-list", "--reverse", `${pinCommit}..HEAD`]))
      .split("\n")
      .filter(Boolean);
    extraCommits = commits.length;
    if (dirty.length === 0 && commits.length === patches.length) {
      const have = await patchIdsForCommits(srcDir, commits);
      const want = await Promise.all(patches.map((p) => patchIdOf(srcDir, p.text)));
      applied = have.every((id, i) => id === want[i]);
    }
  }
  return { head, dirty, applied, extraCommits, atOrAbovePin };
}

/**
 * Per-worktree refs (they live in `.git/refs/worktree/`, so `gclient`, `git am` and `patch-export`
 * ignore them, an APFS clone keeps them, and a `git worktree` experiment does not share them with
 * its source): the pin BDK last put `src` on, and the HEAD it left after applying the series.
 */
export const BASE_REF = "refs/worktree/bdk/base";
export const APPLIED_REF = "refs/worktree/bdk/applied";

async function readRef(srcDir: string, ref: string): Promise<string | undefined> {
  const r = await gitRaw(srcDir, ["rev-parse", "--verify", "--quiet", `${ref}^{commit}`]);
  return r.code === 0 ? r.stdout.trim() : undefined;
}

/** Records the base and resulting HEAD that BDK produced. Call only with HEAD it just produced. */
export async function recordBdkState(srcDir: string, base: string, applied: string): Promise<void> {
  await git(srcDir, ["update-ref", BASE_REF, base]);
  await git(srcDir, ["update-ref", APPLIED_REF, applied]);
}

/**
 * Refuses (UnsafeCheckoutError) if `gclient sync --force` would discard work: any uncommitted
 * change to a tracked file, or a HEAD that BDK did not produce. HEAD is BDK's when it is the
 * current pin, the recorded applied/base commit, or the recorded base plus exactly the current
 * series. Without recorded refs (an older checkout) it falls back to "pin plus exactly the series
 * or nothing" measured against the current pin.
 */
export async function assertSafeToSync(
  srcDir: string,
  pinCommit: string,
  patches: readonly PatchFile[],
  what: { action?: string; exportHint?: string } = {},
): Promise<void> {
  const state = await seriesState(srcDir, pinCommit, patches);
  const refuse = async (why: string): Promise<never> => {
    const recent = (
      await gitRaw(srcDir, ["log", "-5", "--format=  %h %s", `${pinCommit}..HEAD`])
    ).stdout.trim();
    throw new UnsafeCheckoutError(
      `${srcDir} ${why}; ${what.action ?? "gclient sync --force"} would discard it.` +
        `${recent ? `\nCommits on top of the pin:\n${recent}` : ""}\n` +
        `Run ${what.exportHint ?? "chromium:patch-export"} to save it, or pass --force.`,
    );
  };
  if (state.dirty.length > 0) {
    await refuse(
      `has uncommitted changes:\n${state.dirty
        .slice(0, 20)
        .map((l) => `  ${l}`)
        .join("\n")}\n`,
    );
  }
  if (state.head === pinCommit || state.applied) return;
  const base = await readRef(srcDir, BASE_REF);
  const applied = await readRef(srcDir, APPLIED_REF);
  if (base !== undefined && applied !== undefined) {
    if (state.head === applied || state.head === base) return;
    if ((await seriesState(srcDir, base, patches)).applied) return;
    await refuse(
      `is at ${state.head.slice(0, 12)}, which BDK did not produce (it left ${applied.slice(0, 12)})`,
    );
  }
  if (state.extraCommits > 0) {
    await refuse(
      `has ${state.extraCommits} commit(s) on top of the pin that are not the current series`,
    );
  }
  if (!state.atOrAbovePin && !(await isUpstreamCommit(srcDir, state.head, pinCommit))) {
    await refuse(
      `is at ${state.head.slice(0, 12)}, which is not descended from the pin, not upstream history, and not a state BDK recorded`,
    );
  }
}

/**
 * True when `commit` holds no local work: it is an ancestor of the pin, or it is reachable from
 * an upstream remote-tracking branch or tag.
 */
async function isUpstreamCommit(
  srcDir: string,
  commit: string,
  pinCommit: string,
): Promise<boolean> {
  const pinKnown = (await gitRaw(srcDir, ["cat-file", "-e", `${pinCommit}^{commit}`])).code === 0;
  if (
    pinKnown &&
    (await gitRaw(srcDir, ["merge-base", "--is-ancestor", commit, pinCommit])).code === 0
  )
    return true;
  const r = await gitRaw(srcDir, [
    "for-each-ref",
    "--count=1",
    "--format=%(refname)",
    `--contains=${commit}`,
    "refs/remotes",
    "refs/tags",
  ]);
  return r.code === 0 && r.stdout.trim() !== "";
}

export interface ApplyOptions {
  readonly srcDir: string;
  readonly pinCommit: string;
  readonly patchesDir: string;
  /** Discard uncommitted changes and commits that are not in the series. */
  readonly force?: boolean;
  readonly log?: (line: string) => void;
}

export interface ApplyResult {
  /** `already-applied` leaves the tree untouched, so file mtimes (and incremental builds) are kept. */
  readonly status: "applied" | "already-applied";
  readonly patches: readonly string[];
  readonly head: string;
}

/**
 * Resets `src` to the pin and applies the series with `git am --3way`, in order. Refuses to run
 * if `src` has uncommitted changes, or commits that the series does not contain (work that
 * `chromium:patch-export` has not saved), unless `force`. If `src` is already the pin plus
 * exactly the series, it does nothing. The caller must hold the checkout lock and guarantee that
 * `srcDir` is a BDK-managed checkout; use `applyToCheckout` for that.
 */
export async function applySeries(opts: ApplyOptions): Promise<ApplyResult> {
  const log = opts.log ?? (() => {});
  const { patches, orphans } = loadSeries(opts.patchesDir);
  for (const o of orphans) log(`warning: ${o} is not listed in series and is ignored`);

  if (await amInProgress(opts.srcDir)) {
    if (!opts.force) {
      throw new UnsafeCheckoutError(
        `${opts.srcDir} has a "git am" in progress. Finish it, or run "git am --abort" there, or pass --force.`,
      );
    }
    await gitRaw(opts.srcDir, [...bdkIdentityArgs, "am", "--abort"]);
  }

  const state = await seriesState(opts.srcDir, opts.pinCommit, patches);
  const names = patches.map((p) => p.name);
  if (state.applied) {
    log(`patches: already applied (${patches.length} on ${opts.pinCommit.slice(0, 12)})`);
    await recordBdkState(opts.srcDir, opts.pinCommit, state.head);
    return { status: "already-applied", patches: names, head: state.head };
  }
  if (!opts.force) {
    if (state.dirty.length > 0) {
      throw new UnsafeCheckoutError(
        `${opts.srcDir} has uncommitted changes; resetting to the pin would discard them:\n${state.dirty
          .slice(0, 20)
          .map((l) => `  ${l}`)
          .join("\n")}\nCommit them and run chromium:patch-export, or pass --force to discard.`,
      );
    }
    if (!state.atOrAbovePin) {
      throw new UnsafeCheckoutError(
        `HEAD (${state.head.slice(0, 12)}) is not a descendant of the pin ${opts.pinCommit.slice(0, 12)}, or the pin is not in the checkout. ` +
          "Run chromium:sync to move the checkout to the pin, or pass --force to reset anyway.",
      );
    }
    if (state.extraCommits > 0) {
      throw new UnsafeCheckoutError(
        `${opts.srcDir} has ${state.extraCommits} commit(s) on top of the pin that are not the current series. ` +
          "They would be lost. Run chromium:patch-export to save them as patches, or pass --force to discard them.",
      );
    }
  }

  await git(opts.srcDir, ["reset", "--hard", "--quiet", opts.pinCommit]);
  for (const patch of patches) {
    log(`patch: applying ${patch.name}`);
    const r = await amPatch(opts.srcDir, patch.path);
    if (!r.ok) throw new PatchConflictError(patch.name, r.files, r.output);
  }
  const head = (await git(opts.srcDir, ["rev-parse", "HEAD"])).trim();
  await recordBdkState(opts.srcDir, opts.pinCommit, head);
  log(`patches: applied ${patches.length} on ${opts.pinCommit.slice(0, 12)}`);
  return { status: "applied", patches: names, head };
}

/** `applySeries` on a BDK-managed checkout (under `$BDK_HOME/chromium`), using the repo's pin and series. */
export function applyToCheckout(
  ctx: CheckoutContext,
  options: { force?: boolean; log?: (line: string) => void } = {},
): Promise<ApplyResult> {
  if (!existsSync(join(ctx.root, ".gclient")) || !existsSync(join(ctx.src, ".git"))) {
    throw new UnsafeCheckoutError(
      `${ctx.root} is not a BDK-managed checkout (no .gclient and src/.git). Run chromium:sync first.`,
    );
  }
  return applySeries({
    srcDir: ctx.src,
    pinCommit: readPin(ctx.layout).commit,
    patchesDir: ctx.layout.patchesDir,
    ...(options.force === undefined ? {} : { force: options.force }),
    ...(options.log === undefined ? {} : { log: options.log }),
  });
}

async function main(): Promise<void> {
  const { values } = parseArgs({
    args: process.argv.slice(2),
    options: {
      experiment: { type: "string" },
      force: { type: "boolean", default: false },
      [LOCKED_FLAG.slice(2)]: { type: "boolean", default: false },
    },
    strict: true,
  });
  const checkout = checkoutFromFlag(values.experiment);
  if (!values.locked) {
    process.exit(await relaunchUnderLock(checkout, import.meta.url, process.argv.slice(2)));
  }
  const ctx = checkoutContext(checkout, undefined, chromiumLayout());
  try {
    const r = await applyToCheckout(ctx, { force: values.force, log: (l) => console.log(l) });
    console.log(`patch-apply: ${r.status}, HEAD ${r.head.slice(0, 12)}`);
  } catch (e) {
    fail(e instanceof Error ? e.message : String(e));
  }
}

if (isMain(import.meta.url)) await main();
