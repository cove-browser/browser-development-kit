import { mkdirSync, mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { parseArgs } from "node:util";
import { git } from "../git.js";
import {
  chromiumLayout,
  DEFAULT_PATCH_CHECK_REMOTE,
  fail,
  isMain,
  loadSeriesWithApp,
  type PatchFile,
  PatchFormatError,
  readPin,
  seconds,
} from "./common.js";
import { amPatch } from "./patch-apply.js";

export type PatchCheckStatus = "applied" | "conflict" | "invalid";

export interface PatchCheckEntry {
  readonly patch: string;
  readonly status: PatchCheckStatus;
  /** Conflicting files (status `conflict`), or the reason (status `invalid`). */
  readonly files: readonly string[];
  readonly detail: string;
}

export interface PatchCheckResult {
  readonly ok: boolean;
  readonly tag: string;
  readonly commit: string | null;
  readonly remote: string;
  /** Upstream files the series touches, which is all that is checked out. */
  readonly files: readonly string[];
  readonly patches: readonly PatchCheckEntry[];
  readonly orphans: readonly string[];
  readonly timings: {
    readonly fetchMs: number;
    readonly applyMs: number;
    readonly totalMs: number;
  };
}

export interface PatchCheckOptions {
  /** Where to fetch from: the GitHub mirror by default; a local path works for fixtures. */
  readonly remote?: string;
  /** The upstream tag to check against. Build the roll bot on this with a different tag. */
  readonly tag: string;
  /** If given, the fetched commit must equal it (the pin's commit). */
  readonly expectCommit?: string;
  readonly patchesDir: string;
  /** An application's series (source mode), checked after BDK's: `loadSeriesWithApp`. */
  readonly appPatchesDir?: string;
  /** Scratch directory to use (kept afterwards); a temporary one is made and removed otherwise. */
  readonly workDir?: string;
  readonly log?: (line: string) => void;
}

/** A sparse-checkout pattern (gitignore syntax) that matches exactly `path`. */
export function sparsePattern(path: string): string {
  return `/${path.replace(/[\\*?[\]]/g, "\\$&").replace(/^(\/)([#!])/, "$1\\$2")}`;
}

/**
 * Checks that the patch series applies to an upstream tag with no build. Makes a blobless,
 * depth-1, sparse checkout of the tag limited to the files the series touches, then runs
 * `git am --3way` per patch. Every patch is tried, even after a failure, so the report lists each
 * one that does not apply (a later patch that depends on a failed one is reported too). An empty
 * series succeeds without touching the network.
 */
export async function checkPatchSeries(opts: PatchCheckOptions): Promise<PatchCheckResult> {
  const log = opts.log ?? (() => {});
  const remote = opts.remote ?? process.env.BDK_PATCH_CHECK_REMOTE ?? DEFAULT_PATCH_CHECK_REMOTE;
  const started = Date.now();
  const base = { tag: opts.tag, remote };

  let patches: readonly PatchFile[];
  let orphans: readonly string[];
  const entries: PatchCheckEntry[] = [];
  try {
    ({ patches, orphans } = loadSeriesWithApp(opts.patchesDir, opts.appPatchesDir));
  } catch (e) {
    if (!(e instanceof PatchFormatError)) throw e;
    return {
      ...base,
      ok: false,
      commit: null,
      files: [],
      patches: [{ patch: "series", status: "invalid", files: [], detail: e.message }],
      orphans: [],
      timings: { fetchMs: 0, applyMs: 0, totalMs: Date.now() - started },
    };
  }
  const files = [...new Set(patches.flatMap((p) => p.files))].sort();
  if (patches.length === 0) {
    log("patch-check: the series is empty; nothing to apply");
    return {
      ...base,
      ok: orphans.length === 0,
      commit: null,
      files,
      patches: [],
      orphans,
      timings: { fetchMs: 0, applyMs: 0, totalMs: Date.now() - started },
    };
  }

  const ownDir = opts.workDir === undefined;
  const work = opts.workDir ?? mkdtempSync(join(tmpdir(), "bdk-patch-check-"));
  const repo = join(work, "src");
  try {
    mkdirSync(repo, { recursive: true });
    await git(repo, ["init", "--quiet"]);
    await git(repo, ["remote", "add", "origin", remote]);
    await git(repo, ["sparse-checkout", "set", "--no-cone", "--stdin"], {
      input: `${files.map(sparsePattern).join("\n")}\n`,
    });
    log(`patch-check: fetching ${opts.tag} from ${remote} (${files.length} file(s))`);
    await git(repo, [
      "fetch",
      "--quiet",
      "--depth=1",
      "--filter=blob:none",
      "origin",
      `refs/tags/${opts.tag}`,
    ]);
    await git(repo, ["checkout", "--quiet", "--detach", "FETCH_HEAD"]);
    const commit = (await git(repo, ["rev-parse", "HEAD"])).trim();
    const fetchMs = Date.now() - started;
    if (opts.expectCommit !== undefined && commit !== opts.expectCommit) {
      return {
        ...base,
        ok: false,
        commit,
        files,
        patches: [
          {
            patch: "pin",
            status: "invalid",
            files: [],
            detail: `tag ${opts.tag} is ${commit} at ${remote}, but the pin says ${opts.expectCommit}`,
          },
        ],
        orphans,
        timings: { fetchMs, applyMs: 0, totalMs: Date.now() - started },
      };
    }
    const applyStart = Date.now();
    for (const patch of patches) {
      const r = await amPatch(repo, patch.path);
      entries.push({
        patch: patch.name,
        status: r.ok ? "applied" : "conflict",
        files: r.files,
        detail: r.ok ? "" : r.output.trim(),
      });
    }
    const now = Date.now();
    return {
      ...base,
      ok: entries.every((e) => e.status === "applied") && orphans.length === 0,
      commit,
      files,
      patches: entries,
      orphans,
      timings: { fetchMs, applyMs: now - applyStart, totalMs: now - started },
    };
  } finally {
    if (ownDir) rmSync(work, { recursive: true, force: true });
  }
}

export function formatReport(r: PatchCheckResult): string {
  const lines = [
    `patch-check: ${r.tag}${r.commit ? ` (${r.commit.slice(0, 12)})` : ""} from ${r.remote}`,
  ];
  if (r.patches.length === 0) lines.push("  series is empty");
  for (const p of r.patches) {
    lines.push(
      `  ${p.status === "applied" ? "ok      " : p.status === "invalid" ? "invalid " : "CONFLICT"} ${p.patch}`,
    );
    if (p.status === "conflict") {
      for (const f of p.files) lines.push(`            conflicting file: ${f}`);
      if (p.files.length === 0)
        lines.push(`            ${p.detail.split("\n").join("\n            ")}`);
    } else if (p.status === "invalid") {
      lines.push(`            ${p.detail}`);
    }
  }
  for (const o of r.orphans) lines.push(`  ORPHAN   ${o} is not listed in series`);
  lines.push(
    `patch-check: ${r.ok ? "PASS" : "FAIL"} in ${seconds(r.timings.totalMs)} (fetch ${seconds(r.timings.fetchMs)}, apply ${seconds(r.timings.applyMs)})`,
  );
  return lines.join("\n");
}

async function main(): Promise<void> {
  const { values } = parseArgs({
    args: process.argv.slice(2),
    options: {
      remote: { type: "string" },
      tag: { type: "string" },
      commit: { type: "string" },
      "work-dir": { type: "string" },
      json: { type: "boolean", default: false },
    },
    strict: true,
  });
  const layout = chromiumLayout();
  const pin = readPin(layout);
  const tag = values.tag ?? pin.tag;
  const expectCommit = values.commit ?? (tag === pin.tag ? pin.commit : undefined);
  const result = await checkPatchSeries({
    tag,
    patchesDir: layout.patchesDir,
    ...(values.remote === undefined ? {} : { remote: values.remote }),
    ...(expectCommit === undefined ? {} : { expectCommit }),
    ...(values["work-dir"] === undefined ? {} : { workDir: values["work-dir"] }),
    log: (l) => console.error(l),
  });
  console.log(values.json ? JSON.stringify(result, null, 2) : formatReport(result));
  if (!result.ok) process.exit(1);
}

// No top-level await: the bdk CLI bundles this module into a CommonJS executable (packages/cli).
if (isMain(import.meta.url)) {
  main().catch((e: unknown) => fail(e instanceof Error ? e.message : String(e)));
}
