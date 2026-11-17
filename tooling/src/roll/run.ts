// The Chromium roll (repository and tooling plan, section 6.1): detect, resolve, bump, check, open or update one PR.
import { readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { checkPatchSeries, type PatchCheckResult } from "../chromium/patch-check.js";
import { git, gitRaw } from "../git.js";
import { parsePin } from "../runtime/pin.js";
import {
  type GitHubClient,
  NEEDS_REBASE_LABEL,
  ROLL_BRANCH_PREFIX,
  type RollPr,
  STALE_LABEL,
} from "./github.js";
import {
  compareVersions,
  daysBehind,
  detectStable,
  type FetchLike,
  milestone,
  type StableInfo,
} from "./version.js";

export const DEFAULT_STALE_DAYS = 14;
const ROLL_IDENTITY = [
  "-c",
  "user.name=BDK Chromium roll",
  "-c",
  "user.email=roll@bdk.invalid",
  "-c",
  "commit.gpgsign=false",
];

export interface RollOptions {
  /** The BDK workspace root (holds chromium/pin.json and chromium/patches). */
  readonly repoRoot: string;
  /** Git remote that carries the Chromium tags, and is fetched for the patch check. */
  readonly remote: string;
  readonly client: GitHubClient;
  readonly fetch: FetchLike;
  readonly now?: () => Date;
  /** More days behind than this opens (or updates) the stale-pin issue. */
  readonly staleDays?: number;
  /** The branch roll PRs target. */
  readonly base?: string;
  /** Print the plan; no local branch, no push, no GitHub writes. */
  readonly dryRun?: boolean;
  readonly log?: (line: string) => void;
}

export type RollOutcome =
  | { readonly kind: "up-to-date"; readonly pin: string; readonly stable: string }
  | { readonly kind: "tag-missing"; readonly version: string }
  | {
      readonly kind: "rolled";
      readonly version: string;
      readonly commit: string;
      readonly title: string;
      readonly draft: boolean;
      readonly branch: string;
      readonly prUrl: string;
      readonly prCreated: boolean;
      readonly daysBehind: number | null;
      readonly issue: "opened" | "updated" | "none";
      readonly check: PatchCheckResult;
    };

/** The commit a tag resolves to: the peeled `^{}` value when the tag is annotated, else the tag's own. */
export async function resolveTag(remote: string, tag: string, cwd: string): Promise<string | null> {
  const out = await git(cwd, ["ls-remote", remote, `refs/tags/${tag}`, `refs/tags/${tag}^{}`]);
  let plain: string | null = null;
  let peeled: string | null = null;
  for (const line of out.split("\n")) {
    const [sha, ref] = line.trim().split(/\s+/);
    if (!sha || !ref) continue;
    if (ref === `refs/tags/${tag}^{}`) peeled = sha;
    else if (ref === `refs/tags/${tag}`) plain = sha;
  }
  return peeled ?? plain;
}

/** pin.json text with tag and commit replaced and the runtime record cleared. Other fields are kept. */
export function bumpedPinText(text: string, tag: string, commit: string): string {
  parsePin(text);
  const raw = JSON.parse(text) as Record<string, unknown>;
  return `${JSON.stringify({ ...raw, tag, commit, runtime: null }, null, 2)}\n`;
}

export function rollTitle(oldTag: string, newTag: string): string {
  const type = milestone(oldTag) === milestone(newTag) ? "fix" : "feat";
  return `${type}(chromium): roll to ${newTag}`;
}

export function releaseNotesUrl(version: string): string {
  return `https://chromereleases.googleblog.com/search?q=${encodeURIComponent(version)}`;
}

export interface BodyInput {
  readonly oldTag: string;
  readonly newTag: string;
  readonly commit: string;
  readonly daysBehind: number | null;
  readonly check: PatchCheckResult;
}

export function renderPrBody(b: BodyInput): string {
  const lines = [
    `Rolls the Chromium pin from \`${b.oldTag}\` to \`${b.newTag}\` (\`${b.commit}\`).`,
    "",
    `- Release notes: ${releaseNotesUrl(b.newTag)} (a search of the Chrome Releases blog; not every build has its own post)`,
    `- Source changes: https://chromium.googlesource.com/chromium/src/+log/${b.oldTag}..${b.newTag}?pretty=fuller`,
    `- Days behind stable: ${b.daysBehind === null ? "unknown (no release date available)" : `${b.daysBehind} (days since the first stable release newer than the pin)`}`,
    "",
    "## Patch series",
    "",
  ];
  if (b.check.ok) {
    lines.push(
      b.check.patches.length === 0
        ? "The series is empty; nothing to apply."
        : `All ${b.check.patches.length} patch(es) apply to \`${b.newTag}\` with \`git am --3way\`.`,
    );
  } else {
    lines.push(
      `The series does **not** apply to \`${b.newTag}\`. This pull request is a draft with the \`${NEEDS_REBASE_LABEL}\` label.`,
      "",
    );
    for (const p of b.check.patches) {
      if (p.status === "applied") continue;
      lines.push(`- \`${p.patch}\`: ${p.status}`);
      if (p.status === "conflict") {
        if (p.files.length === 0) lines.push(`  - ${firstLine(p.detail)}`);
        for (const f of p.files) lines.push(`  - conflicting file: \`${f}\``);
      } else {
        lines.push(`  - ${p.detail}`);
      }
    }
    for (const o of b.check.orphans)
      lines.push(`- \`${o}\` is in the patches directory but not in \`series\``);
  }
  lines.push(
    "",
    "## Next steps",
    "",
    "CI does not build Chromium. Check out this branch, rebase any failing patches (`pnpm chromium:patch-apply`, then `pnpm chromium:patch-export`), build and package locally, run `pnpm test:release`, publish with `pnpm chromium:publish`, push the updated `chromium/pin.json` to this branch, and merge once the integration workflow passes.",
    "",
    "_Opened by the Chromium roll bot (`chromium-roll.yml`). The bot rewrites this description and `chromium/pin.json` when a newer stable appears._",
  );
  return lines.join("\n");
}

function firstLine(s: string): string {
  return (
    s
      .split("\n")
      .find((l) => l.trim() !== "")
      ?.trim() ?? "no detail"
  );
}

export function renderIssue(o: {
  pinTag: string;
  stable: string;
  days: number;
  threshold: number;
  prUrl: string | null;
}): { title: string; body: string } {
  return {
    title: `Chromium pin is ${o.days} days behind stable`,
    body: [
      `The pin in \`chromium/pin.json\` is \`${o.pinTag}\`; Mac stable is \`${o.stable}\`. The pin has been behind for ${o.days} days, more than the ${o.threshold}-day threshold.`,
      "",
      "Stable releases carry security fixes, so a stale pin is an exposure (risk R4 in `docs/planning/risks.md`).",
      "",
      o.prUrl ? `Roll pull request: ${o.prUrl}` : "There is no roll pull request yet.",
      "",
      "_Opened by the Chromium roll bot; it edits this issue while the pin stays behind._",
    ].join("\n"),
  };
}

async function bumpBranch(o: {
  repoRoot: string;
  branch: string;
  pinText: string;
  title: string;
}): Promise<() => Promise<void>> {
  const dirty = (await git(o.repoRoot, ["status", "--porcelain"])).trim();
  if (dirty)
    throw new Error(`the working tree at ${o.repoRoot} has uncommitted changes:\n${dirty}`);
  const head = (
    await git(o.repoRoot, ["symbolic-ref", "--quiet", "--short", "HEAD"]).catch(() => "")
  ).trim();
  const original = head || (await git(o.repoRoot, ["rev-parse", "HEAD"])).trim();
  await git(o.repoRoot, ["checkout", "--quiet", "-B", o.branch]);
  writeFileSync(join(o.repoRoot, "chromium", "pin.json"), o.pinText);
  await git(o.repoRoot, ["add", "chromium/pin.json"]);
  await git(o.repoRoot, [...ROLL_IDENTITY, "commit", "--quiet", "-m", o.title]);
  return async () => {
    await gitRaw(o.repoRoot, ["checkout", "--quiet", original]);
  };
}

export async function roll(opts: RollOptions): Promise<RollOutcome> {
  const log = opts.log ?? (() => {});
  const now = (opts.now ?? (() => new Date()))();
  const staleDays = opts.staleDays ?? DEFAULT_STALE_DAYS;
  const base = opts.base ?? "main";
  const dry = opts.dryRun === true;
  const pinFile = join(opts.repoRoot, "chromium", "pin.json");
  const pinText = readFileSync(pinFile, "utf8");
  const pin = parsePin(pinText);

  // 1. Detect.
  const stable: StableInfo = await detectStable(opts.fetch, log);
  log(`roll: stable is ${stable.version} (from ${stable.source}); pin is ${pin.tag}`);
  if (compareVersions(stable.version, pin.tag) <= 0) {
    log(`roll: pin is up to date (${pin.tag})`);
    return { kind: "up-to-date", pin: pin.tag, stable: stable.version };
  }
  const version = stable.version;

  // 2. Resolve.
  const commit = await resolveTag(opts.remote, version, opts.repoRoot);
  if (commit === null) {
    log(`roll: tag ${version} is not on ${opts.remote} yet; try again on the next run`);
    return { kind: "tag-missing", version };
  }
  log(`roll: ${version} resolves to ${commit}`);

  // 4. Check the patch series at the new tag (no build). Done before touching branches; it only reads.
  const check = await checkPatchSeries({
    remote: opts.remote,
    tag: version,
    expectCommit: commit,
    patchesDir: join(opts.repoRoot, "chromium", "patches"),
    log,
  });
  const title = rollTitle(pin.tag, version);
  const days = daysBehind(pin.tag, stable.releases, now);
  const draft = !check.ok;
  const body = renderPrBody({ oldTag: pin.tag, newTag: version, commit, daysBehind: days, check });
  log(
    `roll: ${title}; patch series ${check.ok ? "applies" : "FAILS"}${draft ? " (draft, needs-rebase)" : ""}`,
  );

  const client = opts.client;

  // 3 and 5. Bump on a branch and open or update the one roll PR.
  const existing: RollPr | undefined = (await client.listRollPrs())[0];
  const branch = existing?.headRefName ?? `${ROLL_BRANCH_PREFIX}${version}`;
  const newPin = bumpedPinText(pinText, version, commit);

  const remotePin = existing ? await client.remoteFile(branch, "chromium/pin.json") : null;
  const remoteTag = remotePin ? safeTag(remotePin) : null;
  if (remoteTag === version) {
    // A person may have pushed a built runtime to this branch; do not reset it.
    log(`roll: ${branch} already pins ${version}; leaving the branch as it is`);
  } else if (dry) {
    log(`dry-run: would commit to ${branch} (from ${base}) and push:\n${newPin}`);
    await client.pushBranch({ cwd: opts.repoRoot, branch, update: existing !== undefined });
  } else {
    const restore = await bumpBranch({ repoRoot: opts.repoRoot, branch, pinText: newPin, title });
    try {
      await client.pushBranch({ cwd: opts.repoRoot, branch, update: existing !== undefined });
    } finally {
      await restore();
    }
  }

  if (draft) {
    await client.ensureLabel(
      NEEDS_REBASE_LABEL,
      "d93f0b",
      "Patch series does not apply to the pinned Chromium tag",
    );
  }
  let prUrl: string;
  let prCreated = false;
  if (existing) {
    await client.updatePr(existing, {
      title,
      body,
      draft,
      addLabels: draft && !existing.labels.includes(NEEDS_REBASE_LABEL) ? [NEEDS_REBASE_LABEL] : [],
      removeLabels:
        !draft && existing.labels.includes(NEEDS_REBASE_LABEL) ? [NEEDS_REBASE_LABEL] : [],
    });
    prUrl = existing.url;
    log(`roll: updated ${prUrl}`);
  } else {
    const pr = await client.createPr({
      head: branch,
      base,
      title,
      body,
      draft,
      labels: draft ? [NEEDS_REBASE_LABEL] : [],
    });
    prUrl = pr.url;
    prCreated = true;
    log(`roll: opened ${prUrl}`);
  }

  // Stale pin: a security exposure (risk R4).
  let issue: "opened" | "updated" | "none" = "none";
  if (days !== null && days > staleDays) {
    const text = renderIssue({
      pinTag: pin.tag,
      stable: version,
      days,
      threshold: staleDays,
      prUrl,
    });
    await client.ensureLabel(
      STALE_LABEL,
      "b60205",
      "The Chromium pin is behind stable beyond the threshold",
    );
    const found = await client.findStaleIssue();
    if (found) {
      await client.updateIssue(found.number, text);
      issue = "updated";
    } else {
      await client.createIssue({ ...text, labels: [STALE_LABEL] });
      issue = "opened";
    }
    log(`roll: stale-pin issue ${issue} (${days} days behind, threshold ${staleDays})`);
  } else {
    log(
      `roll: ${days === null ? "days behind unknown" : `${days} day(s) behind`}; no stale-pin issue needed`,
    );
  }

  return {
    kind: "rolled",
    version,
    commit,
    title,
    draft,
    branch,
    prUrl,
    prCreated,
    daysBehind: days,
    issue,
    check,
  };
}

function safeTag(text: string): string | null {
  try {
    return parsePin(text).tag;
  } catch {
    return null;
  }
}
