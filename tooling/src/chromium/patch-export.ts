import { existsSync, mkdirSync, readdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { parseArgs } from "node:util";
import { git, gitRaw } from "../git.js";
import {
  checkoutContext,
  checkoutFromFlag,
  chromiumLayout,
  fail,
  isMain,
  LOCKED_FLAG,
  PatchFormatError,
  parsePatch,
  readPin,
  relaunchUnderLock,
} from "./common.js";

/**
 * Flags that make `git format-patch` output a pure function of the commit: no commit id, no git
 * version signature, full blob ids (abbreviation length depends on repository size), no
 * numbering, a fixed diff algorithm, and no user or system git configuration.
 */
const FORMAT_PATCH_ARGS = [
  "format-patch",
  "-1",
  "--stdout",
  "--zero-commit",
  "--no-signature",
  "--full-index",
  "--binary",
  "--no-numbered",
  "--diff-algorithm=myers",
  "--stat=72",
];

const PURE_ENV = { GIT_CONFIG_GLOBAL: "/dev/null", GIT_CONFIG_NOSYSTEM: "1" };

/** The exported patch text for one commit. */
export async function formatPatch(srcDir: string, commit: string): Promise<string> {
  return git(srcDir, [...FORMAT_PATCH_ARGS, commit], { env: PURE_ENV });
}

/** `git patch-id --stable` of a patch text: equal for the same change whatever the line numbers or message. */
export async function patchIdOf(srcDir: string, patchText: string): Promise<string> {
  const out = await git(srcDir, ["patch-id", "--stable"], { input: patchText, env: PURE_ENV });
  return out.split(/\s+/)[0] ?? "";
}

export function patchIdsForCommits(srcDir: string, commits: readonly string[]): Promise<string[]> {
  return Promise.all(commits.map(async (c) => patchIdOf(srcDir, await formatPatch(srcDir, c))));
}

/** `bdk: link //bdk into the Mac framework` becomes `bdk-link-bdk-into-the-mac-framework.patch`. */
export function patchFileName(subject: string, taken: ReadonlySet<string>): string {
  const slug =
    subject
      .toLowerCase()
      .replace(/[^a-z0-9]+/g, "-")
      .replace(/^-+|-+$/g, "")
      .slice(0, 72)
      .replace(/-+$/, "") || "patch";
  let name = `${slug}.patch`;
  for (let n = 2; taken.has(name); n++) name = `${slug}-${n}.patch`;
  return name;
}

export interface ExportOptions {
  readonly srcDir: string;
  readonly pinCommit: string;
  readonly patchesDir: string;
  readonly log?: (line: string) => void;
}

export interface ExportResult {
  readonly names: readonly string[];
  readonly written: readonly string[];
  readonly removed: readonly string[];
}

function writeIfChanged(path: string, text: string): boolean {
  if (existsSync(path) && readFileSync(path, "utf8") === text) return false;
  writeFileSync(path, text);
  return true;
}

/**
 * Regenerates `patches/*.patch` and `patches/series` from the commits between the pin and HEAD in
 * `src`, oldest first. Deterministic: exporting twice changes nothing. Files that did not change
 * are not rewritten, and `.patch` files no longer in the series are removed.
 */
export async function exportSeries(opts: ExportOptions): Promise<ExportResult> {
  const log = opts.log ?? (() => {});
  const { srcDir, pinCommit } = opts;
  if ((await gitRaw(srcDir, ["merge-base", "--is-ancestor", pinCommit, "HEAD"])).code !== 0) {
    throw new PatchFormatError(
      `HEAD in ${srcDir} is not a descendant of the pin ${pinCommit.slice(0, 12)}; nothing to export from.`,
    );
  }
  const merges = (await git(srcDir, ["rev-list", "--merges", `${pinCommit}..HEAD`])).trim();
  if (merges)
    throw new PatchFormatError(
      `Merge commits on top of the pin cannot be exported: ${merges.split("\n").join(", ")}`,
    );
  const commits = (await git(srcDir, ["rev-list", "--reverse", `${pinCommit}..HEAD`]))
    .split("\n")
    .filter(Boolean);

  const taken = new Set<string>();
  const outputs: { name: string; text: string }[] = [];
  for (const commit of commits) {
    const text = await formatPatch(srcDir, commit);
    // Validate before anything is written; the file name is not known yet, so name the commit.
    const probe = parsePatchOrNamed(commit, text);
    const name = patchFileName(probe.subject, taken);
    taken.add(name);
    outputs.push({ name, text });
  }

  mkdirSync(opts.patchesDir, { recursive: true });
  const written: string[] = [];
  for (const o of outputs)
    if (writeIfChanged(join(opts.patchesDir, o.name), o.text)) written.push(o.name);
  const names = outputs.map((o) => o.name);
  const seriesText = names.map((n) => `${n}\n`).join("");
  if (writeIfChanged(join(opts.patchesDir, "series"), seriesText)) written.push("series");
  const removed: string[] = [];
  for (const f of readdirSync(opts.patchesDir)) {
    if (f.endsWith(".patch") && !taken.has(f)) {
      rmSync(join(opts.patchesDir, f));
      removed.push(f);
    }
  }
  log(
    `patch-export: ${names.length} patch(es); ${written.length} written, ${removed.length} removed`,
  );
  return { names, written, removed };
}

function parsePatchOrNamed(commit: string, text: string) {
  try {
    return parsePatch(commit.slice(0, 12), "", text);
  } catch (e) {
    throw new PatchFormatError(`commit ${(e as Error).message}`);
  }
}

async function main(): Promise<void> {
  const { values } = parseArgs({
    args: process.argv.slice(2),
    options: {
      experiment: { type: "string" },
      [LOCKED_FLAG.slice(2)]: { type: "boolean", default: false },
    },
    strict: true,
  });
  const checkout = checkoutFromFlag(values.experiment);
  if (!values.locked) {
    process.exit(await relaunchUnderLock(checkout, import.meta.url, process.argv.slice(2)));
  }
  const layout = chromiumLayout();
  const ctx = checkoutContext(checkout, undefined, layout);
  if (!existsSync(join(ctx.src, ".git")))
    fail(`${ctx.src} is not a checkout. Run chromium:sync first.`);
  try {
    const dirty = (await git(ctx.src, ["status", "--porcelain", "--untracked-files=no"])).trim();
    if (dirty) console.error(`warning: ${ctx.src} has uncommitted changes; they are not exported.`);
    await exportSeries({
      srcDir: ctx.src,
      pinCommit: readPin(layout).commit,
      patchesDir: layout.patchesDir,
      log: (l) => console.log(l),
    });
  } catch (e) {
    if (e instanceof PatchFormatError) fail(e.message);
    throw e;
  }
}

if (isMain(import.meta.url)) await main();
