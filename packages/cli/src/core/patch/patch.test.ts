import { mkdir, readdir, readFile, writeFile } from "node:fs/promises";
import { join } from "node:path";
import { exportSeries } from "@bdk/tooling/dist/chromium/patch-export.js";
import {
  commitBdkHook,
  gitSh,
  makeSrc,
  makeUpstream,
  type Upstream,
} from "@bdk/tooling/dist/chromium/test-support.js";
import { LockBusyError } from "@bdk/tooling/dist/lock.js";
import { describe, expect, it } from "vitest";
import { memoryAssets, tempDir, testContext } from "../../test-support.js";
import type { Context } from "../context.js";
import type { Diagnostic } from "../diagnostics.js";
import { ExitCode } from "../exit-codes.js";
import { run } from "../run.js";

const SOURCE_TOML = '[browser]\nname = "Demo"\n\n[runtime]\nmode = "source"\n';

interface Fixture {
  up: Upstream;
  project: string;
  home: string;
  src: string;
  ctx: (over?: Partial<Context>) => ReturnType<typeof testContext>;
}

/**
 * A fake upstream Chromium (tags 1.0.0 and 2.0.0), BDK's series of one patch exported from it and
 * embedded as the CLI embeds it, a managed checkout at the pin under BDK_HOME, and a source-mode project.
 * No real checkout is used.
 */
async function fixture(options: { checkout?: boolean; toml?: string } = {}): Promise<Fixture> {
  const up = await makeUpstream();
  const work = await makeSrc(up, "1.0.0");
  await commitBdkHook(work);
  const bdkPatches = join(await tempDir("bdk-series-"), "patches");
  await exportSeries({ srcDir: work, pinCommit: up.commit1, patchesDir: bdkPatches });
  const embedded: Record<string, string> = {
    "bdk-release.json": JSON.stringify({
      chromium: { tag: "1.0.0", commit: up.commit1 },
      abi: { version: 1, hash: "a".repeat(64) },
      runtime: null,
    }),
  };
  for (const name of await readdir(bdkPatches)) {
    embedded[`chromium/patches/${name}`] = await readFile(join(bdkPatches, name), "utf8");
  }
  const home = await tempDir("bdk-home-");
  const src = join(home, "chromium", "managed", "src");
  if (options.checkout !== false) {
    await mkdir(join(home, "chromium", "managed"), { recursive: true });
    await makeSrc(up, "1.0.0", src);
  }
  const project = await tempDir("bdk-app-");
  await writeFile(join(project, "bdk.toml"), options.toml ?? SOURCE_TOML);
  return {
    up,
    project,
    home,
    src,
    ctx: (over = {}) =>
      testContext({
        cwd: project,
        env: { ...process.env, BDK_HOME: home, BDK_PATCH_CHECK_REMOTE: up.dir },
        assets: memoryAssets(embedded),
        ...over,
      }),
  };
}

async function patch(f: Fixture, args: string[], over: Partial<Context> = {}) {
  const ctx = f.ctx(over);
  const code = await run(["patch", ...args, "--json"], ctx);
  const document = JSON.parse(ctx.out.join("")) as {
    result?: Record<string, unknown>;
    diagnostics: Diagnostic[];
  };
  return { code, ...document, ctx };
}

/** Commits a change of the application's in the managed checkout, as a developer would. */
async function commitAppChange(src: string, body = "The application needs it."): Promise<void> {
  await writeFile(join(src, "base", "unrelated.cc"), "int unrelated() { return 42; }\n");
  await gitSh(src, ["commit", "-qam", `app: return 42\n\n${body}`]);
}

