// Everything the roll bot does to GitHub, behind one interface so tests and --dry-run never reach it.
import { run } from "../git.js";

export interface RollPr {
  readonly number: number;
  readonly url: string;
  readonly title: string;
  readonly headRefName: string;
  readonly isDraft: boolean;
  readonly labels: readonly string[];
}

export interface RollIssue {
  readonly number: number;
  readonly url: string;
  readonly title: string;
}

export interface NewPr {
  readonly head: string;
  readonly base: string;
  readonly title: string;
  readonly body: string;
  readonly draft: boolean;
  readonly labels: readonly string[];
}

export interface PrUpdate {
  readonly title: string;
  readonly body: string;
  readonly draft: boolean;
  readonly addLabels: readonly string[];
  readonly removeLabels: readonly string[];
}

export interface GitHubClient {
  /** Open pull requests whose head branch starts with `chromium-roll/`. */
  listRollPrs(): Promise<RollPr[]>;
  /** Content of `path` at the tip of `branch` on the remote, or null when the branch or file is absent. */
  remoteFile(branch: string, path: string): Promise<string | null>;
  /** Pushes the local `branch`. `update` means the branch exists on the remote and is replaced (with lease). */
  pushBranch(o: { cwd: string; branch: string; update: boolean }): Promise<void>;
  ensureLabel(name: string, color: string, description: string): Promise<void>;
  createPr(pr: NewPr): Promise<{ number: number; url: string }>;
  updatePr(current: RollPr, update: PrUpdate): Promise<void>;
  /** The open stale-pin issue, if there is one. */
  findStaleIssue(): Promise<RollIssue | null>;
  createIssue(o: {
    title: string;
    body: string;
    labels: readonly string[];
  }): Promise<{ number: number; url: string }>;
  updateIssue(number: number, o: { title: string; body: string }): Promise<void>;
}

export const ROLL_BRANCH_PREFIX = "chromium-roll/";
export const NEEDS_REBASE_LABEL = "needs-rebase";
export const STALE_LABEL = "chromium-pin-stale";

/** The real client: the `gh` CLI (authenticated by GH_TOKEN) and `git push`. */
export class GhCliClient implements GitHubClient {
  constructor(private readonly cwd: string) {}

  private async exec(
    cmd: string,
    args: string[],
    input?: string,
    cwd: string = this.cwd,
  ): Promise<string> {
    const r = await run(cmd, args, { cwd, ...(input === undefined ? {} : { input }) });
    if (r.code !== 0) {
      throw new Error(`${cmd} ${args.join(" ")} failed (exit ${r.code}): ${r.stderr.trim()}`);
    }
    return r.stdout;
  }

  private gh(args: string[], input?: string): Promise<string> {
    return this.exec("gh", args, input);
  }

  async listRollPrs(): Promise<RollPr[]> {
    const out = await this.gh([
      "pr",
      "list",
      "--state",
      "open",
      "--limit",
      "200",
      "--json",
      "number,url,title,headRefName,isDraft,labels",
    ]);
    const rows = JSON.parse(out) as Array<{
      number: number;
      url: string;
      title: string;
      headRefName: string;
      isDraft: boolean;
      labels: Array<{ name: string }>;
    }>;
    return rows
      .filter((r) => r.headRefName.startsWith(ROLL_BRANCH_PREFIX))
      .map((r) => ({
        number: r.number,
        url: r.url,
        title: r.title,
        headRefName: r.headRefName,
        isDraft: r.isDraft,
        labels: r.labels.map((l) => l.name),
      }));
  }

  async remoteFile(branch: string, path: string): Promise<string | null> {
    const ref = `refs/remotes/origin/${branch}`;
    const fetched = await run(
      "git",
      ["fetch", "--quiet", "--depth=1", "origin", `+refs/heads/${branch}:${ref}`],
      { cwd: this.cwd },
    );
    if (fetched.code !== 0) return null;
    const shown = await run("git", ["show", `${ref}:${path}`], { cwd: this.cwd });
    return shown.code === 0 ? shown.stdout : null;
  }

  async pushBranch(o: { cwd: string; branch: string; update: boolean }): Promise<void> {
    // The lease compares against the remote-tracking ref remoteFile() fetched, so a commit someone else
    // pushed to the roll branch since then is not overwritten.
    const args = ["push", "origin", `${o.branch}:refs/heads/${o.branch}`];
    if (o.update) args.splice(1, 0, `--force-with-lease=refs/heads/${o.branch}`);
    await this.exec("git", args, undefined, o.cwd);
  }

