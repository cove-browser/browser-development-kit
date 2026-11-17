import { existsSync, mkdirSync, readFileSync, statSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { describe, expect, it } from "vitest";
import { git } from "../git.js";
import {
  chromiumLayout,
  loadSeries,
  PatchFormatError,
  parsePatch,
  patchTouchedFiles,
} from "./common.js";
import { applySeries, PatchConflictError, UnsafeCheckoutError } from "./patch-apply.js";
import { checkPatchSeries, formatReport, sparsePattern } from "./patch-check.js";
import { exportSeries, patchFileName } from "./patch-export.js";
import { commitBdkHook, gitSh, makeSrc, makeUpstream, tmp } from "./test-support.js";

/** A src at tag 1.0.0 with one BDK commit, exported into a fresh patches directory. */
async function exported() {
  const up = await makeUpstream();
  const src = await makeSrc(up, "1.0.0");
  await commitBdkHook(src);
  const patchesDir = join(tmp("patches"), "patches");
  const result = await exportSeries({ srcDir: src, pinCommit: up.commit1, patchesDir });
  return { up, src, patchesDir, result };
}

describe("patch-export", () => {
  it("writes patch files and the series from commits on top of the pin", async () => {
    const { patchesDir, result } = await exported();
    expect(result.names).toEqual(["bdk-link-bdk.patch"]);
    expect(readFileSync(join(patchesDir, "series"), "utf8")).toBe("bdk-link-bdk.patch\n");
    const text = readFileSync(join(patchesDir, "bdk-link-bdk.patch"), "utf8");
    expect(text.startsWith("From 0000000000000000000000000000000000000000 ")).toBe(true);
    expect(text).not.toMatch(/^-- \n/m);
    expect(text).toContain("Used by: the BDK engine.");
  });

  it("is idempotent: a second export rewrites nothing", async () => {
    const { up, src, patchesDir } = await exported();
    const file = join(patchesDir, "bdk-link-bdk.patch");
    const before = { text: readFileSync(file, "utf8"), mtime: statSync(file).mtimeMs };
    await new Promise((r) => setTimeout(r, 20));
    const again = await exportSeries({ srcDir: src, pinCommit: up.commit1, patchesDir });
    expect(again.written).toEqual([]);
    expect(again.removed).toEqual([]);
    expect(readFileSync(file, "utf8")).toBe(before.text);
    expect(statSync(file).mtimeMs).toBe(before.mtime);
  });

  it("gives the same bytes from the commits that `git am` creates", async () => {
    const { up, patchesDir } = await exported();
    const other = await makeSrc(up, "1.0.0");
    const out = join(tmp("again"), "patches");
    mkdirSync(out);
    await applySeries({ srcDir: other, pinCommit: up.commit1, patchesDir });
    await exportSeries({ srcDir: other, pinCommit: up.commit1, patchesDir: out });
    expect(readFileSync(join(out, "bdk-link-bdk.patch"), "utf8")).toBe(
      readFileSync(join(patchesDir, "bdk-link-bdk.patch"), "utf8"),
    );
  });

  it("removes patch files that are no longer in the series", async () => {
    const { up, patchesDir } = await exported();
    writeFileSync(join(patchesDir, "stale.patch"), "x");
    const clean = await makeSrc(up, "1.0.0");
    const r = await exportSeries({ srcDir: clean, pinCommit: up.commit1, patchesDir });
    expect(r.names).toEqual([]);
    expect([...r.removed].sort()).toEqual(["bdk-link-bdk.patch", "stale.patch"]);
    expect(readFileSync(join(patchesDir, "series"), "utf8")).toBe("");
  });

  it("refuses a commit with an empty message body, naming the commit", async () => {
    const up = await makeUpstream();
    const src = await makeSrc(up, "1.0.0");
    await commitBdkHook(src, "bdk: no explanation\n");
    const head = (await git(src, ["rev-parse", "HEAD"])).trim().slice(0, 12);
    await expect(
      exportSeries({ srcDir: src, pinCommit: up.commit1, patchesDir: join(tmp("p"), "p") }),
    ).rejects.toThrow(new RegExp(`commit ${head}: .*empty commit message body`));
  });

  it("names files by subject and keeps duplicates apart", () => {
    expect(patchFileName("bdk: Link //bdk into the Mac framework", new Set())).toBe(
      "bdk-link-bdk-into-the-mac-framework.patch",
    );
    expect(patchFileName("same", new Set(["same.patch"]))).toBe("same-2.patch");
  });
});

describe("patch files", () => {
  it("rejects an empty body and non-format-patch input", () => {
    const empty =
      "From 0000000000000000000000000000000000000000 Mon Sep 17 00:00:00 2001\nSubject: [PATCH] x\n\n---\n a | 1 +\n\ndiff --git a/a b/a\n";
    expect(() => parsePatch("x.patch", "/x", empty)).toThrow(/empty commit message body/);
    expect(() => parsePatch("y.patch", "/y", "diff --git a/a b/a\n")).toThrow(PatchFormatError);
  });

  it("collects touched files including renames and paths with spaces", () => {
    const text = [
      "diff --git a/a/b.cc b/a/b.cc",
      "diff --git a/old name.h b/old name.h",
      "diff --git a/from.cc b/to.cc",
      "rename from from.cc",
      "rename to to.cc",
    ].join("\n");
    expect(patchTouchedFiles(text)).toEqual(["a/b.cc", "from.cc", "old name.h", "to.cc"]);
  });

  it("escapes sparse-checkout pattern characters", () => {
    expect(sparsePattern("a/[b]*.cc")).toBe("/a/\\[b\\]\\*.cc");
  });

  it("rejects series entries that escape the directory or are missing", () => {
    const dir = tmp("series");
    writeFileSync(join(dir, "series"), "../x.patch\n");
    expect(() => loadSeries(dir)).toThrow(/plain file name/);
    writeFileSync(join(dir, "series"), "missing.patch\n");
    expect(() => loadSeries(dir)).toThrow(/does not exist/);
  });
});

describe("patch-apply", () => {
  it("applies the series on the pin, deterministically, and then does nothing", async () => {
    const { up, patchesDir } = await exported();
    const a = await makeSrc(up, "1.0.0");
    const b = await makeSrc(up, "1.0.0");
    const first = await applySeries({ srcDir: a, pinCommit: up.commit1, patchesDir });
    const second = await applySeries({ srcDir: b, pinCommit: up.commit1, patchesDir });
    expect(first.status).toBe("applied");
    expect(first.head).toBe(second.head);
    expect(readFileSync(join(a, "chrome", "BUILD.gn"), "utf8")).toContain('"//bdk"');
    const mtime = statSync(join(a, "chrome", "BUILD.gn")).mtimeMs;
    const again = await applySeries({ srcDir: a, pinCommit: up.commit1, patchesDir });
    expect(again.status).toBe("already-applied");
    expect(statSync(join(a, "chrome", "BUILD.gn")).mtimeMs).toBe(mtime);
  });

  it("fails on a conflict, naming the patch and every conflicting file, and leaves no am in progress", async () => {
    const { up, patchesDir } = await exported();
    const src = await makeSrc(up, "2.0.0"); // 2.0.0 changed the line the patch edits
    const err = await applySeries({ srcDir: src, pinCommit: up.commit2, patchesDir }).catch(
      (e: unknown) => e,
    );
    expect(err).toBeInstanceOf(PatchConflictError);
    const e = err as PatchConflictError;
    expect(e.patch).toBe("bdk-link-bdk.patch");
    expect(e.files).toContain("chrome/BUILD.gn");
    expect(e.message).toContain("bdk-link-bdk.patch");
    expect(e.message).toContain("chrome/BUILD.gn");
    expect(existsSync(join(src, ".git", "rebase-apply"))).toBe(false);
  });

  it("refuses to reset a dirty tree unless forced", async () => {
    const { up, patchesDir } = await exported();
    const src = await makeSrc(up, "1.0.0");
    writeFileSync(join(src, "base", "unrelated.cc"), "// edited\n");
    await expect(applySeries({ srcDir: src, pinCommit: up.commit1, patchesDir })).rejects.toThrow(
      UnsafeCheckoutError,
    );
    expect(readFileSync(join(src, "base", "unrelated.cc"), "utf8")).toBe("// edited\n");
    const r = await applySeries({ srcDir: src, pinCommit: up.commit1, patchesDir, force: true });
    expect(r.status).toBe("applied");
    expect(readFileSync(join(src, "base", "unrelated.cc"), "utf8")).toContain("unrelated");
  });

  it("refuses to drop commits that are not in the series", async () => {
    const { up, src, patchesDir } = await exported();
    // src holds a commit that is already exported; add another that is not.
    writeFileSync(join(src, "base", "unrelated.cc"), "// new work\n");
    await gitSh(src, ["commit", "-qam", "wip\n\nnot exported yet"]);
    await expect(applySeries({ srcDir: src, pinCommit: up.commit1, patchesDir })).rejects.toThrow(
      /patch-export/,
    );
  });
});

describe("patch-check", () => {
  it("passes against the pinned tag and reports each patch", async () => {
    const { up, patchesDir } = await exported();
    const r = await checkPatchSeries({
      remote: up.dir,
      tag: "1.0.0",
      expectCommit: up.commit1,
      patchesDir,
    });
    expect(r.ok).toBe(true);
    expect(r.commit).toBe(up.commit1);
    expect(r.files).toEqual(["chrome/BUILD.gn", "chrome/app/framework.exports"]);
    expect(r.patches).toMatchObject([{ patch: "bdk-link-bdk.patch", status: "applied" }]);
  });

  it("fails a deliberately conflicting patch with the file named (T-CHR-03 acceptance)", async () => {
    const { up, patchesDir } = await exported();
    const r = await checkPatchSeries({ remote: up.dir, tag: "2.0.0", patchesDir });
    expect(r.ok).toBe(false);
    expect(r.patches[0]).toMatchObject({ patch: "bdk-link-bdk.patch", status: "conflict" });
    expect(r.patches[0]?.files).toContain("chrome/BUILD.gn");
    expect(formatReport(r)).toMatch(
      /CONFLICT bdk-link-bdk\.patch[\s\S]*conflicting file: chrome\/BUILD\.gn/,
    );
  });

  it("only checks out the files the series touches", async () => {
    const { up, patchesDir } = await exported();
    const workDir = tmp("work");
    await checkPatchSeries({ remote: up.dir, tag: "1.0.0", patchesDir, workDir });
    expect(existsSync(join(workDir, "src", "chrome", "BUILD.gn"))).toBe(true);
    expect(existsSync(join(workDir, "src", "base", "unrelated.cc"))).toBe(false);
  });

  it("flags a tag whose commit is not the pin", async () => {
    const { up, patchesDir } = await exported();
    const r = await checkPatchSeries({
      remote: up.dir,
      tag: "2.0.0",
      expectCommit: up.commit1,
      patchesDir,
    });
    expect(r.ok).toBe(false);
    expect(r.patches[0]?.detail).toMatch(/pin says/);
  });

  it("succeeds on an empty series without contacting the remote", async () => {
    const patchesDir = tmp("empty");
    writeFileSync(join(patchesDir, "series"), "# nothing yet\n");
    const r = await checkPatchSeries({ remote: "/nonexistent/remote", tag: "1.0.0", patchesDir });
    expect(r.ok).toBe(true);
    expect(r.patches).toEqual([]);
  });

  it("reports an invalid (empty-body) patch instead of throwing", async () => {
    const patchesDir = tmp("bad");
    writeFileSync(join(patchesDir, "series"), "a.patch\n");
    writeFileSync(
      join(patchesDir, "a.patch"),
      "From 0000000000000000000000000000000000000000 Mon Sep 17 00:00:00 2001\nSubject: [PATCH] a\n\n---\n",
    );
    const r = await checkPatchSeries({ remote: "/nonexistent/remote", tag: "1.0.0", patchesDir });
    expect(r.ok).toBe(false);
    expect(r.patches[0]).toMatchObject({ status: "invalid" });
  });

  it("uses the repository layout helpers", () => {
    expect(chromiumLayout("/r").seriesFile).toBe("/r/chromium/patches/series");
  });
});