describe("bdk patch: the source-mode workflow on a fixture checkout", () => {
  it("applies BDK's series, starts a patch, exports it into the project, and checks it", async () => {
    const f = await fixture();
    const status = await patch(f, ["status"]);
    expect(status.code).toBe(0);
    expect(status.result?.patches).toEqual([
      {
        name: "bdk-link-bdk.patch",
        origin: "bdk",
        files: ["chrome/BUILD.gn", "chrome/app/framework.exports"],
      },
    ]);
    expect(status.result?.checkout).toMatchObject({ applied: false });

    const applied = await patch(f, ["apply"]);
    expect(applied).toMatchObject({
      code: 0,
      result: { status: "applied", patches: ["bdk-link-bdk.patch"] },
    });
    expect(await readFile(join(f.src, "chrome", "BUILD.gn"), "utf8")).toContain('"//bdk"');

    const started = await patch(f, ["new", "return-42"]);
    expect(started.code).toBe(0);
    expect(started.result).toMatchObject({ status: "already-applied", name: "return-42" });

    await commitAppChange(f.src);
    const exported = await patch(f, ["export"]);
    expect(exported.code).toBe(0);
    const dir = join(f.project, "Chromium", "patches");
    expect(exported.result).toMatchObject({ directory: dir, names: ["app-return-42.patch"] });
    expect(await readFile(join(dir, "series"), "utf8")).toBe("app-return-42.patch\n");
    // Exporting again changes nothing.
    expect((await patch(f, ["export"])).result).toMatchObject({ written: [], removed: [] });

    const after = await patch(f, ["status"]);
    expect(after.result?.patches).toContainEqual({
      name: "app/app-return-42.patch",
      origin: "application",
      files: ["base/unrelated.cc"],
    });
    expect(after.result?.checkout).toMatchObject({ applied: true });

    const checked = await patch(f, ["check"]);
    expect(checked.code).toBe(0);
    expect(checked.result).toMatchObject({ ok: true, commit: f.up.commit1 });

    // Re-applying both series on a fresh reset gives the same tree.
    await gitSh(f.src, ["reset", "-q", "--hard", f.up.commit1]);
    expect((await patch(f, ["apply"])).result).toMatchObject({
      status: "applied",
      patches: ["bdk-link-bdk.patch", "app/app-return-42.patch"],
    });
    expect(await readFile(join(f.src, "base", "unrelated.cc"), "utf8")).toContain("42");
  });

  it("reports an application patch that conflicts, naming the project's file", async () => {
    const f = await fixture();
    await patch(f, ["apply"]);
    await commitAppChange(f.src);
    await patch(f, ["export"]);
    const file = join(f.project, "Chromium", "patches", "app-return-42.patch");
    await writeFile(file, (await readFile(file, "utf8")).replace("return 0", "return 7"));
    const checked = await patch(f, ["check"]);
    expect(checked.code).toBe(ExitCode.ProjectFailure);
    expect(checked.diagnostics).toContainEqual(
      expect.objectContaining({
        code: "patch.conflict",
        file,
        message: expect.stringContaining("base/unrelated.cc"),
      }),
    );
    await gitSh(f.src, ["reset", "-q", "--hard", f.up.commit1]);
    const applied = await patch(f, ["apply"]);
    expect(applied.code).toBe(ExitCode.ProjectFailure);
    expect(applied.diagnostics[0]).toMatchObject({ code: "patch.conflict", file });
  });

  it("tries the series against another tag with `rebase`", async () => {
    const f = await fixture();
    const rebased = await patch(f, ["rebase", "2.0.0"]);
    expect(rebased.code).toBe(ExitCode.ProjectFailure);
    expect(rebased.result).toMatchObject({ ok: false, tag: "2.0.0" });
    expect(rebased.diagnostics[0]).toMatchObject({
      code: "patch.conflict",
      message: expect.stringContaining("chrome/BUILD.gn"),
    });
  });

  it("refuses to export commits that do not start with BDK's series", async () => {
    const f = await fixture();
    await commitAppChange(f.src);
    const exported = await patch(f, ["export"]);
    expect(exported.code).toBe(ExitCode.ProjectFailure);
    expect(exported.diagnostics[0]?.code).toBe("patch.base_unknown");
  });

  it("refuses to apply over work that is not exported", async () => {
    const f = await fixture();
    await patch(f, ["apply"]);
    await commitAppChange(f.src);
    const applied = await patch(f, ["apply"]);
    expect(applied.code).toBe(ExitCode.ProjectFailure);
    expect(applied.diagnostics[0]?.code).toBe("patch.checkout_unsafe");
  });

  it("reads a list of patch files from [chromium] patches, and cannot export into one", async () => {
    const f = await fixture();
    await patch(f, ["apply"]);
    await commitAppChange(f.src);
    await patch(f, ["export"]);
    await mkdir(join(f.project, "mine"));
    await writeFile(
      join(f.project, "mine", "a.patch"),
      await readFile(join(f.project, "Chromium", "patches", "app-return-42.patch"), "utf8"),
    );
    await writeFile(
      join(f.project, "bdk.toml"),
      `${SOURCE_TOML}\n[chromium]\npatches = ["mine/a.patch"]\n`,
    );
    expect((await patch(f, ["check"])).result).toMatchObject({ ok: true });
    const exported = await patch(f, ["export"]);
    expect(exported.diagnostics[0]?.code).toBe("patch.list_not_exportable");
  });
});

