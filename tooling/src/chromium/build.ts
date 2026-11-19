import {
  appendFileSync,
  chmodSync,
  copyFileSync,
  existsSync,
  mkdirSync,
  readdirSync,
  readFileSync,
  rmSync,
  statSync,
  writeFileSync,
} from "node:fs";
import { join, relative } from "node:path";
import { parseArgs } from "node:util";
import { git, run } from "../git.js";
import { computeSourceIdentity, sourceKey } from "../runtime/identity.js";
import { BUILD_STAMP_NAME, clearBuildStamp, writeBuildStamp } from "../runtime/stamp.js";
import {
  type CheckoutContext,
  checkoutContext,
  checkoutFromFlag,
  chromiumLayout,
  depotToolsEnv,
  fail,
  isMain,
  LOCKED_FLAG,
  OVERLAY_MARKER_SYMBOL,
  relaunchUnderLock,
  seconds,
} from "./common.js";
import { applyToCheckout } from "./patch-apply.js";
import { ensureDepotTools } from "./sync.js";

// ---------------------------------------------------------------------------------------------
// Overlay

export interface MountResult {
  readonly copied: readonly string[];
  readonly removed: readonly string[];
  readonly unchanged: number;
}

function listFiles(dir: string, base = dir): string[] {
  const out: string[] = [];
  for (const e of readdirSync(dir, { withFileTypes: true })) {
    const p = join(dir, e.name);
    if (e.isDirectory()) out.push(...listFiles(p, base));
    else if (e.isFile()) out.push(relative(base, p));
  }
  return out.sort();
}

/** A directory outside `chromium/bdk` that is copied into the overlay under `prefix`. */
export interface OverlaySource {
  readonly dir: string;
  readonly prefix: string;
}

/**
 * What the engine compiles from `packages/core`: the runtime core it shares with the stub, the C ABI
 * headers and the generated ABI hash. They are copied to `//bdk/core/...`, so the engine builds the
 * same core sources as the stub and the conformance suite.
 */
export function coreOverlaySources(repoRoot: string): OverlaySource[] {
  const core = join(repoRoot, "packages", "core");
  return [
    { dir: join(core, "runtime"), prefix: "core/runtime" },
    { dir: join(core, "include", "bdk"), prefix: "core/include/bdk" },
    { dir: join(core, "generated"), prefix: "core/generated" },
  ];
}

/**
 * Makes `<src>/bdk` a content-identical copy of the overlay directory (plus each extra source under
 * its prefix). Only files whose content
 * differs are written, so they get a fresh mtime and Ninja or Siso rebuilds exactly them; files
 * that are the same keep their mtime and cause no rebuild. Files that no longer exist in the
 * overlay are removed. Also keeps `bdk/` out of `git status` in `src`.
 *
 * Why a copy and not a symlink: the managed checkout is shared by every worktree, and a symlink
 * out of the source root would point at one worktree's files, which can change or vanish
 * (branch switch, worktree removal) while a build reads them; Siso and GN also resolve and
 * stat paths relative to the source root. A copy made under the lock is self-contained. An
 * experiment cloned from the managed tree gets a real directory, not a dangling link. A copy
 * with fresh mtimes only on changed files also fixes a symlink's real weakness: switching the
 * link to an older file would not look newer than the object files, so it would not rebuild.
 */
export async function mountOverlay(
  srcDir: string,
  overlayDir: string,
  extra: readonly OverlaySource[] = [],
): Promise<MountResult> {
  const dest = join(srcDir, "bdk");
  mkdirSync(dest, { recursive: true });
  const sources = new Map<string, string>();
  for (const f of listFiles(overlayDir)) sources.set(f, join(overlayDir, f));
  for (const x of extra) {
    if (!existsSync(x.dir)) continue;
    for (const f of listFiles(x.dir)) {
      const rel = join(x.prefix, f);
      if (sources.has(rel))
        throw new Error(`overlay: ${rel} exists in chromium/bdk and in ${x.dir}`);
      sources.set(rel, join(x.dir, f));
    }
  }
  const want = [...sources.keys()].sort();
  const have = existsSync(dest) ? listFiles(dest) : [];
  const copied: string[] = [];
  let unchanged = 0;
  for (const f of want) {
    const from = sources.get(f) as string;
    const to = join(dest, f);
    if (existsSync(to) && readFileSync(from).equals(readFileSync(to))) {
      unchanged++;
      continue;
    }
    mkdirSync(join(to, ".."), { recursive: true });
    copyFileSync(from, to);
    chmodSync(to, statSync(from).mode & 0o777);
    copied.push(f);
  }
  const removed: string[] = [];
  const wanted = new Set(want);
  for (const f of have) {
    if (!wanted.has(f)) {
      rmSync(join(dest, f));
      removed.push(f);
    }
  }
  await excludeFromGit(srcDir, "/bdk/");
  return { copied, removed, unchanged };
}

