import { appendFileSync, existsSync, readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { afterEach, describe, expect, it } from "vitest";
import { fileBackend } from "../runtime/backends.js";
import { sha256Hex } from "../runtime/canonical.js";
import { packageRuntime } from "../runtime/packager.js";
import { publishRuntime } from "../runtime/publisher.js";
import { removeTree } from "../runtime/store.js";
import { makeSandbox, packageOptions, stampOut, tempDir } from "../runtime/testing.js";
import { type CheckResult, type Runner, releaseCheck } from "./checks.js";
import { render } from "./release-check.js";

const cleanup: string[] = [];
afterEach(async () => {
  for (const d of cleanup.splice(0)) await removeTree(d);
});

const ZIP = "fake xcframework zip\n";
const ZIP_SHA = sha256Hex(ZIP);

interface FakeSwift {
  readonly targets?: readonly Record<string, unknown>[];
  readonly resolveCode?: number;
  readonly tags?: string;
}

/** Stands in for `swift package dump-package`, `swift package resolve` and `git tag --list`. */
function fakeRunner(f: FakeSwift = {}): Runner & { calls: string[] } {
  const calls: string[] = [];
  const runner = async (cmd: string, args: readonly string[]) => {
    calls.push(`${cmd} ${args.join(" ")}`);
    if (cmd === "git") return { code: 0, stdout: f.tags ?? "", stderr: "" };
    if (args[1] === "dump-package") {
      const targets = f.targets ?? [
        { name: "BDK", type: "regular" },
        {
          name: "BDKRuntime",
          type: "binary",
          url: "https://example.invalid/BDKRuntime.xcframework.zip",
          checksum: ZIP_SHA,
        },
      ];
      return { code: 0, stdout: JSON.stringify({ targets }), stderr: "" };
    }
    return { code: f.resolveCode ?? 0, stdout: "", stderr: f.resolveCode ? "resolve failed" : "" };
  };
  return Object.assign(runner, { calls });
}

async function published(label: string, fixture: Parameters<typeof makeSandbox>[1] = {}) {
  const s = makeSandbox(label, { notices: true, ...fixture });
  cleanup.push(s.root);
  await stampOut(s);
  await packageRuntime(packageOptions(s));
  const dest = tempDir("release-dest");
  cleanup.push(dest);
  const { record } = await publishRuntime({
    repoRoot: s.repo,
    paths: s.paths,
    backend: fileBackend(`file://${dest}`),
    gitStatus: async () => "",
  });
  const zip = join(s.root, "BDKRuntime.xcframework.zip");
  writeFileSync(zip, ZIP);
  return { s, record, zip };
}

const byName = (results: readonly CheckResult[]) =>
  Object.fromEntries(results.map((r) => [r.name, r.status]));

const clean = async () => "";

describe("T-PKG-06 release dry run", () => {
  it("passes for a published runtime whose digests match the pin record and a package that resolves", async () => {
    const { s, zip } = await published("rc-ok");
    const runner = fakeRunner();
    const results = await releaseCheck({
      repoRoot: s.repo,
      version: "0.4.0",
      swiftArchive: zip,
      gitStatus: clean,
      runner,
    });
    expect(byName(results)).toEqual({
      "clean-tree": "pass",
      "version-tag": "pass",
      "pin-record": "pass",
      "abi-hash": "pass",
      "source-key": "pass",
      "runtime-digest": "pass",
      "swift-manifest": "pass",
      "swift-checksum": "pass",
      "swift-resolve": "pass",
    });
    expect(runner.calls).toContain("git tag --list v0.4.0");
    expect(runner.calls).toContain("swift package resolve");
    // It verified into a scratch store: the repository's pointer and the sandbox store are untouched.
    expect(existsSync(join(s.repo, ".bdk"))).toBe(false);
    expect(render(results)).toMatch(/^PASS {2}clean-tree/);
  });

  it("fails when nothing is published, and skips the digest check", async () => {
    const s = makeSandbox("rc-none", { notices: true });
    cleanup.push(s.root);
    const results = await releaseCheck({
      repoRoot: s.repo,
      gitStatus: clean,
      runner: fakeRunner(),
    });
    expect(byName(results)).toMatchObject({
      "pin-record": "fail",
      "runtime-digest": "skip",
      "version-tag": "skip",
    });
  });

  it("fails a tampered archive, a manifest digest that differs from the pin, and a stale source key", async () => {
    const { s, record } = await published("rc-tamper");
    appendFileSync(new URL(record.url).pathname, "x");
    let r = await releaseCheck({ repoRoot: s.repo, gitStatus: clean, runner: fakeRunner() });
    expect(r.find((c) => c.name === "runtime-digest")).toMatchObject({ status: "fail" });
    expect(r.find((c) => c.name === "runtime-digest")?.detail).toMatch(/archive SHA-256/);

    const { s: s2 } = await published("rc-manifest");
    const pinFile = join(s2.repo, "chromium/pin.json");
    const pin = JSON.parse(readFileSync(pinFile, "utf8"));
    writeFileSync(
      pinFile,
      JSON.stringify({ ...pin, runtime: { ...pin.runtime, manifest_sha256: "0".repeat(64) } }),
    );
    r = await releaseCheck({ repoRoot: s2.repo, gitStatus: clean, runner: fakeRunner() });
    expect(r.find((c) => c.name === "runtime-digest")?.detail).toMatch(/manifest digest/);

    writeFileSync(join(s2.repo, "chromium/patches/0001-example.patch"), "--- changed\n");
    r = await releaseCheck({ repoRoot: s2.repo, gitStatus: clean, runner: fakeRunner() });
    expect(r.find((c) => c.name === "source-key")).toMatchObject({ status: "fail" });
  });

  it("fails when the headers' ABI hash is not the published one", async () => {
    const { s } = await published("rc-abi");
    writeFileSync(join(s.repo, "packages/core/generated/abi-hash.txt"), `${"f".repeat(64)}\n`);
    const r = await releaseCheck({ repoRoot: s.repo, gitStatus: clean, runner: fakeRunner() });
    expect(byName(r)).toMatchObject({ "abi-hash": "fail", "source-key": "fail" });
  });

  it("fails a published runtime that carries no notices when the repository declares them", async () => {
    const { s } = await published("rc-nonotices", { notices: false });
    const file = join(s.repo, "chromium/runtime-files.json");
    const rf = JSON.parse(readFileSync(file, "utf8"));
    writeFileSync(
      file,
      JSON.stringify({
        ...rf,
        notices: {
          from: "gen/components/resources/about_credits.html",
          to: "THIRD_PARTY_NOTICES.html",
        },
      }),
    );
    const r = await releaseCheck({ repoRoot: s.repo, gitStatus: clean, runner: fakeRunner() });
    expect(r.find((c) => c.name === "runtime-digest")?.detail).toMatch(/third-party notices/);
  });

  it("checks the Swift package: a binary target, https, .zip, its checksum, and resolution", async () => {
    const { s, zip } = await published("rc-swift");
    const check = (f: FakeSwift, swiftArchive?: string) =>
      releaseCheck({
        repoRoot: s.repo,
        gitStatus: clean,
        runner: fakeRunner(f),
        ...(swiftArchive === undefined ? {} : { swiftArchive }),
      });
    let r = await check({ targets: [{ name: "BDK", type: "regular" }] });
    expect(r.find((c) => c.name === "swift-manifest")?.detail).toMatch(/T-PKG-04/);
    r = await check({
      targets: [{ name: "R", type: "binary", url: "http://x.invalid/r.tar.zst", checksum: "zz" }],
    });
    const detail = r.find((c) => c.name === "swift-manifest")?.detail ?? "";
    expect(detail).toMatch(/only https/);
    expect(detail).toMatch(/64 hex/);
    expect(detail).toMatch(/\.zip/);
    writeFileSync(zip, "another archive\n");
    r = await check({}, zip);
    expect(byName(r)["swift-checksum"]).toBe("fail");
    r = await check({ resolveCode: 1 });
    expect(byName(r)["swift-resolve"]).toBe("fail");
    r = await releaseCheck({
      repoRoot: s.repo,
      gitStatus: clean,
      runner: fakeRunner(),
      skipSwiftResolve: true,
    });
    expect(byName(r)["swift-resolve"]).toBe("skip");
  });

  it("fails a dirty tree, an existing tag and a version that is not SemVer", async () => {
    const { s } = await published("rc-tag");
    let r = await releaseCheck({
      repoRoot: s.repo,
      version: "0.4.0",
      gitStatus: async () => " M Package.swift\n",
      runner: fakeRunner({ tags: "v0.4.0\n" }),
    });
    expect(byName(r)).toMatchObject({ "clean-tree": "fail", "version-tag": "fail" });
    r = await releaseCheck({
      repoRoot: s.repo,
      version: "v0.4",
      gitStatus: clean,
      runner: fakeRunner(),
    });
    expect(r.find((c) => c.name === "version-tag")?.detail).toMatch(/SemVer/);
  });
});
