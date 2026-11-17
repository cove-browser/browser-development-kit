import { spawn } from "node:child_process";
import { existsSync, mkdirSync, readFileSync, statSync, utimesSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { describe, expect, it, vi } from "vitest";

vi.setConfig({ testTimeout: 120_000 });

import { git } from "../git.js";
import { repoRoot } from "./common.js";
import { exportSeries } from "./patch-export.js";
import {
  commitBdkHook,
  makeSandbox,
  makeSrc,
  makeUpstream,
  type Sandbox,
  type Upstream,
} from "./test-support.js";

const tsx = join(repoRoot(), "node_modules", ".bin", "tsx");
const script = (name: string) => join(repoRoot(), "tooling", "src", "chromium", `${name}.ts`);

interface Out {
  code: number | null;
  stdout: string;
  stderr: string;
}

function cli(name: string, args: string[], sb: Sandbox, env: NodeJS.ProcessEnv = {}): Promise<Out> {
  return new Promise((resolve, reject) => {
    const child = spawn(tsx, [script(name), ...args], { env: { ...sb.env, ...env }, cwd: sb.root });
    let stdout = "";
    let stderr = "";
    child.stdout.on("data", (d) => {
      stdout += d;
    });
    child.stderr.on("data", (d) => {
      stderr += d;
    });
    child.on("error", reject);
    child.on("close", (code) => resolve({ code, stdout, stderr }));
  });
}

const managedSrc = (sb: Sandbox) => join(sb.paths.chromiumManaged, "src");
const log = (sb: Sandbox) => readFileSync(sb.log, "utf8");

/** A sandbox whose series has the one BDK hook patch. */
async function sandboxWithPatch(): Promise<{ up: Upstream; sb: Sandbox }> {
  const up = await makeUpstream();
  const sb = makeSandbox(up);
  const src = await makeSrc(up, "1.0.0");
  await commitBdkHook(src);
  await exportSeries({
    srcDir: src,
    pinCommit: up.commit1,
    patchesDir: join(sb.repo, "chromium", "patches"),
  });
  return { up, sb };
}

describe("chromium:sync", () => {
  it("writes .gclient from the template, syncs to the pin, and applies the series", async () => {
    const { up, sb } = await sandboxWithPatch();
    const r = await cli("sync", [], sb);
    expect(r.code, r.stderr + r.stdout).toBe(0);
    expect(readFileSync(join(sb.paths.chromiumManaged, ".gclient"), "utf8")).toBe(
      'solutions = [{"url": "https://example.invalid/src.git"}]\n',
    );
    expect(log(sb)).toContain(
      "gclient sync --revision src@refs/tags/1.0.0 --no-history --shallow -D --force",
    );
    expect(readFileSync(join(managedSrc(sb), "chrome", "BUILD.gn"), "utf8")).toContain('"//bdk"');
    expect((await git(managedSrc(sb), ["rev-parse", "HEAD~1"])).trim()).toBe(up.commit1);
    // A second sync is consistent: still pin plus series.
    const again = await cli("sync", [], sb);
    expect(again.code, again.stderr).toBe(0);
    expect((await git(managedSrc(sb), ["rev-parse", "HEAD~1"])).trim()).toBe(up.commit1);
  });

  it("fails when src does not end up at the pinned commit", async () => {
    const up = await makeUpstream();
    const sb = makeSandbox(up, "1.0.0", up.commit2);
    const r = await cli("sync", [], sb);
    expect(r.code).toBe(1);
    expect(r.stderr).toMatch(/chromium\/pin\.json says/);
  });

  it("refuses to run while the checkout lock is held, naming the owner", async () => {
    const { sb } = await sandboxWithPatch();
    await cli("sync", [], sb);
    const { runUnderCheckoutLock } = await import("../lock.js");
    const held = runUnderCheckoutLock("managed", ["sleep", "2"], { paths: sb.paths });
    await new Promise((r) => setTimeout(r, 400));
    const r = await cli("sync", [], sb);
    expect(r.code).toBe(75);
    expect(r.stderr).toMatch(/Checkout "managed" is locked by pid \d+ \(sleep 2\)/);
    await held;
  });
});

describe("chromium:patch-apply", () => {
  it("resets and applies, and names the patch and file on conflict", async () => {
    const { sb } = await sandboxWithPatch();
    expect((await cli("sync", ["--no-patch"], sb)).code).toBe(0);
    const ok = await cli("patch-apply", [], sb);
    expect(ok.code, ok.stderr).toBe(0);
    expect(ok.stdout).toContain("patch-apply: applied");
    expect((await cli("patch-apply", [], sb)).stdout).toContain("already-applied");
  });

  it("exits non-zero naming the patch and the conflicting file when the pin moved", async () => {
    const up = await makeUpstream();
    const sb = makeSandbox(up, "2.0.0", up.commit2);
    const src = await makeSrc(up, "1.0.0");
    await commitBdkHook(src);
    await exportSeries({
      srcDir: src,
      pinCommit: up.commit1,
      patchesDir: join(sb.repo, "chromium", "patches"),
    });
    expect((await cli("sync", ["--no-patch"], sb)).code).toBe(0);
    const r = await cli("patch-apply", [], sb);
    expect(r.code).toBe(1);
    expect(r.stderr).toContain("bdk-link-bdk.patch");
    expect(r.stderr).toContain("chrome/BUILD.gn");
    expect(existsSync(join(managedSrc(sb), ".git", "rebase-apply"))).toBe(false);
    const build = await cli("build", [], sb);
    expect(build.code).toBe(1);
    expect(build.stderr).toContain("chrome/BUILD.gn");
  });

  it("refuses a dirty tree without --force", async () => {
    const { sb } = await sandboxWithPatch();
    await cli("sync", ["--no-patch"], sb);
    writeFileSync(join(managedSrc(sb), "base", "unrelated.cc"), "// local\n");
    const r = await cli("patch-apply", [], sb);
    expect(r.code).toBe(1);
    expect(r.stderr).toMatch(/uncommitted changes/);
    expect((await cli("patch-apply", ["--force"], sb)).code).toBe(0);
  });
});

describe("chromium:build", () => {
  it("applies patches, mounts //bdk, runs gn gen and autoninja with the ccache env, and verifies the link", async () => {
    const { sb } = await sandboxWithPatch();
    await cli("sync", [], sb);
    const r = await cli("build", ["--config", "dev"], sb, { BDK_CCACHE_MAXSIZE: "7G" });
    expect(r.code, r.stderr + r.stdout).toBe(0);
    const calls = log(sb);
    expect(calls).toContain("gn gen out/dev");
    expect(calls).toMatch(/autoninja -C out\/dev chrome start=\d+/);
    expect(calls).toContain(`CCACHE_DIR=${sb.paths.ccache}`);
    expect(calls).toContain("CCACHE_MAXSIZE=7G");
    expect(calls).toContain("CCACHE_COMPILERCHECK=content");
    expect(calls).toContain("CCACHE_NOHASHDIR=1");
    const src = managedSrc(sb);
    expect(readFileSync(join(src, "out", "dev", "args.gn"), "utf8")).toBe(
      'target_os = "mac"\nis_debug = true\n',
    );
    expect(readFileSync(join(src, "bdk", "bdk_overlay.cc"), "utf8")).toContain(
      "bdk_overlay_marker",
    );
    expect(r.stdout).toMatch(/timing: full build of chrome \(dev\)/);
    expect(r.stdout).toContain("_bdk_overlay_marker is exported");
    expect(await git(src, ["status", "--porcelain", "--untracked-files=all"])).not.toMatch(/bdk\//); // bdk/ is excluded from git
    // Output directories are per configuration.
    const rel = await cli("build", [], sb);
    expect(rel.code, rel.stderr).toBe(0);
    expect(existsSync(join(src, "out", "release", "args.gn"))).toBe(true);
    expect(readFileSync(join(src, "out", "release", "args.gn"), "utf8")).toContain(
      "is_debug = false",
    );
    expect(readFileSync(join(src, "out", "dev", "args.gn"), "utf8")).toContain("is_debug = true");
  });

  it("is incremental the second time and keeps mtimes of unchanged overlay files", async () => {
    const { sb } = await sandboxWithPatch();
    await cli("sync", [], sb);
    await cli("build", [], sb);
    const f = join(managedSrc(sb), "bdk", "bdk_overlay.cc");
    const old = new Date(Date.now() - 3_600_000);
    utimesSync(f, old, old);
    const patched = join(managedSrc(sb), "chrome", "BUILD.gn");
    const patchedMtime = statSync(patched).mtimeMs;
    const r = await cli("build", [], sb);
    expect(r.stdout).toMatch(/timing: incremental build/);
    expect(statSync(f).mtimeMs).toBe(old.getTime());
    expect(statSync(patched).mtimeMs).toBe(patchedMtime);
    expect(
      readFileSync(join(managedSrc(sb), "out", "release", "bdk-build-timings.jsonl"), "utf8")
        .trim()
        .split("\n"),
    ).toHaveLength(2);
  });

  it("follows the overlay of the building worktree: changed files recopied, deleted files removed", async () => {
    const { sb } = await sandboxWithPatch();
    await cli("sync", [], sb);
    await cli("build", [], sb);
    const overlay = join(sb.repo, "chromium", "bdk");
    writeFileSync(join(overlay, "bdk_overlay.cc"), "int bdk_overlay_marker() { return 2; }\n");
    mkdirSync(join(overlay, "sub"));
    writeFileSync(join(overlay, "sub", "extra.cc"), "// extra\n");
    await cli("build", [], sb);
    const dest = join(managedSrc(sb), "bdk");
    expect(readFileSync(join(dest, "bdk_overlay.cc"), "utf8")).toContain("return 2");
    expect(existsSync(join(dest, "sub", "extra.cc"))).toBe(true);
    const { rmSync } = await import("node:fs");
    rmSync(join(overlay, "sub"), { recursive: true });
    await cli("build", [], sb);
    expect(existsSync(join(dest, "sub", "extra.cc"))).toBe(false);
  });

  it("fails if the marker symbol is not in the framework", async () => {
    const { sb } = await sandboxWithPatch();
    await cli("sync", [], sb);
    const r = await cli("build", [], sb, { FAKE_NO_LINK: "1" });
    expect(r.code).toBe(1);
    expect(r.stderr).toMatch(
      /\/\/bdk is not linked into the framework: _bdk_overlay_marker is missing/,
    );
    expect((await cli("build", ["--no-verify"], sb, { FAKE_NO_LINK: "1" })).code).toBe(0);
  });

  it("refuses to build twice at once on one checkout", async () => {
    const { sb } = await sandboxWithPatch();
    await cli("sync", [], sb);
    const slow = cli("build", [], sb, { FAKE_BUILD_SECONDS: "2" });
    await new Promise((r) => setTimeout(r, 1500));
    const second = await cli("build", [], sb);
    expect(second.code).toBe(75);
    expect(second.stderr).toMatch(/locked by pid \d+/);
    expect((await slow).code).toBe(0);
  });
});

describe.skipIf(process.platform !== "darwin")("chromium:experiment", () => {
  it("clones the source tree without out/, builds concurrently with managed, and keeps outputs apart", async () => {
    const { sb } = await sandboxWithPatch();
    await cli("sync", [], sb);
    expect((await cli("build", [], sb)).code).toBe(0); // managed now has out/release
    const created = await cli("experiment", ["create", "e1"], sb);
    expect(created.code, created.stderr + created.stdout).toBe(0);
    const expRoot = sb.paths.chromiumExperiment("e1");
    expect(existsSync(join(expRoot, ".gclient"))).toBe(true);
    expect(existsSync(join(expRoot, "src", ".git"))).toBe(true);
    expect(existsSync(join(expRoot, "src", "out"))).toBe(false);
    expect(existsSync(join(managedSrc(sb), "out", "release"))).toBe(true);
    // The clone is copy-on-write, so it is a distinct tree.
    writeFileSync(join(expRoot, "src", "base", "unrelated.cc"), "// experiment edit\n");
    expect(readFileSync(join(managedSrc(sb), "base", "unrelated.cc"), "utf8")).not.toContain(
      "experiment edit",
    );

    const list = await cli("experiment", ["list"], sb);
    expect(list.stdout).toMatch(/^e1\tclone\tfrom managed\t.*idle/m);

    const [m, e] = await Promise.all([
      cli("build", [], sb, { FAKE_BUILD_SECONDS: "3" }),
      cli("build", ["--experiment", "e1", "--no-patch"], sb, { FAKE_BUILD_SECONDS: "3" }),
    ]);
    expect(m.code, m.stderr).toBe(0);
    expect(e.code, e.stderr).toBe(0);
    const calls = log(sb)
      .split("\n")
      .filter((l) => l.startsWith("autoninja") && l.includes("start="));
    const ends = log(sb)
      .split("\n")
      .filter((l) => l.startsWith("autoninja end="));
    // The last two builds overlapped in time.
    const startOf = (cwd: string) =>
      Number(
        calls
          .filter((l) => l.includes(`cwd=${cwd}`))
          .pop()
          ?.match(/start=(\d+)/)?.[1],
      );
    const endOf = (cwd: string) =>
      Number(
        ends
          .filter((l) => l.includes(`cwd=${cwd}`))
          .pop()
          ?.match(/end=(\d+)/)?.[1],
      );
    const mSrc = managedSrc(sb);
    const eSrc = join(expRoot, "src");
    expect(startOf(mSrc)).toBeLessThan(endOf(eSrc));
    expect(startOf(eSrc)).toBeLessThan(endOf(mSrc));
    // Each tree's output was built by its own tree and nothing crossed over.
    expect(readFileSync(join(mSrc, "out", "release", "built-by"), "utf8").trim()).toBe(mSrc);
    expect(readFileSync(join(eSrc, "out", "release", "built-by"), "utf8").trim()).toBe(eSrc);
    expect(existsSync(join(mSrc, "out", "release", "built-by"))).toBe(true);
    expect(readFileSync(join(eSrc, "bdk", "bdk_overlay.cc"), "utf8")).toContain(
      "bdk_overlay_marker",
    );

    // The experiment has its own lock: building it twice at once is refused, managed is unaffected.
    const slow = cli("build", ["--experiment", "e1", "--no-patch"], sb, {
      FAKE_BUILD_SECONDS: "2",
    });
    await new Promise((r) => setTimeout(r, 1500));
    const busy = await cli("build", ["--experiment", "e1", "--no-patch"], sb);
    expect(busy.code).toBe(75);
    expect(await cli("experiment", ["list"], sb)).toMatchObject({
      stdout: expect.stringMatching(/LOCKED/),
    });
    await slow;

    expect((await cli("experiment", ["create", "e1"], sb)).code).toBe(1); // exists
    const removed = await cli("experiment", ["remove", "e1"], sb);
    expect(removed.code, removed.stderr).toBe(0);
    expect(existsSync(expRoot)).toBe(false);
    expect(existsSync(join(mSrc, "out", "release", "built-by"))).toBe(true);
  });

  it("creates a git-worktree experiment with tools/git/worktree.py", async () => {
    const { sb } = await sandboxWithPatch();
    await cli("sync", [], sb);
    const tools = join(managedSrc(sb), "tools", "git");
    mkdirSync(tools, { recursive: true });
    // Mimics the behaviour of Chromium's script that matters here: the new root is
    // <parent of checkout>/<name> and it holds src (a worktree) and a copy of .gclient.
    writeFileSync(
      join(tools, "worktree.py"),
      `import argparse, os, shutil, subprocess
p = argparse.ArgumentParser()
p.add_argument("parent_repo"); p.add_argument("--name"); p.add_argument("--branch"); p.add_argument("--base"); p.add_argument("--light", action="store_true")
a = p.parse_args()
root = os.path.join(os.path.dirname(os.path.abspath(a.parent_repo)), a.name)
os.makedirs(root)
subprocess.check_call(["git", "worktree", "add", os.path.join(root, "src"), "-b", a.branch or a.name], cwd=os.path.join(a.parent_repo, "src"))
shutil.copy2(os.path.join(a.parent_repo, ".gclient"), os.path.join(root, ".gclient"))
`,
    );
    const r = await cli("experiment", ["create", "wt", "--method", "worktree"], sb);
    expect(r.code, r.stderr + r.stdout).toBe(0);
    const root = sb.paths.chromiumExperiment("wt");
    expect(existsSync(join(root, "src", ".git"))).toBe(true);
    expect(JSON.parse(readFileSync(join(root, ".bdk-experiment.json"), "utf8"))).toMatchObject({
      id: "wt",
      method: "worktree",
    });
    const rm = await cli("experiment", ["remove", "wt"], sb);
    expect(rm.code, rm.stderr).toBe(0);
    expect(existsSync(root)).toBe(false);
    expect((await git(managedSrc(sb), ["worktree", "list"])).trim().split("\n")).toHaveLength(1);
    expect((await git(managedSrc(sb), ["branch", "--list", "bdk-exp-wt"])).trim()).toBe("");
  });

  it("rejects invalid ids", async () => {
    const { sb } = await sandboxWithPatch();
    const r = await cli("experiment", ["create", "../x"], sb);
    expect(r.code).toBe(1);
    expect(r.stderr).toMatch(/Invalid experiment id/);
  });
});