async function excludeFromGit(srcDir: string, pattern: string): Promise<void> {
  const path = (
    await git(srcDir, ["rev-parse", "--path-format=absolute", "--git-path", "info/exclude"])
  ).trim();
  const text = existsSync(path) ? readFileSync(path, "utf8") : "";
  if (text.split("\n").includes(pattern)) return;
  mkdirSync(join(path, ".."), { recursive: true });
  writeFileSync(path, `${text}${text === "" || text.endsWith("\n") ? "" : "\n"}${pattern}\n`);
}

// ---------------------------------------------------------------------------------------------
// GN, ninja, verification

export type BuildConfig = "dev" | "release";

export function ccacheEnv(ctx: CheckoutContext, maxSize: string): NodeJS.ProcessEnv {
  return {
    CCACHE_DIR: ctx.paths.ccache,
    CCACHE_MAXSIZE: maxSize,
    CCACHE_COMPILERCHECK: "content",
    CCACHE_NOHASHDIR: "1",
  };
}

/** Copies the preset to `out/<config>/args.gn` unless it is already identical (so GN does not regenerate for nothing). */
export function writeArgs(
  ctx: CheckoutContext,
  config: BuildConfig,
): { outDir: string; changed: boolean } {
  const preset = join(ctx.layout.gnDir, `${config}.gn`);
  if (!existsSync(preset)) throw new Error(`No GN preset ${preset}`);
  const outDir = join(ctx.src, "out", config);
  const target = join(outDir, "args.gn");
  const text = readFileSync(preset, "utf8");
  mkdirSync(outDir, { recursive: true });
  if (existsSync(target) && readFileSync(target, "utf8") === text)
    return { outDir, changed: false };
  writeFileSync(target, text);
  return { outDir, changed: true };
}

/** `* Framework.framework/Versions/<v>/* Framework` binaries in an output directory. */
export function findFrameworkBinaries(outDir: string): string[] {
  if (!existsSync(outDir)) return [];
  const found: string[] = [];
  for (const e of readdirSync(outDir, { withFileTypes: true })) {
    if (!e.isDirectory() || !e.name.endsWith(".framework")) continue;
    const name = e.name.slice(0, -".framework".length);
    const versions = join(outDir, e.name, "Versions");
    if (!existsSync(versions)) continue;
    for (const v of readdirSync(versions)) {
      const bin = join(versions, v, name);
      if (v !== "Current" && existsSync(bin) && statSync(bin).isFile()) found.push(bin);
    }
  }
  return found.sort();
}

/**
 * Proves //bdk is linked into the framework: the exported symbol must be in the binary's symbol
 * table. GN has no view of the link result, and a `gn check` or `gn desc` only shows that the
 * dependency edge exists; `nm` on the product shows that the object really reached the binary.
 */
export async function frameworkExportsMarker(
  binary: string,
  env: NodeJS.ProcessEnv,
): Promise<boolean> {
  const nm = process.env.BDK_NM ?? "nm";
  const r = await run(nm, ["-gU", binary], { env });
  if (r.code !== 0) throw new Error(`${nm} -gU ${binary} failed: ${r.stderr.trim()}`);
  return r.stdout.split("\n").some((l) => l.trim().endsWith(` ${OVERLAY_MARKER_SYMBOL}`));
}

export interface BuildOptions {
  readonly config: BuildConfig;
  readonly targets: readonly string[];
  readonly ccacheMaxSize: string;
  readonly noPatch?: boolean;
  readonly noVerify?: boolean;
  readonly forcePatch?: boolean;
  readonly log?: (line: string) => void;
}

