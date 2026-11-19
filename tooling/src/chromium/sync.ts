import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { parseArgs } from "node:util";
import { git, run } from "../git.js";
import {
  type CheckoutContext,
  checkoutContext,
  checkoutFromFlag,
  chromiumLayout,
  DEFAULT_CHROMIUM_URL,
  DEFAULT_DEPOT_TOOLS_URL,
  depotToolsDir,
  depotToolsEnv,
  fail,
  isMain,
  LOCKED_FLAG,
  loadSeries,
  readPin,
  relaunchUnderLock,
  seconds,
} from "./common.js";
import { applyToCheckout, assertSafeToSync, recordBdkState, seriesState } from "./patch-apply.js";

/** Renders `chromium/gclient/gclient.template`: `{{chromium_url}}` is the only placeholder. */
export function renderGclient(template: string, chromiumUrl: string): string {
  return template.replaceAll("{{chromium_url}}", chromiumUrl);
}

/** Writes `.gclient` into the checkout root from the template if it is absent. Returns whether it wrote. */
export function ensureGclientFile(
  ctx: CheckoutContext,
  chromiumUrl = process.env.BDK_CHROMIUM_URL ?? DEFAULT_CHROMIUM_URL,
): boolean {
  const target = join(ctx.root, ".gclient");
  if (existsSync(target)) return false;
  const template = readFileSync(join(ctx.layout.gclientDir, "gclient.template"), "utf8");
  mkdirSync(ctx.root, { recursive: true });
  writeFileSync(target, renderGclient(template, chromiumUrl));
  return true;
}

/** Clones depot_tools into `$BDK_HOME/depot_tools` if it is missing. */
export async function ensureDepotTools(
  ctx: CheckoutContext,
  log: (l: string) => void,
): Promise<string> {
  const dir = depotToolsDir(ctx.paths);
  if (existsSync(join(dir, "gclient"))) return dir;
  const url = process.env.BDK_DEPOT_TOOLS_URL ?? DEFAULT_DEPOT_TOOLS_URL;
  log(`sync: cloning depot_tools from ${url} into ${dir}`);
  mkdirSync(ctx.paths.home, { recursive: true });
  const r = await run("git", ["clone", "--quiet", url, dir], { inherit: true });
  if (r.code !== 0) throw new Error(`git clone of depot_tools failed (exit ${r.code})`);
  return dir;
}

export interface SyncOptions {
  readonly force?: boolean;
  /** Skip applying the patch series after the sync. */
  readonly noPatch?: boolean;
  /** Pass --nohooks to gclient (the default runs hooks). */
  readonly noHooks?: boolean;
  readonly log?: (line: string) => void;
}

/**
 * `gclient sync` the checkout to the pin, check that `src` is at the pinned commit, then apply the
 * patch series. If `src` is already the pin plus exactly the series (and neither `force` nor
 * `noPatch` is set), it stays where it is and only the dependencies are synced, so an unchanged
 * checkout costs the next build nothing. The caller must hold the checkout lock. Shallow, no history: `git-cache` is not
 * used (deviation from repository-and-tooling.md section 7, which lists it for full clones).
 */