  async ensureLabel(name: string, color: string, description: string): Promise<void> {
    const out = await this.gh(["label", "list", "--limit", "500", "--json", "name"]);
    if ((JSON.parse(out) as Array<{ name: string }>).some((l) => l.name === name)) return;
    await this.gh(["label", "create", name, "--color", color, "--description", description]);
  }

  async createPr(pr: NewPr): Promise<{ number: number; url: string }> {
    const args = [
      "pr",
      "create",
      "--head",
      pr.head,
      "--base",
      pr.base,
      "--title",
      pr.title,
      "--body-file",
      "-",
    ];
    if (pr.draft) args.push("--draft");
    for (const l of pr.labels) args.push("--label", l);
    const url = (await this.gh(args, pr.body)).trim().split("\n").pop() ?? "";
    return { url, number: Number(url.split("/").pop()) };
  }

  async updatePr(current: RollPr, u: PrUpdate): Promise<void> {
    const n = String(current.number);
    const args = ["pr", "edit", n, "--title", u.title, "--body-file", "-"];
    for (const l of u.addLabels) args.push("--add-label", l);
    for (const l of u.removeLabels) args.push("--remove-label", l);
    await this.gh(args, u.body);
    if (u.draft !== current.isDraft) {
      await this.gh(u.draft ? ["pr", "ready", n, "--undo"] : ["pr", "ready", n]);
    }
  }

  async findStaleIssue(): Promise<RollIssue | null> {
    const out = await this.gh([
      "issue",
      "list",
      "--state",
      "open",
      "--label",
      STALE_LABEL,
      "--json",
      "number,url,title",
    ]);
    const rows = JSON.parse(out) as RollIssue[];
    return rows[0] ?? null;
  }

  async createIssue(o: {
    title: string;
    body: string;
    labels: readonly string[];
  }): Promise<{ number: number; url: string }> {
    const args = ["issue", "create", "--title", o.title, "--body-file", "-"];
    for (const l of o.labels) args.push("--label", l);
    const url = (await this.gh(args, o.body)).trim().split("\n").pop() ?? "";
    return { url, number: Number(url.split("/").pop()) };
  }

  async updateIssue(number: number, o: { title: string; body: string }): Promise<void> {
    await this.gh(
      ["issue", "edit", String(number), "--title", o.title, "--body-file", "-"],
      o.body,
    );
  }
}

/** For --dry-run: reads nothing, writes nothing, and says what it would have done. */
export class DryRunClient implements GitHubClient {
  constructor(private readonly say: (line: string) => void) {}
  async listRollPrs(): Promise<RollPr[]> {
    this.say("dry-run: would list open roll PRs with gh (not called; assuming none)");
    return [];
  }
  async remoteFile(): Promise<string | null> {
    return null;
  }
  async pushBranch(o: { branch: string; update: boolean }): Promise<void> {
    this.say(`dry-run: would push ${o.branch}`);
  }
  async ensureLabel(name: string): Promise<void> {
    this.say(`dry-run: would create label ${name} if missing`);
  }
  async createPr(pr: NewPr): Promise<{ number: number; url: string }> {
    this.say(
      `dry-run: would open ${pr.draft ? "DRAFT " : ""}PR "${pr.title}" (${pr.head} -> ${pr.base}) labels=[${pr.labels.join(", ")}]\n${indent(pr.body)}`,
    );
    return { number: 0, url: "(dry run)" };
  }
  async updatePr(current: RollPr, u: PrUpdate): Promise<void> {
    this.say(`dry-run: would update PR #${current.number}: "${u.title}"\n${indent(u.body)}`);
  }
  async findStaleIssue(): Promise<RollIssue | null> {
    return null;
  }
  async createIssue(o: { title: string; body: string }): Promise<{ number: number; url: string }> {
    this.say(`dry-run: would open issue "${o.title}"\n${indent(o.body)}`);
    return { number: 0, url: "(dry run)" };
  }
  async updateIssue(number: number, o: { title: string }): Promise<void> {
    this.say(`dry-run: would update issue #${number}: "${o.title}"`);
  }
}

function indent(text: string): string {
  return text
    .split("\n")
    .map((l) => `    | ${l}`)
    .join("\n");
}