export interface BuildTimings {
  readonly kind: "full" | "incremental";
  readonly config: BuildConfig;
  readonly targets: readonly string[];
  readonly patchMs: number;
  readonly overlayMs: number;
  readonly gnMs: number;
  readonly ninjaMs: number;
  readonly totalMs: number;
  readonly at: string;
}

/** Targets whose build produces the framework, so the marker check applies. */
const FRAMEWORK_TARGETS = new Set([
  "chrome",
  "//chrome",
  "//chrome:chrome",
  "chrome_framework",
  "//chrome:chrome_framework",
]);

/** Targets that build the complete runtime (Chromium.app), so a build of them can be stamped. */
const RUNTIME_TARGETS = new Set(["chrome", "//chrome", "//chrome:chrome"]);

/**
 * Applies patches, mounts the overlay, runs `gn gen` with the preset, then `autoninja`, with
 * the ccache environment of repository-and-tooling.md section 7.2. The caller must hold the
 * checkout lock. Timings go to the log and to `out/<config>/bdk-build-timings.jsonl`.
 */
export async function buildCheckout(
  ctx: CheckoutContext,
  options: BuildOptions,
): Promise<BuildTimings> {
  const log = options.log ?? ((l: string) => console.log(l));
  const started = Date.now();
  if (!existsSync(join(ctx.src, ".git"))) {
    throw new Error(
      `${ctx.src} is not a checkout. Run chromium:sync${ctx.checkout === "managed" ? "" : " --experiment <id>"} first.`,
    );
  }
  await ensureDepotTools(ctx, log);
  const env = { ...depotToolsEnv(ctx.paths), ...ccacheEnv(ctx, options.ccacheMaxSize) };
  mkdirSync(ctx.paths.ccache, { recursive: true });
  const cc = await run("ccache", ["--version"], { env }).catch(() => ({ code: 1 }));
  if (cc.code !== 0)
    throw new Error(
      'ccache is not installed or not on PATH; the GN presets use cc_wrapper = "... ccache". Install it (brew install ccache).',
    );
  if (process.platform === "darwin") {
    // ANGLE compiles Metal shaders; Xcode 26+ ships the Metal Toolchain as a separate download.
    const xcrun = process.env.BDK_XCRUN ?? "xcrun";
    const metal = await run(xcrun, ["-f", "metal"], { env }).catch(() => ({ code: 1 }));
    if (metal.code !== 0)
      throw new Error(
        "The Metal Toolchain is missing (`xcrun -f metal` failed); ANGLE needs it. Install it with: xcodebuild -downloadComponent MetalToolchain",
      );
  }

  // The build is tied to the inputs it starts from. The stamp is gone while the output is in flux and
  // is written only after a successful, verified build (the packager refuses an out/ without it).
  const identity = await computeSourceIdentity({
    repoRoot: ctx.layout.repoRoot,
    config: options.config,
  });
  await clearBuildStamp(join(ctx.src, "out", options.config));

  let t = Date.now();
  if (!options.noPatch) {
    await applyToCheckout(ctx, { ...(options.forcePatch ? { force: true } : {}), log });
  }
  const patchMs = Date.now() - t;

  t = Date.now();
  const mounted = await mountOverlay(
    ctx.src,
    ctx.layout.overlayDir,
    coreOverlaySources(ctx.layout.repoRoot),
  );
  log(
    `overlay: //bdk mounted from ${ctx.layout.overlayDir} (${mounted.copied.length} copied, ${mounted.removed.length} removed, ${mounted.unchanged} unchanged)`,
  );
  writeFileSync(
    join(ctx.root, ".bdk-overlay.json"),
    `${JSON.stringify({ worktree: ctx.layout.repoRoot, overlay: ctx.layout.overlayDir, at: new Date().toISOString() })}\n`,
  );
  const overlayMs = Date.now() - t;

  const { outDir } = writeArgs(ctx, options.config);
  const kind = existsSync(join(outDir, "build.ninja")) ? "incremental" : "full";

  t = Date.now();
  const gn = await run("gn", ["gen", join("out", options.config)], {
    cwd: ctx.src,
    env,
    inherit: true,
  });
  if (gn.code !== 0) throw new Error(`gn gen failed (exit ${gn.code})`);
  const gnMs = Date.now() - t;

  t = Date.now();
  log(`build: autoninja -C out/${options.config} ${options.targets.join(" ")} (${kind})`);
  const ninja = await run("autoninja", ["-C", join("out", options.config), ...options.targets], {
    cwd: ctx.src,
    env,
    inherit: true,
  });
  if (ninja.code !== 0) throw new Error(`autoninja failed (exit ${ninja.code})`);
  const ninjaMs = Date.now() - t;

  const verify = !options.noVerify && options.targets.some((x) => FRAMEWORK_TARGETS.has(x));
  if (verify) {
    const binaries = findFrameworkBinaries(outDir);
    if (binaries.length === 0)
      throw new Error(
        `Built ${options.targets.join(" ")} but found no *.framework binary in ${outDir} to check for //bdk.`,
      );
    let linked = false;
    for (const b of binaries) linked = linked || (await frameworkExportsMarker(b, env));
    if (!linked) {
      throw new Error(
        `//bdk is not linked into the framework: ${OVERLAY_MARKER_SYMBOL} is missing from ${binaries.join(", ")}. ` +
          "Check that the first patch in chromium/patches applied (chrome_framework public_deps and chrome/app/framework.exports).",
      );
    }
    log(
      `verify: ${OVERLAY_MARKER_SYMBOL} is exported by ${relative(ctx.src, binaries[0] as string)}`,
    );
  }

  // Only a verified build of the whole runtime may bless out/ for packaging. A partial build
  // (e.g. --targets base) leaves older runtime files on disk that do not match these inputs.
  if (verify && options.targets.some((x) => RUNTIME_TARGETS.has(x))) {
    await writeBuildStamp(outDir, identity);
    log(`stamp: ${BUILD_STAMP_NAME} written for source key ${sourceKey(identity)}`);
  } else {
    log(
      `stamp: not written (${options.noVerify ? "--no-verify" : "partial build"}); build chrome with verification before packaging`,
    );
  }

  const timings: BuildTimings = {
    kind,
    config: options.config,
    targets: options.targets,
    patchMs,
    overlayMs,
    gnMs,
    ninjaMs,
    totalMs: Date.now() - started,
    at: new Date().toISOString(),
  };
  appendFileSync(join(outDir, "bdk-build-timings.jsonl"), `${JSON.stringify(timings)}\n`);
  log(
    `timing: ${kind} build of ${options.targets.join(" ")} (${options.config}): patch ${seconds(patchMs)}, overlay ${seconds(overlayMs)}, gn gen ${seconds(gnMs)}, ninja ${seconds(ninjaMs)}, total ${seconds(timings.totalMs)}`,
  );
  return timings;
}

