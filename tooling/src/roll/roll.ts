// Chromium roll bot CLI: `pnpm exec tsx tooling/src/roll/roll.ts [--dry-run] [--stale-days N] [--remote R] [--base B]`.
import { parseArgs } from "node:util";
import { DEFAULT_PATCH_CHECK_REMOTE, fail, isMain, repoRoot } from "../chromium/common.js";
import { DryRunClient, GhCliClient } from "./github.js";
import { DEFAULT_STALE_DAYS, roll } from "./run.js";

async function main(): Promise<void> {
  const { values } = parseArgs({
    args: process.argv.slice(2),
    options: {
      "dry-run": { type: "boolean", default: false },
      "stale-days": { type: "string" },
      remote: { type: "string" },
      base: { type: "string" },
    },
    strict: true,
  });
  const staleDays = Number(
    values["stale-days"] ?? process.env.BDK_ROLL_STALE_DAYS ?? DEFAULT_STALE_DAYS,
  );
  if (!Number.isFinite(staleDays) || staleDays < 0)
    fail("--stale-days must be a non-negative number.");
  const root = repoRoot();
  const dryRun = values["dry-run"] === true;
  const say = (l: string) => console.log(l);
  const outcome = await roll({
    repoRoot: root,
    remote: values.remote ?? process.env.BDK_PATCH_CHECK_REMOTE ?? DEFAULT_PATCH_CHECK_REMOTE,
    client: dryRun ? new DryRunClient(say) : new GhCliClient(root),
    fetch: (url) => fetch(url, { headers: { accept: "application/json" } }),
    staleDays,
    dryRun,
    ...(values.base === undefined ? {} : { base: values.base }),
    log: say,
  });
  if (process.env.GITHUB_STEP_SUMMARY && outcome.kind === "rolled") {
    const { appendFileSync } = await import("node:fs");
    appendFileSync(
      process.env.GITHUB_STEP_SUMMARY,
      `Chromium roll: ${outcome.title}${outcome.draft ? " (draft, patches do not apply)" : ""}: ${outcome.prUrl}\n`,
    );
  }
}

if (isMain(import.meta.url)) {
  await main().catch((e: unknown) => fail(e instanceof Error ? e.message : String(e)));
}
