// The Chromium roll (repository and tooling plan, section 6.1): detect, resolve, bump, check, open or update one PR.
import { mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { checkPatchSeries, type PatchCheckResult } from "../chromium/patch-check.js";
import { git, gitRaw, run } from "../git.js";
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
const ROLL_NAME = "BDK Chromium roll";
const ROLL_EMAIL = "roll@bdk.invalid";
/** Every commit the bot makes ends with `BDK-Roll-Bot: <version>`; with the identity, that marks it as the bot's. */
export const ROLL_TRAILER = "BDK-Roll-Bot";
const ROLL_IDENTITY = [
  "-c",
  `user.name=${ROLL_NAME}`,
  "-c",
  `user.email=${ROLL_EMAIL}`,
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
  /** The open roll PR already pins a version newer than the detected stable; nothing is changed. */
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
      lines.push(`- ${code(p.patch)}: ${p.status}`);
      if (p.status === "conflict") {
        if (p.files.length === 0) lines.push(`  - ${plain(firstLine(p.detail))}`);
        for (const f of p.files) lines.push(`  - conflicting file: ${code(f)}`);
      } else {
        lines.push(`  - ${plain(p.detail)}`);
      }
    }
    for (const o of b.check.orphans)
      lines.push(`- ${code(o)} is in the patches directory but not in \`series\``);
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

/** Inline code span for text that comes from the repository: no backticks, one line. */
function code(s: string): string {
  return `\`${s.replace(/[`\r\n]/g, " ")}\``;
}