async function main(): Promise<void> {
  const args = process.argv.slice(2);
  const { values, positionals } = parseArgs({
    args,
    allowPositionals: true,
    options: {
      experiment: { type: "string" },
      config: { type: "string", default: "release" },
      targets: { type: "string" },
      "ccache-maxsize": { type: "string" },
      "no-patch": { type: "boolean", default: false },
      "no-verify": { type: "boolean", default: false },
      force: { type: "boolean", default: false },
      [LOCKED_FLAG.slice(2)]: { type: "boolean", default: false },
    },
    strict: true,
  });
  if (values.config !== "dev" && values.config !== "release")
    fail(`--config must be dev or release, got "${values.config}"`);
  const checkout = checkoutFromFlag(values.experiment);
  if (!values.locked) process.exit(await relaunchUnderLock(checkout, import.meta.url, args));
  const targets = [...(values.targets ? values.targets.split(",") : []), ...positionals].filter(
    Boolean,
  );
  try {
    await buildCheckout(checkoutContext(checkout, undefined, chromiumLayout()), {
      config: values.config as BuildConfig,
      targets: targets.length > 0 ? targets : ["chrome"],
      ccacheMaxSize: values["ccache-maxsize"] ?? process.env.BDK_CCACHE_MAXSIZE ?? "50G",
      noPatch: values["no-patch"],
      noVerify: values["no-verify"],
      forcePatch: values.force,
    });
  } catch (e) {
    fail(e instanceof Error ? e.message : String(e));
  }
}

if (isMain(import.meta.url)) await main();
