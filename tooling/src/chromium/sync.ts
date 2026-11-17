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
import { applyToCheckout, assertSafeToSync, recordBdkState } from "./patch-apply.js";

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
 * patch series. The caller must hold the checkout lock. Shallow, no history: `git-cache` is not
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
  if (existsSync(join(ctx.src, ".git")) && !options.force) {
    const { patches } = loadSeries(ctx.layout.patchesDir);
    await assertSafeToSync(ctx.src, pin.commit, patches);
  }

  const gclientArgs = [
    "sync",
    "--revision",
    `src@refs/tags/${pin.tag}`,
    "--no-history",
    "--shallow",
    "-D",
    "--force",
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
