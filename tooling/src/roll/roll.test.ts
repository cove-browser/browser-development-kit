import { mkdirSync, readdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { describe, expect, it } from "vitest";
import { exportSeries } from "../chromium/patch-export.js";
import { commitBdkHook, gitSh, makeSrc, makeUpstream, tmp } from "../chromium/test-support.js";
import {
  type GitHubClient,
  NEEDS_REBASE_LABEL,
  type NewPr,
  type PrUpdate,
  type RollIssue,
  type RollPr,
} from "./github.js";
import { bumpedPinText, resolveTag, roll } from "./run.js";
import { CHROMIUM_DASH_URL, compareVersions, daysBehind, VERSION_HISTORY_URL } from "./version.js";

const NOW = new Date("2026-03-01T00:00:00Z");
const DAY = 86_400_000;

class FakeClient implements GitHubClient {
  readonly calls: string[] = [];
  prs: RollPr[] = [];
  issue: RollIssue | null = null;
  /** Remote branch heads by name, as fetchBranch reports them. */
  heads: Record<string, string> = {};
  created: NewPr[] = [];
  updated: Array<{ pr: RollPr; update: PrUpdate }> = [];
  issues: Array<{ title: string; body: string; labels: readonly string[] }> = [];
  issueEdits: Array<{ number: number; title: string }> = [];
  pushes: Array<{ branch: string; replaceSha: string | null }> = [];
  labels: string[] = [];
  async listRollPrs() {
    this.calls.push("listRollPrs");
    return this.prs;
  }
  async fetchBranch(branch: string) {
    this.calls.push("fetchBranch");
    return this.heads[branch] ?? null;
  }
  async pushBranch(o: { branch: string; replaceSha?: string }) {
    this.calls.push("pushBranch");
    this.pushes.push({ branch: o.branch, replaceSha: o.replaceSha ?? null });
  }
  async ensureLabel(name: string) {
    this.calls.push("ensureLabel");
    this.labels.push(name);
  }
  async createPr(pr: NewPr) {
    this.calls.push("createPr");
    this.created.push(pr);
    return { number: 7, url: "https://example.invalid/pull/7" };
  }
  async updatePr(pr: RollPr, update: PrUpdate) {
    this.calls.push("updatePr");
    this.updated.push({ pr, update });
  }
  async findStaleIssue() {
    this.calls.push("findStaleIssue");
    return this.issue;
  }
  async createIssue(o: { title: string; body: string; labels: readonly string[] }) {
    this.calls.push("createIssue");
    this.issues.push(o);
    return { number: 9, url: "https://example.invalid/issues/9" };
  }
  async updateIssue(number: number, o: { title: string }) {
    this.calls.push("updateIssue");
    this.issueEdits.push({ number, title: o.title });
  }
}

interface Api {
  versionHistory?: unknown | "fail";
  dash?: unknown | "fail";
}

function fakeFetch(api: Api) {
  const seen: string[] = [];
  const f = async (url: string): Promise<Response> => {
    seen.push(url);
    const body =
      url === VERSION_HISTORY_URL
        ? api.versionHistory
        : url === CHROMIUM_DASH_URL
          ? api.dash
          : undefined;
    if (body === undefined || body === "fail") return new Response("nope", { status: 500 });
    return new Response(JSON.stringify(body), { status: 200 });
  };
  return { f, seen };
}

const history = (...versions: string[]) => ({
  versions: versions.map((v) => ({
    name: `chrome/platforms/mac/channels/stable/versions/${v}`,
    version: v,
  })),
});
const dash = (...rows: Array<[string, number]>) =>
  rows.map(([version, time]) => ({ version, time, milestone: Number(version.split(".")[0]) }));

/** Upstream with 1.0.0 (pinned), 1.0.1 (clean point), 2.0.0 (conflicts), 3.0.0 (clean, annotated tag). */
async function fixture(pinTag = "1.0.0") {
  const up = await makeUpstream();
  await gitSh(up.dir, ["checkout", "-q", "-b", "clean", up.commit1]);
  writeFileSync(join(up.dir, "base", "unrelated.cc"), "int unrelated() { return 1; }\n");
  await gitSh(up.dir, ["commit", "-qam", "point"]);
  await gitSh(up.dir, ["tag", "1.0.1"]);
  writeFileSync(join(up.dir, "base", "unrelated.cc"), "int unrelated() { return 2; }\n");
  await gitSh(up.dir, ["commit", "-qam", "three"]);
  await gitSh(up.dir, ["tag", "-a", "3.0.0", "-m", "annotated"]);
  const peeled = (await gitSh(up.dir, ["rev-parse", "3.0.0^{commit}"])).trim();

  const src = await makeSrc(up, "1.0.0");
  await commitBdkHook(src);
  const repo = tmp("repo");
  mkdirSync(join(repo, "chromium", "patches"), { recursive: true });
  await exportSeries({
    srcDir: src,
    pinCommit: up.commit1,
    patchesDir: join(repo, "chromium", "patches"),
  });
  writeFileSync(
    join(repo, "chromium", "pin.json"),
    `${JSON.stringify({ tag: pinTag, commit: up.commit1, bdk_abi_version: 1, runtime: null }, null, 2)}\n`,
  );
  await gitSh(repo, ["init", "-q", "-b", "main"]);
  await gitSh(repo, ["add", "-A"]);
  await gitSh(repo, ["commit", "-q", "-m", "init"]);
  return { up, repo, peeled };
}

const BOT = {
  GIT_AUTHOR_NAME: "BDK Chromium roll",
  GIT_AUTHOR_EMAIL: "roll@bdk.invalid",
  GIT_COMMITTER_NAME: "BDK Chromium roll",
  GIT_COMMITTER_EMAIL: "roll@bdk.invalid",
};

/**
 * Creates `branch` off main in `repo` as if it were the remote roll branch: a pin bump to `pinTag` by the
 * bot, then optionally a person's commit that rewrites the patch series with `edit`. Returns its head.
 */
async function seedBranch(
  repo: string,
  branch: string,
  o: { pinTag: string; bot?: boolean; human?: (patchesDir: string) => void },
): Promise<string> {
  await gitSh(repo, ["checkout", "-q", "-b", branch, "main"]);
  const pin = JSON.parse(readFileSync(join(repo, "chromium", "pin.json"), "utf8"));
  writeFileSync(
    join(repo, "chromium", "pin.json"),
    `${JSON.stringify({ ...pin, tag: o.pinTag }, null, 2)}\n`,
  );
  await gitSh(repo, ["add", "-A"]);
  await gitSh(
    repo,
    ["commit", "-q", "-m", `roll ${o.pinTag}`, "-m", `BDK-Roll-Bot: ${o.pinTag}`],
    BOT,
  );
  if (o.human) {
    o.human(join(repo, "chromium", "patches"));
    await gitSh(repo, ["add", "-A"]);
    await gitSh(repo, ["commit", "-q", "-m", "fix: rebase patch"]);
  }
  const sha = (await gitSh(repo, ["rev-parse", "HEAD"])).trim();
  await gitSh(repo, ["checkout", "-q", "main"]);
  return sha;
}

const prFor = (branch: string, over: Partial<RollPr> = {}): RollPr => ({
  number: 5,
  url: "https://example.invalid/pull/5",
  title: "t",
  headRefName: branch,
  isDraft: false,
  labels: [],
  ...over,
});

const run = (
  repo: string,
  up: { dir: string },
  client: FakeClient,
  api: Api,
  extra: { staleDays?: number; dryRun?: boolean } = {},
  now: Date = NOW,
) =>
  roll({
    repoRoot: repo,
    remote: up.dir,
    client,
    fetch: fakeFetch(api).f,
    now: () => now,
    ...extra,
  });

describe("versions", () => {
  it("compares dotted versions numerically", () => {
    expect(compareVersions("158.0.8127.37", "158.0.8127.9")).toBeGreaterThan(0);
    expect(compareVersions("99.0.1.1", "158.0.0.0")).toBeLessThan(0);
    expect(compareVersions("1.2", "1.2.0")).toBe(0);
  });
  it("counts days from the first release newer than the pin", () => {
    const releases = [
      { version: "3.0.0", time: NOW.getTime() - 2 * DAY },
      { version: "2.0.0", time: NOW.getTime() - 20 * DAY },
      { version: "1.0.0", time: NOW.getTime() - 90 * DAY },
    ];
    expect(daysBehind("1.0.0", releases, NOW)).toBe(20);
    expect(daysBehind("3.0.0", releases, NOW)).toBeNull();
  });
  it("rewrites the pin and keeps other fields", () => {
    const text = JSON.stringify({
      tag: "1",
      commit: "a",
      bdk_abi_version: 4,
      runtime: null,
      extra: 1,
    });
    const out = JSON.parse(bumpedPinText(text, "2", "b"));
    expect(out).toEqual({ tag: "2", commit: "b", bdk_abi_version: 4, runtime: null, extra: 1 });
  });
});

describe("resolveTag", () => {
  it("prefers the peeled commit of an annotated tag", async () => {
    const { up, peeled } = await fixture();
    const tagObject = (await gitSh(up.dir, ["rev-parse", "3.0.0"])).trim();
    expect(tagObject).not.toBe(peeled);
    expect(await resolveTag(up.dir, "3.0.0", up.dir)).toBe(peeled);
    expect(await resolveTag(up.dir, "1.0.1", up.dir)).toBe(
      (await gitSh(up.dir, ["rev-parse", "1.0.1"])).trim(),
    );
    expect(await resolveTag(up.dir, "9.9.9", up.dir)).toBeNull();
  });
});

describe("roll", () => {
  it("does nothing when the pin is up to date", async () => {
    const { up, repo } = await fixture("3.0.0");
    const client = new FakeClient();
    const out = await run(repo, up, client, {
      versionHistory: history("3.0.0", "2.0.0"),
      dash: dash(["3.0.0", NOW.getTime()]),
    });
    expect(out.kind).toBe("up-to-date");
    expect(client.calls).toEqual([]);
    expect((await gitSh(repo, ["branch", "--list", "chromium-roll/*"])).trim()).toBe("");
  });

  it("does nothing when stable is older than the pin", async () => {
    const { up, repo } = await fixture("3.0.0");
    const client = new FakeClient();
    const out = await run(repo, up, client, { versionHistory: history("2.0.0"), dash: "fail" });
    expect(out.kind).toBe("up-to-date");
    expect(client.calls).toEqual([]);
  });

  it("opens a non-draft feat PR for a new milestone when the patches apply", async () => {
    const { up, repo, peeled } = await fixture();
    const client = new FakeClient();
    const out = await run(repo, up, client, {
      versionHistory: history("3.0.0", "2.0.0"),
      dash: dash(["3.0.0", NOW.getTime() - 2 * DAY]),
    });
    expect(out).toMatchObject({
      kind: "rolled",
      draft: false,
      title: "feat(chromium): roll to 3.0.0",
    });
    expect(client.created).toHaveLength(1);
    const pr = client.created[0] as NewPr;
    expect(pr.title).toBe("feat(chromium): roll to 3.0.0");
    expect(pr.draft).toBe(false);
    expect(pr.labels).toEqual([]);
    expect(pr.head).toBe("chromium-roll/3.0.0");
    expect(pr.body).toContain("chromereleases.googleblog.com/search?q=3.0.0");
    expect(pr.body).toContain("Days behind stable: 2");
    expect(client.pushes).toEqual([{ branch: "chromium-roll/3.0.0", replaceSha: null }]);
    expect(client.issues).toEqual([]);
    // The branch holds the bumped pin, with runtime reset and other fields kept; the checkout is restored.
    const onBranch = JSON.parse(
      await gitSh(repo, ["show", "chromium-roll/3.0.0:chromium/pin.json"]),
    );
    expect(onBranch).toEqual({ tag: "3.0.0", commit: peeled, bdk_abi_version: 1, runtime: null });
    expect((await gitSh(repo, ["rev-parse", "--abbrev-ref", "HEAD"])).trim()).toBe("main");
    expect(JSON.parse(readFileSync(join(repo, "chromium", "pin.json"), "utf8")).tag).toBe("1.0.0");
  });

  it("titles a point release fix", async () => {
    const { up, repo } = await fixture();
    const client = new FakeClient();
    await run(repo, up, client, { versionHistory: history("1.0.1"), dash: "fail" });
    expect((client.created[0] as NewPr).title).toBe("fix(chromium): roll to 1.0.1");
    expect((client.created[0] as NewPr).draft).toBe(false);
  });

  it("opens a draft with needs-rebase naming the patch and file when a patch conflicts", async () => {
    const { up, repo } = await fixture();
    const client = new FakeClient();
    const out = await run(repo, up, client, { versionHistory: history("2.0.0"), dash: "fail" });
    expect(out).toMatchObject({ kind: "rolled", draft: true });
    const pr = client.created[0] as NewPr;
    expect(pr.draft).toBe(true);
    expect(pr.labels).toEqual([NEEDS_REBASE_LABEL]);
    expect(client.labels).toContain(NEEDS_REBASE_LABEL);
    expect(pr.body).toContain("bdk-link-bdk.patch");
    expect(pr.body).toContain("`chrome/BUILD.gn`");
    expect(pr.body).toContain("Days behind stable: unknown");
  });

  it("updates the existing roll PR instead of opening a second one", async () => {
    const { up, repo } = await fixture();
    const client = new FakeClient();
    const br = "chromium-roll/1.0.1";
    client.heads[br] = await seedBranch(repo, br, { pinTag: "1.0.1" });
    client.prs = [prFor(br)];
    const out = await run(repo, up, client, { versionHistory: history("2.0.0"), dash: "fail" });
    expect(out).toMatchObject({ kind: "rolled", prCreated: false, branch: br });
    expect(client.created).toEqual([]);
    expect(client.updated).toHaveLength(1);
    const { update } = client.updated[0] as { update: PrUpdate };
    expect(update.title).toBe("feat(chromium): roll to 2.0.0");
    expect(update.draft).toBe(true);
    expect(update.addLabels).toEqual([NEEDS_REBASE_LABEL]);
    // Only the bot's commits are on the branch, so it is replaced, leased on the exact fetched head.
    expect(client.pushes).toEqual([{ branch: br, replaceSha: client.heads[br] }]);
    expect(JSON.parse(await gitSh(repo, ["show", `${br}:chromium/pin.json`])).tag).toBe("2.0.0");
    expect(await gitSh(repo, ["log", "-1", "--format=%B", br])).toContain("BDK-Roll-Bot: 2.0.0");
  });

  it("keeps a person's commits when a newer stable appears (no force push)", async () => {
    const { up, repo } = await fixture();
    const client = new FakeClient();
    const br = "chromium-roll/2.0.0";
    const human = await seedBranch(repo, br, {
      pinTag: "2.0.0",
      human: (dir) => {
        const f = join(dir, "series");
        writeFileSync(f, `${readFileSync(f, "utf8")}# maintainer note\n`);
      },
    });
    client.heads[br] = human;
    client.prs = [prFor(br)];
    await run(repo, up, client, { versionHistory: history("3.0.0"), dash: "fail" });
    // A plain push, on top of the fetched head.
    expect(client.pushes).toEqual([{ branch: br, replaceSha: null }]);
    await gitSh(repo, ["merge-base", "--is-ancestor", human, br]);
    expect(JSON.parse(await gitSh(repo, ["show", `${br}:chromium/pin.json`])).tag).toBe("3.0.0");
    expect(await gitSh(repo, ["show", `${br}:chromium/patches/series`])).toContain(
      "# maintainer note",
    );
    expect((await gitSh(repo, ["rev-parse", "--abbrev-ref", "HEAD"])).trim()).toBe("main");
  });

  it("treats a commit that only claims to be the bot's as a person's", async () => {
    const { up, repo } = await fixture();
    const client = new FakeClient();
    const br = "chromium-roll/2.0.0";
    // Bot identity and trailer, but it also edits a patch file.
    await gitSh(repo, ["checkout", "-q", "-b", br, "main"]);
    writeFileSync(join(repo, "chromium", "patches", "series"), "# edited\n");
    await gitSh(repo, ["add", "-A"]);
    await gitSh(repo, ["commit", "-q", "-m", "x", "-m", "BDK-Roll-Bot: 2.0.0"], BOT);
    client.heads[br] = (await gitSh(repo, ["rev-parse", "HEAD"])).trim();
    await gitSh(repo, ["checkout", "-q", "main"]);
    client.prs = [prFor(br)];
    await run(repo, up, client, { versionHistory: history("3.0.0"), dash: "fail" });
    expect(client.pushes).toEqual([{ branch: br, replaceSha: null }]);
    await gitSh(repo, ["merge-base", "--is-ancestor", client.heads[br] as string, br]);
  });

  it("leaves a roll PR that already pins a newer version than stable", async () => {
    const { up, repo } = await fixture();
    const client = new FakeClient();
    const br = "chromium-roll/3.0.0";
    client.heads[br] = await seedBranch(repo, br, { pinTag: "3.0.0" });
    client.prs = [prFor(br)];
    const out = await run(repo, up, client, { versionHistory: history("2.0.0"), dash: "fail" });
    expect(out.kind).toBe("up-to-date");
    expect(client.pushes).toEqual([]);
    expect(client.updated).toEqual([]);
  });

  it("reuses an existing roll branch with no PR instead of pushing a second one", async () => {
    const { up, repo } = await fixture();
    const client = new FakeClient();
    const br = "chromium-roll/1.0.1";
    client.heads[br] = await seedBranch(repo, br, { pinTag: "1.0.1" });
    const out = await run(repo, up, client, { versionHistory: history("1.0.1"), dash: "fail" });
    expect(out).toMatchObject({ kind: "rolled", prCreated: true });
    expect(client.pushes).toEqual([]);
    expect(client.created).toHaveLength(1);
  });

  it("recovers when the branch was pushed but the PR creation failed", async () => {
    const { up, repo } = await fixture();
    const client = new FakeClient();
    const br = "chromium-roll/1.0.1";
    let fail = true;
    const create = client.createPr.bind(client);
    client.createPr = async (pr: NewPr) => {
      if (fail) throw new Error("gh: transient");
      return create(pr);
    };
    const api = { versionHistory: history("1.0.1"), dash: "fail" } as const;
    await expect(run(repo, up, client, api)).rejects.toThrow(/transient/);
    // Stand in for the remote: the pushed branch now exists.
    client.heads[br] = (await gitSh(repo, ["rev-parse", br])).trim();
    fail = false;
    const out = await run(repo, up, client, api);
    expect(out).toMatchObject({ kind: "rolled", prCreated: true });
    expect(client.pushes).toHaveLength(1); // the retry pushed nothing
    expect(client.created).toHaveLength(1);
    expect((await gitSh(repo, ["rev-parse", br])).trim()).toBe(client.heads[br]);
  });

  it("does not re-push or reset a branch that already pins the version", async () => {
    const { up, repo } = await fixture();
    const client = new FakeClient();
    const br = "chromium-roll/1.0.1";
    client.heads[br] = await seedBranch(repo, br, { pinTag: "1.0.1" });
    client.prs = [prFor(br, { isDraft: true, labels: [NEEDS_REBASE_LABEL] })];
    await run(repo, up, client, { versionHistory: history("1.0.1"), dash: "fail" });
    expect(client.pushes).toEqual([]);
    const { update } = client.updated[0] as { update: PrUpdate };
    // Cleaner now, so the label goes; draft is a person's call, and stays.
    expect(update.draft).toBe(true);
    expect(update.removeLabels).toEqual([NEEDS_REBASE_LABEL]);
  });

  // A series a maintainer repaired so that it applies: here, by dropping every patch.
  const emptySeries = (dir: string) => {
    for (const f of readdirSync(dir)) if (f !== "series") rmSync(join(dir, f), { recursive: true });
    writeFileSync(join(dir, "series"), "");
  };

  it("judges a same-version rerun by the PR head's patches: repaired head is not downgraded", async () => {
    const { up, repo } = await fixture();
    const client = new FakeClient();
    const br = "chromium-roll/2.0.0";
    // On main, 2.0.0 conflicts. The PR head replaces the series with an empty one (applies cleanly).
    client.heads[br] = await seedBranch(repo, br, {
      pinTag: "2.0.0",
      human: (dir) => {
        emptySeries(dir);
      },
    });
    client.prs = [prFor(br, { isDraft: false, labels: [] })];
    const out = await run(repo, up, client, { versionHistory: history("2.0.0"), dash: "fail" });
    expect(out).toMatchObject({ kind: "rolled", draft: false });
    const { update } = client.updated[0] as { update: PrUpdate };
    expect(update.draft).toBe(false);
    expect(update.addLabels).toEqual([]);
    expect(update.body).not.toContain("does **not** apply");
    expect(client.pushes).toEqual([]);
  });

  it("removes needs-rebase from a repaired head but leaves the draft state alone", async () => {
    const { up, repo } = await fixture();
    const client = new FakeClient();
    const br = "chromium-roll/2.0.0";
    client.heads[br] = await seedBranch(repo, br, {
      pinTag: "2.0.0",
      human: emptySeries,
    });
    client.prs = [prFor(br, { isDraft: true, labels: [NEEDS_REBASE_LABEL] })];
    await run(repo, up, client, { versionHistory: history("2.0.0"), dash: "fail" });
    const { update } = client.updated[0] as { update: PrUpdate };
    expect(update.draft).toBe(true);
    expect(update.removeLabels).toEqual([NEEDS_REBASE_LABEL]);
    expect(update.addLabels).toEqual([]);
  });

  it("makes a ready PR a draft with the label when its own head conflicts", async () => {
    const { up, repo } = await fixture();
    const client = new FakeClient();
    const br = "chromium-roll/2.0.0";
    client.heads[br] = await seedBranch(repo, br, { pinTag: "2.0.0" });
    client.prs = [prFor(br, { isDraft: false, labels: [] })];
    const out = await run(repo, up, client, { versionHistory: history("2.0.0"), dash: "fail" });
    expect(out).toMatchObject({ kind: "rolled", draft: true });
    const { update } = client.updated[0] as { update: PrUpdate };
    expect(update.draft).toBe(true);
    expect(update.addLabels).toEqual([NEEDS_REBASE_LABEL]);
    expect(update.body).toContain("bdk-link-bdk.patch");
  });

  it("opens a stale-pin issue beyond the threshold, and updates it on the next run", async () => {
    const { up, repo } = await fixture();
    const api = {
      versionHistory: history("1.0.1"),
      dash: dash(["1.0.1", NOW.getTime() - 30 * DAY]),
    };
    const client = new FakeClient();
    const out = await run(repo, up, client, api);
    expect(out).toMatchObject({ issue: "opened", daysBehind: 30 });
    expect(client.issues[0]?.title).toBe("Chromium pin is 30 days behind stable");
    expect(client.issues[0]?.body).toContain("R4");

    client.issue = { number: 9, url: "u", title: "Chromium pin is 29 days behind stable" };
    const again = await run(repo, up, client, api);
    expect(again).toMatchObject({ issue: "updated" });
    expect(client.issues).toHaveLength(1);
    expect(client.issueEdits).toEqual([
      { number: 9, title: "Chromium pin is 30 days behind stable" },
    ]);
  });

  it("honours a configured threshold", async () => {
    const { up, repo } = await fixture();
    const client = new FakeClient();
    const out = await run(
      repo,
      up,
      client,
      { versionHistory: history("1.0.1"), dash: dash(["1.0.1", NOW.getTime() - 20 * DAY]) },
      { staleDays: 30 },
    );
    expect(out).toMatchObject({ issue: "none" });
    expect(client.issues).toEqual([]);
  });

  it("falls back to Chromium Dash when VersionHistory fails", async () => {
    const { up, repo } = await fixture();
    const client = new FakeClient();
    const out = await run(repo, up, client, {
      versionHistory: "fail",
      dash: dash(["1.0.1", NOW.getTime() - DAY], ["1.0.0", NOW.getTime() - 40 * DAY]),
    });
    expect(out).toMatchObject({ kind: "rolled", version: "1.0.1" });
  });

  it("fails clearly when both sources fail", async () => {
    const { up, repo } = await fixture();
    await expect(
      run(repo, up, new FakeClient(), { versionHistory: "fail", dash: "fail" }),
    ).rejects.toThrow(/could not detect/);
  });

  it("reports a tag the remote does not have yet, without touching GitHub", async () => {
    const { up, repo } = await fixture();
    const client = new FakeClient();
    const out = await run(repo, up, client, { versionHistory: history("9.0.0"), dash: "fail" });
    expect(out.kind).toBe("tag-missing");
    expect(client.calls).toEqual([]);
  });

  it("dry-run through the real plan path leaves git alone", async () => {
    const { up, repo } = await fixture();
    const client = new FakeClient();
    await run(
      repo,
      up,
      client,
      { versionHistory: history("1.0.1"), dash: "fail" },
      { dryRun: true },
    );
    expect((await gitSh(repo, ["branch", "--list", "chromium-roll/*"])).trim()).toBe("");
  });
});