export async function syncCheckout(ctx: CheckoutContext, options: SyncOptions = {}): Promise<void> {
  const log = options.log ?? ((l: string) => console.log(l));
  const pin = readPin(ctx.layout);
  const started = Date.now();
  await ensureDepotTools(ctx, log);
  mkdirSync(ctx.root, { recursive: true });
  if (ensureGclientFile(ctx)) log(`sync: wrote ${join(ctx.root, ".gclient")} from the template`);

  // gclient --force would drop uncommitted changes and commits BDK did not produce; refuse unless told to.
  // If src is already the pin plus exactly the series, keep it: gclient is handed HEAD itself, so it
  // syncs the dependencies and runs hooks without rewriting the patched files. Resetting to the pin
  // and re-applying would give them new mtimes and cost the next build a regen and a relink.
  let keepHead: string | undefined;
  if (existsSync(join(ctx.src, ".git")) && !options.force) {
    const { patches } = loadSeries(ctx.layout.patchesDir);
    await assertSafeToSync(ctx.src, pin.commit, patches);
    if (!options.noPatch && patches.length > 0) {
      const state = await seriesState(ctx.src, pin.commit, patches);
      if (state.applied) keepHead = state.head;
    }
  }

  // gclient skips its own fetch only when --revision is a SHA it already has (a tag never counts),
  // and that fetch is a full `git fetch origin` of every branch: over 20 minutes against Chromium,
  // twice ending in an HTTP 502. So fetch just the pinned tag at depth 1 (under 4 minutes) and, once
  // it resolves to the pinned commit, hand gclient the SHA. Best effort: on failure gclient fetches.
  let revision = keepHead ?? `refs/tags/${pin.tag}`;
  if (keepHead === undefined && existsSync(join(ctx.src, ".git"))) {
    const resolveTag = async () =>
      (
        await run("git", ["rev-parse", "--verify", "--quiet", `${revision}^{commit}`], {
          cwd: ctx.src,
        })
      ).stdout.trim();
    let tagged = await resolveTag();
    if (tagged !== pin.commit) {
      log(`sync: fetching ${pin.tag} at depth 1`);
      const r = await run(
        "git",
        ["fetch", "--depth=1", "--no-tags", "origin", `+${revision}:${revision}`],
        { cwd: ctx.src, inherit: true },
      );
      if (r.code !== 0) log(`sync: depth-1 fetch of ${pin.tag} failed; gclient will fetch instead`);
      tagged = await resolveTag();
    }
    if (tagged === pin.commit) revision = pin.commit;
  }

  const gclientArgs = [
    "sync",
    "--revision",
    `src@${revision}`,
    "--no-history",
    "--shallow",
    "-D",
    // gclient --force runs `git reset --hard HEAD` in every dependency, even one already at its
    // revision. That recreated all 466,465 files of the dependencies and made the next build
    // recompile everything, so pass it only when asked. Without it gclient still moves
    // dependencies whose revision changed, and -D still removes clean ones DEPS no longer lists.
    ...(options.force ? ["--force"] : []),
    ...(options.noHooks ? ["--nohooks"] : []),
  ];
  log(`sync: gclient ${gclientArgs.join(" ")}`);
  const r = await run("gclient", gclientArgs, {
    cwd: ctx.root,
    env: depotToolsEnv(ctx.paths),
    inherit: true,
  });
  if (r.code !== 0) throw new Error(`gclient sync failed (exit ${r.code})`);

  const head = (await git(ctx.src, ["rev-parse", "HEAD"])).trim();
  if (keepHead !== undefined) {
    if (head !== keepHead) {
      throw new Error(
        `After gclient sync, ${ctx.src} is at ${head} but it was asked to stay at ${keepHead} (${pin.tag} plus the series).`,
      );
    }
    await recordBdkState(ctx.src, pin.commit, head);
    log(`sync: src is already ${pin.tag} plus the series (${head.slice(0, 12)}); left untouched`);
    log(`sync: done in ${seconds(Date.now() - started)}`);
    return;
  }
  if (head !== pin.commit) {
    throw new Error(
      `After gclient sync, ${ctx.src} is at ${head} but chromium/pin.json says ${pin.commit} (${pin.tag}). ` +
        "The tag may have moved, or the pin is wrong.",
    );
  }
  await recordBdkState(ctx.src, pin.commit, head);
  log(`sync: src is at the pin ${pin.tag} (${pin.commit.slice(0, 12)})`);

  if (!options.noPatch) {
    await applyToCheckout(ctx, {
      ...(options.force === undefined ? {} : { force: options.force }),
      log,
    });
  }
  log(`sync: done in ${seconds(Date.now() - started)}`);
}

async function main(): Promise<void> {
  const args = process.argv.slice(2);
  const { values } = parseArgs({
    args,
    options: {
      experiment: { type: "string" },
      force: { type: "boolean", default: false },
      "no-patch": { type: "boolean", default: false },
      "no-hooks": { type: "boolean", default: false },
      [LOCKED_FLAG.slice(2)]: { type: "boolean", default: false },
    },
    strict: true,
  });
  const checkout = checkoutFromFlag(values.experiment);
  if (!values.locked) process.exit(await relaunchUnderLock(checkout, import.meta.url, args));
  try {
    await syncCheckout(checkoutContext(checkout, undefined, chromiumLayout()), {
      force: values.force,
      noPatch: values["no-patch"],
      noHooks: values["no-hooks"],
    });
  } catch (e) {
    fail(e instanceof Error ? e.message : String(e));
  }
}

if (isMain(import.meta.url)) await main();