/** Text from tool output for a markdown list item: one line, no markup, no mentions or links. */
function plain(s: string): string {
  return s
    .replace(/[\r\n]+/g, " ")
    .replace(/[`@<>[\]*_]/g, "\\$&")
    .slice(0, 300);
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

/**
 * The roll reads the pin and patches from the checkout and builds a new roll branch from HEAD,
 * so HEAD must be the base the pull request targets; otherwise unmerged commits ride along.
 */
async function assertOnBase(repoRoot: string, base: string): Promise<void> {
  const resolve = async (ref: string): Promise<string | null> => {
    const r = await gitRaw(repoRoot, ["rev-parse", "--verify", "--quiet", `${ref}^{commit}`]);
    return r.code === 0 ? r.stdout.trim() : null;
  };
  const head = await resolve("HEAD");
  const candidates = [`refs/remotes/origin/${base}`, `refs/heads/${base}`];
  const tips: string[] = [];
  for (const ref of candidates) {
    const sha = await resolve(ref);
    if (sha !== null) tips.push(sha);
  }
  if (tips.length === 0) throw new Error(`roll: cannot resolve the base branch "${base}"`);
  if (head === null || !tips.includes(head)) {
    throw new Error(
      `roll: HEAD (${head?.slice(0, 12) ?? "none"}) is not the tip of the base branch "${base}"; ` +
        `check out ${base} so the roll branch does not carry unrelated commits.`,
    );
  }
}

async function bumpBranch(o: {
  repoRoot: string;
  branch: string;
  /** Commit the branch is built on; the checked-out commit when absent. */
  startPoint?: string;
  pinText: string;
  title: string;
  version: string;
}): Promise<() => Promise<void>> {
  const dirty = (await git(o.repoRoot, ["status", "--porcelain"])).trim();
  if (dirty)
    throw new Error(`the working tree at ${o.repoRoot} has uncommitted changes:\n${dirty}`);
  const head = (
    await git(o.repoRoot, ["symbolic-ref", "--quiet", "--short", "HEAD"]).catch(() => "")
  ).trim();
  const original = head || (await git(o.repoRoot, ["rev-parse", "HEAD"])).trim();
  await git(o.repoRoot, [
    "checkout",
    "--quiet",
    "-B",
    o.branch,
    ...(o.startPoint === undefined ? [] : [o.startPoint]),
  ]);
  writeFileSync(join(o.repoRoot, "chromium", "pin.json"), o.pinText);
  await git(o.repoRoot, ["add", "chromium/pin.json"]);
  // The environment wins over -c, so set the author explicitly too: the identity marks the bot's commits.
  await git(
    o.repoRoot,
    [...ROLL_IDENTITY, "commit", "--quiet", "-m", o.title, "-m", `${ROLL_TRAILER}: ${o.version}`],
    {
      env: {
        ...process.env,
        GIT_AUTHOR_NAME: ROLL_NAME,
        GIT_AUTHOR_EMAIL: ROLL_EMAIL,
        GIT_COMMITTER_NAME: ROLL_NAME,
        GIT_COMMITTER_EMAIL: ROLL_EMAIL,
      },
    },
  );
  return async () => {
    await gitRaw(o.repoRoot, ["checkout", "--quiet", original]);
  };
}

/**
 * True when every commit in `HEAD..sha` is one the bot made: the roll identity, the trailer, and nothing
 * touched but chromium/pin.json. Any other commit is a person's work and must survive.
 */
export async function onlyBotCommits(repoRoot: string, sha: string): Promise<boolean> {
  const shas = (await git(repoRoot, ["rev-list", `HEAD..${sha}`])).split("\n").filter(Boolean);
  for (const c of shas) {
    const meta = await git(repoRoot, ["show", "-s", "--format=%ae%x00%B", c]);
    const [email, message = ""] = meta.split("\0");
    if (email?.trim() !== ROLL_EMAIL) return false;
    if (!new RegExp(`^${ROLL_TRAILER}: \\d+(\\.\\d+)*\\s*$`, "m").test(message)) return false;
    const files = (await git(repoRoot, ["diff-tree", "--no-commit-id", "--name-only", "-r", c]))
      .split("\n")
      .filter(Boolean);
    if (files.length !== 1 || files[0] !== "chromium/pin.json") return false;
  }
  return true;
}

/** Writes `chromium/patches` as committed at `sha` to a temporary directory; returns it and a cleanup. */
async function patchesAt(
  repoRoot: string,
  sha: string,
): Promise<{ dir: string; cleanup: () => void }> {
  const tmp = mkdtempSync(join(tmpdir(), "bdk-roll-head-"));
  const cleanup = () => rmSync(tmp, { recursive: true, force: true });
  try {
    const tarball = join(tmp, "patches.tar");
    await git(repoRoot, ["archive", "--format=tar", "-o", tarball, sha, "chromium/patches"]);
    mkdirSync(join(tmp, "tree"));
    const x = await run("tar", ["-xf", tarball, "-C", join(tmp, "tree")]);
    if (x.code !== 0) throw new Error(`tar failed: ${x.stderr.trim()}`);
  } catch (e) {
    cleanup();
    throw new Error(`could not read chromium/patches at ${sha}: ${(e as Error).message}`);
  }
  return { dir: join(tmp, "tree", "chromium", "patches"), cleanup };
}

type BranchMode = "create" | "keep" | "extend" | "replace";

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
  await assertOnBase(opts.repoRoot, base);
  const version = stable.version;

  // 2. Resolve.
  const commit = await resolveTag(opts.remote, version, opts.repoRoot);
  if (commit === null) {
    log(`roll: tag ${version} is not on ${opts.remote} yet; try again on the next run`);
    return { kind: "tag-missing", version };
  }
  log(`roll: ${version} resolves to ${commit}`);

  const client = opts.client;
  const title = rollTitle(pin.tag, version);
  const days = daysBehind(pin.tag, stable.releases, now);

  // 3. Plan the branch. The roll branch may carry a person's commits (rebased patches, overlay fixes), so what
  // is on the remote decides what the bot may do to it.
  const open = await client.listRollPrs();
  if (open.length > 1)
    log(`roll: ${open.length} roll PRs are open; updating #${open[0]?.number}, leaving the others`);
  const existing: RollPr | undefined = open[0];
  const branch = existing?.headRefName ?? `${ROLL_BRANCH_PREFIX}${version}`;
  // Also looked up without a PR: a rerun after a push whose PR creation failed must not push a second branch.
  const headSha = await client.fetchBranch(branch);
  let mode: BranchMode = "create";
  let basePin = pinText;
  if (headSha !== null) {
    const shown = await gitRaw(opts.repoRoot, ["show", `${headSha}:chromium/pin.json`]);
    const headPinText = shown.code === 0 ? shown.stdout : null;
    const headTag = headPinText === null ? null : safeTag(headPinText);
    if (headTag !== null && compareVersions(headTag, version) > 0) {
      log(
        `roll: ${branch} already pins ${headTag}, newer than stable ${version}; leaving it as it is`,
      );
      return { kind: "up-to-date", pin: headTag, stable: version };
    }
    if (headTag === version) {
      // A person may have pushed a built runtime to this branch; do not reset it.
      mode = "keep";
    } else if (await onlyBotCommits(opts.repoRoot, headSha)) {
      mode = "replace";
    } else {
      mode = "extend";
      if (headTag !== null && headPinText !== null) basePin = headPinText;
    }
  }
  log(`roll: branch ${branch}: ${mode}${headSha ? ` (remote at ${headSha})` : ""}`);

  // 4. Check the patch series at the new tag (no build); only reads. A branch that keeps or extends the
  // remote is judged by its own patches, not the default branch's: a maintainer may have repaired them.
  let patchesDir = join(opts.repoRoot, "chromium", "patches");
  let cleanup = () => {};
  if (headSha !== null && (mode === "keep" || mode === "extend")) {
    const at = await patchesAt(opts.repoRoot, headSha);
    patchesDir = at.dir;
    cleanup = at.cleanup;
  }
  let check: PatchCheckResult;
  try {
    check = await checkPatchSeries({
      remote: opts.remote,
      tag: version,
      expectCommit: commit,
      patchesDir,
      log,
    });
  } finally {
    cleanup();
  }
  // A PR that already exists keeps its draft state unless its own patches conflict: a person may have
  // marked it ready, and the bot never takes that back.
  const draft = existing && check.ok ? existing.isDraft : !check.ok;
  const body = renderPrBody({ oldTag: pin.tag, newTag: version, commit, daysBehind: days, check });
  log(
    `roll: ${title}; patch series ${check.ok ? "applies" : "FAILS"}${!check.ok ? " (draft, needs-rebase)" : ""}`,
  );

  // 5. Bump the branch, then open or update the one roll PR.
  const newPin = bumpedPinText(basePin, version, commit);
  if (mode === "keep") {
    log(`roll: ${branch} already pins ${version}; leaving the branch as it is`);
  } else if (dry) {
    log(`dry-run: would commit to ${branch} (${mode}) and push:\n${newPin}`);
    await client.pushBranch({ cwd: opts.repoRoot, branch });
  } else {
    const restore = await bumpBranch({
      repoRoot: opts.repoRoot,
      branch,
      ...(mode === "extend" && headSha !== null ? { startPoint: headSha } : {}),
      pinText: newPin,
      title,
      version,
    });
    try {
      await client.pushBranch({
        cwd: opts.repoRoot,
        branch,
        ...(mode === "replace" && headSha !== null ? { replaceSha: headSha } : {}),
      });
    } finally {
      await restore();
    }
  }

  if (!check.ok) {
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
      addLabels:
        !check.ok && !existing.labels.includes(NEEDS_REBASE_LABEL) ? [NEEDS_REBASE_LABEL] : [],
      removeLabels:
        check.ok && existing.labels.includes(NEEDS_REBASE_LABEL) ? [NEEDS_REBASE_LABEL] : [],
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