describe("bdk patch: the checkout lock", () => {
  it("runs apply, new and export again under the lock, and status and check without it", async () => {
    const f = await fixture();
    const locked: string[][] = [];
    const over: Partial<Context> = {
      underCheckoutLock: (_paths, args) => {
        locked.push([...args]);
        return Promise.resolve(0);
      },
    };
    for (const sub of [["apply"], ["new", "x"], ["export"]]) {
      const ctx = f.ctx(over);
      expect(await run(["patch", ...sub], ctx)).toBe(0);
      // The child prints the report; the parent prints nothing.
      expect(ctx.out).toEqual([]);
    }
    expect(locked).toEqual([
      ["patch", "apply"],
      ["patch", "new", "x"],
      ["patch", "export"],
    ]);
    expect(await run(["patch", "status"], f.ctx(over))).toBe(0);
    expect(locked).toHaveLength(3);
  });

  it("fails at once, naming the holder, when another process holds the lock", async () => {
    const f = await fixture();
    const busy = await patch(f, ["apply"], {
      underCheckoutLock: () =>
        Promise.reject(
          new LockBusyError("managed", "/x.lock", {
            pid: 42,
            command: "autoninja",
            started: "now",
            worktree: "/w",
          }),
        ),
    });
    expect(busy.code).toBe(ExitCode.Environment);
    expect(busy.diagnostics[0]).toMatchObject({
      code: "patch.checkout_locked",
      message: expect.stringContaining("pid 42"),
    });
  });
});

describe("bdk patch: refusals", () => {
  it("is for source mode only", async () => {
    const f = await fixture({ toml: '[browser]\nname = "Demo"\n' });
    const r = await patch(f, ["status"]);
    expect(r.code).toBe(ExitCode.ProjectFailure);
    expect(r.diagnostics[0]?.code).toBe("patch.not_source_mode");
  });

  it("needs the managed checkout for apply, but not for check", async () => {
    const f = await fixture({ checkout: false });
    const r = await patch(f, ["apply"]);
    expect(r.code).toBe(ExitCode.Environment);
    expect(r.diagnostics[0]?.code).toBe("patch.checkout_missing");
    expect((await patch(f, ["check"])).code).toBe(0);
  });

  it("refuses the network with --offline", async () => {
    const f = await fixture();
    const ctx = f.ctx({ env: { BDK_HOME: f.home, PATH: process.env.PATH } });
    expect(await run(["patch", "check", "--offline"], ctx)).toBe(ExitCode.Environment);
    expect(ctx.err.join("")).toContain("[patch.offline]");
  });

  it.each([
    [[], "No patch command given"],
    [["frobnicate"], 'Unknown patch command "frobnicate"'],
    [["new"], "needs a name"],
    [["rebase"], "needs a Chromium tag"],
    [["status", "extra"], "Too many arguments"],
    [["new", "Bad Name"], "is not a patch name"],
  ])("%j is a usage error", async (args, message) => {
    const f = await fixture();
    const ctx = f.ctx({
      env: { BDK_HOME: f.home, BDK_CHECKOUT_LOCKED: "1", PATH: process.env.PATH },
    });
    expect(await run(["patch", ...args], ctx)).toBe(ExitCode.Usage);
    expect(ctx.err.join("")).toContain(message);
  });
});
