// Fixtures for the chromium command tests: a fake upstream repository, a fake BDK repository tree
// and a fake depot_tools whose tools record their calls. Nothing here touches a real checkout.
import {
  chmodSync,
  mkdirSync,
  mkdtempSync,
  readFileSync,
  realpathSync,
  writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { bdkIdentityArgs, run } from "../git.js";
import { type BdkPaths, bdkPaths } from "../paths.js";

export async function sh(
  cwd: string,
  cmd: string,
  args: string[],
  env: NodeJS.ProcessEnv = {},
): Promise<string> {
  const r = await run(cmd, args, { cwd, env: { ...process.env, ...env } });
  if (r.code !== 0) throw new Error(`${cmd} ${args.join(" ")} failed: ${r.stderr}${r.stdout}`);
  return r.stdout;
}

export function tmp(prefix: string): string {
  return realpathSync(mkdtempSync(join(tmpdir(), `bdk-test-${prefix}-`)));
}

const GIT_ENV = {
  GIT_AUTHOR_NAME: "Upstream",
  GIT_AUTHOR_EMAIL: "up@example.invalid",
  GIT_COMMITTER_NAME: "Upstream",
  GIT_COMMITTER_EMAIL: "up@example.invalid",
  GIT_AUTHOR_DATE: "2026-01-01T00:00:00+0000",
  GIT_COMMITTER_DATE: "2026-01-01T00:00:00+0000",
  GIT_CONFIG_GLOBAL: "/dev/null",
  GIT_CONFIG_NOSYSTEM: "1",
};

export function gitSh(cwd: string, args: string[], env: NodeJS.ProcessEnv = {}): Promise<string> {
  return sh(cwd, "git", args, { ...GIT_ENV, ...env });
}

export const BUILD_GN_V1 = `import("//build/config/compiler/compiler.gni")

group("chrome") {
  deps = [ ":chrome_framework" ]
}

mac_framework_bundle("chrome_framework") {
  public_deps = [ ":chrome_dll" ]
}
`;
export const BUILD_GN_V2 = BUILD_GN_V1.replace(
  'public_deps = [ ":chrome_dll" ]',
  'public_deps = [ ":chrome_dll_renamed" ]',
);
export const EXPORTS_V1 = "_ChromeMain\n";

export interface Upstream {
  readonly dir: string;
  readonly commit1: string;
  readonly commit2: string;
}

/** A fake upstream with tag `1.0.0` and tag `2.0.0`; 2.0.0 edits the line the fixture patch edits. */
export async function makeUpstream(): Promise<Upstream> {
  const dir = tmp("upstream");
  await gitSh(dir, ["init", "-q", "-b", "main"]);
  mkdirSync(join(dir, "chrome", "app"), { recursive: true });
  mkdirSync(join(dir, "base"), { recursive: true });
  writeFileSync(join(dir, "chrome", "BUILD.gn"), BUILD_GN_V1);
  writeFileSync(join(dir, "chrome", "app", "framework.exports"), EXPORTS_V1);
  writeFileSync(join(dir, "base", "unrelated.cc"), "int unrelated() { return 0; }\n");
  await gitSh(dir, ["add", "-A"]);
  await gitSh(dir, ["commit", "-q", "-m", "one"]);
  await gitSh(dir, ["tag", "1.0.0"]);
  const commit1 = (await gitSh(dir, ["rev-parse", "HEAD"])).trim();
  writeFileSync(join(dir, "chrome", "BUILD.gn"), BUILD_GN_V2);
  await gitSh(dir, ["commit", "-qam", "two"]);
  await gitSh(dir, ["tag", "2.0.0"]);
  const commit2 = (await gitSh(dir, ["rev-parse", "HEAD"])).trim();
  return { dir, commit1, commit2 };
}

/** Clones the fake upstream and checks out `tag`: a stand-in for `src`. */
export async function makeSrc(
  up: Upstream,
  tag: string,
  dest = join(tmp("src"), "src"),
): Promise<string> {
  await gitSh(tmp("x"), ["clone", "-q", up.dir, dest]);
  await gitSh(dest, ["checkout", "-q", "--detach", `refs/tags/${tag}`]);
  return dest;
}

/** Adds a commit in `src` that adds //bdk to the framework's deps and exports a marker. */
export async function commitBdkHook(
  src: string,
  message = "bdk: link //bdk\n\nThe overlay must be linked into the framework.\n\nUsed by: the BDK engine.\n",
): Promise<void> {
  const p = join(src, "chrome", "BUILD.gn");
  const text = readFileSync(p, "utf8");
  writeFileSync(
    p,
    text.replace('public_deps = [ ":chrome_dll" ]', 'public_deps = [ ":chrome_dll", "//bdk" ]'),
  );
  writeFileSync(
    join(src, "chrome", "app", "framework.exports"),
    `${EXPORTS_V1}_bdk_overlay_marker\n`,
  );
  await gitSh(src, ["add", "-A"]);
  await gitSh(src, ["commit", "-q", "-m", message], {
    GIT_AUTHOR_DATE: "2026-02-02T00:00:00+0000",
  });
}

export const GIT_IDENTITY_FOR_TESTS = bdkIdentityArgs;

export interface Sandbox {
  readonly root: string;
  readonly home: string;
  readonly paths: BdkPaths;
  readonly repo: string;
  readonly env: NodeJS.ProcessEnv;
  readonly log: string;
}

const GCLIENT = `#!/bin/sh
echo "gclient $* (cwd=$(pwd))" >> "$FAKE_LOG"
if [ "$1" = sync ]; then
  [ -d src/.git ] || git clone -q "$FAKE_UPSTREAM" src
  tag=$(echo "$*" | sed -n 's|.*refs/tags/\\([^ ]*\\).*|\\1|p')
  git -C src fetch -q --tags origin
  git -C src checkout -q -f --detach "refs/tags/$tag"
fi
`;
const GN = `#!/bin/sh
echo "gn $* (cwd=$(pwd)) args=$(tr '\\n' '|' < "$(echo "$2")/args.gn" | cut -c1-60)" >> "$FAKE_LOG"
mkdir -p "$2"
touch "$2/build.ninja"
`;
const AUTONINJA = `#!/bin/sh
out="$2"
echo "autoninja $* start=$(date +%s) cwd=$(pwd) CCACHE_DIR=$CCACHE_DIR CCACHE_MAXSIZE=$CCACHE_MAXSIZE CCACHE_COMPILERCHECK=$CCACHE_COMPILERCHECK CCACHE_NOHASHDIR=$CCACHE_NOHASHDIR" >> "$FAKE_LOG"
sleep "\${FAKE_BUILD_SECONDS:-0}"
fw="$out/Chromium Framework.framework/Versions/A"
mkdir -p "$fw"
if [ -f bdk/bdk_overlay.cc ] && [ -z "$FAKE_NO_LINK" ]; then echo "has marker" > "$fw/Chromium Framework"; else echo "no" > "$fw/Chromium Framework"; fi
echo "$(pwd)" > "$out/built-by"
echo "autoninja end=$(date +%s) cwd=$(pwd)" >> "$FAKE_LOG"
`;
const NM = `#!/bin/sh
if grep -q "has marker" "$2"; then echo "0000000000001000 T _bdk_overlay_marker"; fi
echo "0000000000002000 T _ChromeMain"
`;
const CCACHE = `#!/bin/sh
echo "ccache 4.fake"
`;

/** BDK_HOME, a fake BDK repo tree (pin, gn presets, gclient template, overlay, empty series), fake depot_tools. */
export function makeSandbox(up: Upstream, pinTag = "1.0.0", pinCommit = up.commit1): Sandbox {
  const root = tmp("sandbox");
  const home = join(root, "home");
  const repo = join(root, "repo");
  const paths = bdkPaths(home);
  for (const d of [
    "repo/chromium/patches",
    "repo/chromium/gn",
    "repo/chromium/gclient",
    "repo/chromium/bdk",
    "home/depot_tools",
  ]) {
    mkdirSync(join(root, d), { recursive: true });
  }
  writeFileSync(
    join(repo, "chromium", "pin.json"),
    JSON.stringify({ tag: pinTag, commit: pinCommit, bdk_abi_version: 1, runtime: null }),
  );
  mkdirSync(join(repo, "packages", "core", "include", "bdk"), { recursive: true });
  mkdirSync(join(repo, "packages", "core", "generated"), { recursive: true });
  writeFileSync(
    join(repo, "packages", "core", "include", "bdk", "bdk_base.h"),
    "#define BDK_ABI_VERSION 1\n",
  );
  writeFileSync(join(repo, "packages", "core", "generated", "abi-hash.txt"), `${"a".repeat(64)}\n`);
  writeFileSync(
    join(repo, "chromium", "runtime-files.json"),
    JSON.stringify({ schema: 1, target: { os: "macos", min_os: "13.0" }, paths: ["Chromium.app"] }),
  );
  writeFileSync(join(repo, "chromium", "patches", "series"), "");
  writeFileSync(
    join(repo, "chromium", "gn", "release.gn"),
    'target_os = "mac"\nis_debug = false\n',
  );
  writeFileSync(join(repo, "chromium", "gn", "dev.gn"), 'target_os = "mac"\nis_debug = true\n');
  writeFileSync(
    join(repo, "chromium", "gclient", "gclient.template"),
    'solutions = [{"url": "{{chromium_url}}"}]\n',
  );
  writeFileSync(join(repo, "chromium", "bdk", "BUILD.gn"), 'source_set("bdk") {}\n');
  writeFileSync(
    join(repo, "chromium", "bdk", "bdk_overlay.cc"),
    "int bdk_overlay_marker() { return 1; }\n",
  );
  const depot = join(home, "depot_tools");
  for (const [name, text] of Object.entries({
    gclient: GCLIENT,
    gn: GN,
    autoninja: AUTONINJA,
    nm: NM,
    ccache: CCACHE,
  })) {
    writeFileSync(join(depot, name), text);
    chmodSync(join(depot, name), 0o755);
  }
  const log = join(root, "calls.log");
  writeFileSync(log, "");
  return {
    root,
    home,
    paths,
    repo,
    log,
    env: {
      ...process.env,
      BDK_HOME: home,
      BDK_REPO_ROOT: repo,
      BDK_CHROMIUM_URL: "https://example.invalid/src.git",
      FAKE_UPSTREAM: up.dir,
      FAKE_LOG: log,
      // The Metal Toolchain preflight runs `$BDK_XCRUN -f metal`; `true` always succeeds.
      BDK_XCRUN: "true",
    },
  };
}
