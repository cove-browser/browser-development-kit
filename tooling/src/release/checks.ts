import { mkdtemp } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { run } from "../git.js";
import { bdkPaths } from "../paths.js";
import { readAbi } from "../runtime/abi.js";
import { type Downloader, download } from "../runtime/backends.js";
import { sha256File } from "../runtime/canonical.js";
import { computeSourceIdentity, sourceKey } from "../runtime/identity.js";
import { type Pin, readPin } from "../runtime/pin.js";
import { gitStatusPorcelain } from "../runtime/publisher.js";
import { fetchRuntime } from "../runtime/resolver.js";
import { readRuntimeFiles } from "../runtime/runtime-files.js";
import { removeTree } from "../runtime/store.js";

/**
 * The release dry run (T-PKG-06, docs/release/procedure.md): every check a release needs that can run with no
 * Chromium build, no signing identity and no upload. It never writes to the repository, `.bdk/` or `BDK_HOME`:
 * the published runtime is fetched into a scratch store that is removed afterwards.
 */
export type CheckStatus = "pass" | "fail" | "skip";

export interface CheckResult {
  readonly name: string;
  readonly status: CheckStatus;
  readonly detail: string;
}

export type Runner = (
  cmd: string,
  args: readonly string[],
  cwd: string,
) => Promise<{ code: number | null; stdout: string; stderr: string }>;

export interface ReleaseCheckOptions {
  readonly repoRoot: string;
  /** `X.Y.Z` (or `X.Y.Z-pre`) to be tagged `vX.Y.Z`; the tag check is skipped without it. */
  readonly version?: string;
  /** A local copy of the Swift binary target's archive, to compare with the checksum in `Package.swift`. */
  readonly swiftArchive?: string;
  /** Skip `swift package resolve` (it needs a Swift toolchain and, for a binary target, the network). */
  readonly skipSwiftResolve?: boolean;
  /** Injectable for tests. */
  readonly downloader?: Downloader;
  readonly gitStatus?: (repoRoot: string) => Promise<string>;
  readonly runner?: Runner;
}

const SEMVER = /^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(-[0-9A-Za-z.-]+)?$/;

const defaultRunner: Runner = (cmd, args, cwd) => run(cmd, args, { cwd });

const pass = (name: string, detail: string): CheckResult => ({ name, status: "pass", detail });
const fail = (name: string, detail: string): CheckResult => ({ name, status: "fail", detail });
const skip = (name: string, detail: string): CheckResult => ({ name, status: "skip", detail });

async function cleanTree(opts: ReleaseCheckOptions): Promise<CheckResult> {
  const status = await (opts.gitStatus ?? gitStatusPorcelain)(opts.repoRoot);
  return status.trim() === ""
    ? pass("clean-tree", "git status --porcelain is empty")
    : fail(
        "clean-tree",
        `the tree has changes; a release is cut from a clean tree:\n${status.trimEnd()}`,
      );
}

async function versionTag(opts: ReleaseCheckOptions, runner: Runner): Promise<CheckResult> {
  if (opts.version === undefined) return skip("version-tag", "no --version given");
  if (!SEMVER.test(opts.version)) {
    return fail("version-tag", `"${opts.version}" is not SemVer (X.Y.Z); the tag would be vX.Y.Z`);
  }
  const tag = `v${opts.version}`;
  const r = await runner("git", ["tag", "--list", tag], opts.repoRoot);
  if (r.code !== 0) return fail("version-tag", `git tag --list failed: ${r.stderr.trim()}`);
  return r.stdout.trim() === ""
    ? pass("version-tag", `${tag} does not exist yet`)
    : fail("version-tag", `${tag} already exists; a published tag is never moved`);
}

async function pinChecks(repoRoot: string, pin: Pin): Promise<CheckResult[]> {
  const results: CheckResult[] = [];
  const abi = readAbi(repoRoot);
  const record = pin.runtime;
  if (record === null) {
    results.push(
      fail("pin-record", "chromium/pin.json records no published runtime (pnpm chromium:publish)"),
    );
    return results;
  }
  results.push(pass("pin-record", `runtime ${record.source_key} at ${record.url}`));
  results.push(
    pin.bdk_abi_version === abi.version && record.abi_hash === abi.hash
      ? pass("abi-hash", `ABI ${abi.version}, ${abi.hash} in the headers and the pin record`)
      : fail(
          "abi-hash",
          `the headers give ABI ${abi.version} ${abi.hash}; chromium/pin.json records ${pin.bdk_abi_version} ${record.abi_hash}`,
        ),
  );
  const key = sourceKey(await computeSourceIdentity({ repoRoot }));
  results.push(
    key === record.source_key
      ? pass("source-key", `the tree's inputs give ${key}, the published key`)
      : fail(
          "source-key",
          `the tree's inputs give ${key} but the published runtime is ${record.source_key}: rebuild, package and publish`,
        ),
  );
  return results;
}

/** Fetches the published runtime into a scratch store: archive digest, every file, manifest digest, ABI hash. */
async function runtimeDigest(opts: ReleaseCheckOptions, pin: Pin): Promise<CheckResult> {
  const record = pin.runtime;
  if (record === null) return skip("runtime-digest", "nothing is published");
  const scratch = await mkdtemp(join(tmpdir(), "bdk-release-check-"));
  try {
    const paths = bdkPaths(scratch);
    const verified = await fetchRuntime(
      record.source_key,
      record,
      paths,
      opts.downloader ?? download,
    );
    const notices = readRuntimeFiles(opts.repoRoot).notices;
    if (notices !== undefined && verified.manifest.files[notices.to] === undefined) {
      return fail("runtime-digest", `the runtime has no third-party notices (${notices.to})`);
    }
    return pass(
      "runtime-digest",
      `archive ${record.archive_sha256} and manifest ${verified.manifestSha256} match chromium/pin.json`,
    );
  } catch (error) {
    return fail("runtime-digest", (error as Error).message);
  } finally {
    await removeTree(scratch);
  }
}

interface DumpedTarget {
  readonly name?: string;
  readonly type?: string;
  readonly url?: string | null;
  readonly checksum?: string | null;
  readonly path?: string | null;
}

/** `Package.swift` parses, its binary target is an https archive with a checksum, and the package resolves. */
async function swiftPackage(opts: ReleaseCheckOptions, runner: Runner): Promise<CheckResult[]> {
  const results: CheckResult[] = [];
  const dump = await runner("swift", ["package", "dump-package"], opts.repoRoot);
  if (dump.code !== 0) {
    return [fail("swift-manifest", `swift package dump-package failed: ${dump.stderr.trim()}`)];
  }
  let targets: DumpedTarget[];
  try {
    targets = ((JSON.parse(dump.stdout) as { targets?: DumpedTarget[] }).targets ?? []).filter(
      (t) => t.type === "binary",
    );
  } catch {
    return [fail("swift-manifest", "swift package dump-package printed no JSON")];
  }
  const remote = targets.filter((t) => typeof t.url === "string");
  if (remote.length === 0) {
    results.push(
      fail(
        "swift-manifest",
        "Package.swift declares no remote binary target for the runtime (T-PKG-04): an application could not resolve it",
      ),
    );
  } else {
    const problems: string[] = [];
    for (const t of remote) {
      if (!(t.url as string).startsWith("https://"))
        problems.push(`${t.name}: SwiftPM accepts only https binary targets`);
      if (!/^[0-9a-f]{64}$/.test(t.checksum ?? ""))
        problems.push(`${t.name}: checksum is not 64 hex digits`);
      if (!(t.url as string).endsWith(".zip"))
        problems.push(`${t.name}: SwiftPM binary targets are .zip archives`);
    }
    results.push(
      problems.length === 0
        ? pass("swift-manifest", remote.map((t) => `${t.name} ${t.checksum}`).join(", "))
        : fail("swift-manifest", problems.join("; ")),
    );
    if (opts.swiftArchive !== undefined) {
      const sha = await sha256File(opts.swiftArchive);
      const match = remote.find((t) => t.checksum === sha);
      results.push(
        match !== undefined
          ? pass("swift-checksum", `${opts.swiftArchive} is ${match.name}`)
          : fail(
              "swift-checksum",
              `${opts.swiftArchive} has SHA-256 ${sha}, which no binary target in Package.swift names`,
            ),
      );
    }
  }
  if (opts.skipSwiftResolve) {
    results.push(skip("swift-resolve", "--skip-swift-resolve"));
  } else {
    const r = await runner("swift", ["package", "resolve"], opts.repoRoot);
    results.push(
      r.code === 0
        ? pass("swift-resolve", "swift package resolve succeeded")
        : fail("swift-resolve", `swift package resolve failed: ${r.stderr.trim()}`),
    );
  }
  return results;
}

export async function releaseCheck(opts: ReleaseCheckOptions): Promise<CheckResult[]> {
  const runner = opts.runner ?? defaultRunner;
  const pin = readPin(opts.repoRoot);
  return [
    await cleanTree(opts),
    await versionTag(opts, runner),
    ...(await pinChecks(opts.repoRoot, pin)),
    await runtimeDigest(opts, pin),
    ...(await swiftPackage(opts, runner)),
  ];
}
