import { execFileSync } from "node:child_process";
import { chmod, mkdir, writeFile } from "node:fs/promises";
import { join } from "node:path";
import { describe, expect, it } from "vitest";
import { createAssets } from "../../assets.js";
import type { HostToolchain, Platform, SignatureReport } from "../../platform/platform.js";
import { tempDir, tempProject, testContext } from "../../test-support.js";
import { resolveConfig } from "../config/index.js";
import type { Diagnostic } from "../diagnostics.js";
import { ExitCode } from "../exit-codes.js";
import { run } from "../run.js";

/** A healthy Apple Silicon Mac with a current Xcode. */
const GOOD: HostToolchain = {
  os: "macos",
  osVersion: "27.2",
  arch: "arm64",
  ide: "27.2",
  swift: "6.4.2",
};

const SIGNED: SignatureReport = { valid: true, helpers: [], stapled: false };

/** The test context's platform, with the machine and the signature faked. */
function platform(toolchain: HostToolchain, signature: SignatureReport = SIGNED): Platform {
  return {
    ...testContext().platform,
    toolchain: () => Promise.resolve(toolchain),
    inspectSignature: () => Promise.resolve(signature),
  };
}

/** A directory of fake executables, for PATH. */
async function bin(...names: string[]): Promise<string> {
  const dir = await tempDir("bdk-doctor-bin-");
  for (const name of names) {
    await writeFile(join(dir, name), "#!/bin/sh\n");
    await chmod(join(dir, name), 0o755);
  }
  return dir;
}

/** Source mode's depot_tools in the test's BDK_HOME, so only the finding under test is an error. */
async function withDepotTools(dir: string): Promise<undefined> {
  await mkdir(join(dir, ".home", "depot_tools"), { recursive: true });
  await writeFile(join(dir, ".home", "depot_tools", "gclient"), "");
  return undefined;
}

/** The store entry the fixtures resolve, and the project's pointer to it (`.bdk/runtime.json`). */
const STORE_ENTRY = "fixture-runtime";
const RUNTIME_POINTER = JSON.stringify({ source_key: STORE_ENTRY });

/**
 * A runtime in the test's store that verifies against its manifest (it has no files besides the manifest),
 * resolved by the project. By default it is this BDK's pin and ABI.
 */
async function storeRuntime(
  dir: string,
  over: { abi?: object; chromium?: { tag: string; commit: string }; minOs?: string },
): Promise<void> {
  const release = JSON.parse(createAssets({ packaged: false }).readText("bdk-release.json"));
  const key = STORE_ENTRY;
  const store = join(dir, ".home", "runtimes", key);
  await mkdir(store, { recursive: true });
  await writeFile(
    join(store, "manifest.json"),
    JSON.stringify({
      schema: 1,
      source_key: key,
      bdk_abi: over.abi ?? release.abi,
      chromium: { ...(over.chromium ?? release.chromium), deps_digest: "d" },
      target: { os: "macos", arch: "arm64", min_os: over.minOs ?? "14.0" },
      files: {},
    }),
  );
  await mkdir(join(dir, ".bdk"), { recursive: true });
  await writeFile(join(dir, ".bdk", "runtime.json"), JSON.stringify({ source_key: key }));
}

interface Doctor {
  code: number;
  diagnostics: Diagnostic[];
  result: { groups: Record<string, { codes: string[] }> };
}

/** Runs `bdk doctor --json` with the real schema and resolver. */
async function doctor(
  dir: string,
  options: {
    toolchain?: HostToolchain;
    env?: NodeJS.ProcessEnv;
    signature?: SignatureReport;
    args?: string[];
  } = {},
): Promise<Doctor> {
  const ctx = testContext({
    cwd: dir,
    env: { PATH: await bin(), BDK_HOME: join(dir, ".home"), ...options.env },
    assets: createAssets({ packaged: false }),
    platform: platform(options.toolchain ?? GOOD, options.signature),
    resolveConfig,
  });
  const code = await run(["doctor", "--json", ...(options.args ?? [])], ctx);
  const document = JSON.parse(ctx.out.join(""));
  return { code, diagnostics: document.diagnostics, result: document.result };
}

function codes(d: Doctor): string[] {
  return d.diagnostics.map((x) => x.code);
}

describe("bdk doctor: a healthy project", () => {
  it("exits 0, reports the four groups (three without --bundle) and only notes and warnings", async () => {
    const { dir } = await tempProject();
    const d = await doctor(dir);
    expect(d.code).toBe(ExitCode.Success);
    expect(Object.keys(d.result.groups)).toEqual(["machine", "project", "compatibility"]);
    expect(d.diagnostics.every((x) => x.severity !== "error")).toBe(true);
  });

  it("prints a summary per group without --json", async () => {
    const { dir } = await tempProject();
    const ctx = testContext({
      cwd: dir,
      env: { PATH: await bin(), BDK_HOME: join(dir, ".home") },
      assets: createAssets({ packaged: false }),
      platform: platform(GOOD),
      resolveConfig,
    });
    expect(await run(["doctor"], ctx)).toBe(0);
    expect(ctx.out.join("")).toMatch(/machine\s+ok/);
    expect(ctx.out.join("")).toMatch(/compatibility\s+ok/);
  });

  it("does not report disk space", async () => {
    const { dir } = await tempProject();
    const d = await doctor(dir);
    expect(JSON.stringify(d).toLowerCase()).not.toMatch(/disk|free space/);
  });
});

/** Each failure mode: its fixture, the code it gives, the group it is in and words of its message. */
describe("bdk doctor: each failure mode has a fixture and a message", () => {
  const cases: {
    name: string;
    code: string;
    group: string;
    message: RegExp;
    severity: Diagnostic["severity"];
    toolchain?: HostToolchain;
    toml?: string;
    setup?: (dir: string) => Promise<NodeJS.ProcessEnv | undefined>;
  }[] = [
    {
      name: "a host that is not macOS",
      code: "doctor.host_unsupported",
      group: "machine",
      severity: "error",
      message: /runs linux/,
      toolchain: { os: "linux", arch: "x64" },
    },
    {
      name: "an Intel Mac",
      code: "doctor.not_apple_silicon",
      group: "machine",
      severity: "error",
      message: /Apple Silicon/,
      toolchain: { ...GOOD, arch: "x64" },
    },
    {
      name: "macOS older than the minimum",
      code: "doctor.macos_too_old",
      group: "machine",
      severity: "error",
      message: /macOS 13\.6 is older than 14\.0/,
      toolchain: { ...GOOD, osVersion: "13.6" },
    },
    {
      name: "no Xcode",
      code: "doctor.xcode_missing",
      group: "machine",
      severity: "error",
      message: /Xcode is not installed/,
      toolchain: { os: "macos", osVersion: "27.2", arch: "arm64", swift: "6.4.2" },
    },
    {
      name: "an old Xcode",
      code: "doctor.xcode_too_old",
      group: "machine",
      severity: "error",
      message: /Xcode 15\.4 is older than 26\.0/,
      toolchain: { ...GOOD, ide: "15.4" },
    },
    {
      name: "no Swift",
      code: "doctor.swift_missing",
      group: "machine",
      severity: "error",
      message: /Swift compiler was not found/,
      toolchain: { os: "macos", osVersion: "27.2", arch: "arm64", ide: "27.2" },
    },
    {
      name: "an old Swift",
      code: "doctor.swift_too_old",
      group: "machine",
      severity: "error",
      message: /Swift 5\.10 is older than 6\.0/,
      toolchain: { ...GOOD, swift: "5.10" },
    },
    {
      name: "Vite pages and no Node",
      code: "doctor.node_missing",
      group: "machine",
      severity: "error",
      message: /Node is not on PATH/,
      setup: async (dir) => {
        await mkdir(join(dir, "Pages"));
        await writeFile(join(dir, "Pages", "vite.config.ts"), "export default {};\n");
        return { PATH: await bin("pnpm") };
      },
    },
    {
      name: "automation scripts and no package manager",
      code: "doctor.package_manager_missing",
      group: "machine",
      severity: "error",
      message: /none of pnpm, npm, yarn, bun/,
      setup: async (dir) => {
        await mkdir(join(dir, "Automation"));
        await writeFile(join(dir, "Automation", "a.ts"), "export default () => 1;\n");
        return { PATH: await bin("node") };
      },
    },
    {
      name: "source mode with BDK_HOME containing a space",
      code: "doctor.bdk_home_invalid",
      group: "machine",
      severity: "error",
      message: /must not contain spaces/,
      toml: '[browser]\nname = "Demo"\n\n[runtime]\nmode = "source"\n',
      setup: async (dir) => ({ BDK_HOME: join(dir, "with space") }),
    },
    {
      name: "source mode without depot_tools",
      code: "doctor.depot_tools_missing",
      group: "machine",
      severity: "error",
      message: /depot_tools is not at/,
      toml: '[browser]\nname = "Demo"\n\n[runtime]\nmode = "source"\n',
    },
    {
      name: "source mode without a managed checkout",
      code: "doctor.checkout_missing",
      group: "machine",
      severity: "warning",
      message: /no managed Chromium checkout/,
      toml: '[browser]\nname = "Demo"\n\n[runtime]\nmode = "source"\n',
      setup: withDepotTools,
    },
    {
      name: "source mode with a managed checkout that is not at the pin",
      code: "doctor.checkout_not_at_pin",
      group: "machine",
      severity: "warning",
      message: /not at Chromium/,
      toml: '[browser]\nname = "Demo"\n\n[runtime]\nmode = "source"\n',
      setup: async (dir) => {
        await withDepotTools(dir);
        const src = join(dir, ".home", "chromium", "managed", "src");
        await mkdir(src, { recursive: true });
        const git = (...args: string[]) =>
          execFileSync("git", ["-c", "user.name=t", "-c", "user.email=t@t", ...args], { cwd: src });
        git("init", "-q");
        git("commit", "-q", "--allow-empty", "-m", "elsewhere");
        return undefined;
      },
    },
    {
      name: "a runtime built for another ABI",
      code: "doctor.abi_mismatch",
      group: "compatibility",
      severity: "error",
      message: /would fail at start/,
      setup: async (dir) => {
        await storeRuntime(dir, { abi: { version: 99, hash: "f".repeat(64) } });
        return undefined;
      },
    },
    {
      name: "a runtime of another Chromium",
      code: "doctor.runtime_other_chromium",
      group: "compatibility",
      severity: "warning",
      message: /is Chromium 1\.0\.0/,
      setup: async (dir) => {
        await storeRuntime(dir, { chromium: { tag: "1.0.0", commit: "0".repeat(40) } });
        return undefined;
      },
    },
    {
      name: "macOS older than the resolved runtime needs",
      code: "doctor.macos_too_old",
      group: "machine",
      severity: "error",
      message: /older than 28\.0/,
      setup: async (dir) => {
        await storeRuntime(dir, { minOs: "28.0" });
        return undefined;
      },
    },
    {
      name: "source mode without ccache",
      code: "doctor.ccache_missing",
      group: "machine",
      severity: "warning",
      message: /ccache is not on PATH/,
      toml: '[browser]\nname = "Demo"\n\n[runtime]\nmode = "source"\n',
      setup: withDepotTools,
    },
    {
      name: "an invalid bdk.toml (the resolver's own diagnostic)",
      code: "config.unknown_key",
      group: "project",
      severity: "error",
      message: /bogus/,
      toml: '[browser]\nname = "Demo"\nbogus = 1\n',
    },
    {
      name: "a BDK version in Package.resolved that is not this bdk's",
      code: "doctor.bdk_version_mismatch",
      group: "project",
      severity: "warning",
      message: /pins BDK 9\.9\.9, but this bdk is 1\.2\.3/,
      setup: async (dir) => {
        await writeFile(
          join(dir, "Package.resolved"),
          JSON.stringify({
            pins: [
              {
                identity: "browser-development-kit",
                location: "https://github.com/cove-browser/browser-development-kit.git",
                state: { version: "9.9.9" },
              },
            ],
          }),
        );
        return undefined;
      },
    },
    {
      name: "no published runtime for this version",
      code: "doctor.runtime_unpublished",
      group: "project",
      severity: "warning",
      message: /has no published runtime/,
    },
    {
      name: "a resolved runtime missing from the store",
      code: "doctor.runtime_missing",
      group: "project",
      severity: "error",
      message: /not in the store/,
      setup: async (dir) => {
        await mkdir(join(dir, ".bdk"));
        await writeFile(join(dir, ".bdk", "runtime.json"), RUNTIME_POINTER);
        return undefined;
      },
    },
    {
      name: "a resolved runtime whose files do not match its manifest",
      code: "doctor.runtime_modified",
      group: "project",
      severity: "error",
      message: /does not match its manifest/,
      setup: async (dir) => {
        await mkdir(join(dir, ".bdk"));
        await writeFile(join(dir, ".bdk", "runtime.json"), RUNTIME_POINTER);
        const store = join(dir, ".home", "runtimes", STORE_ENTRY);
        await mkdir(store, { recursive: true });
        await writeFile(join(store, "extra"), "x");
        return undefined;
      },
    },
    {
      name: "an application patch series that lists a missing file",
      code: "doctor.patch_invalid",
      group: "compatibility",
      severity: "error",
      message: /application's patches cannot be read/,
      toml: '[browser]\nname = "Demo"\n\n[runtime]\nmode = "source"\n',
      setup: async (dir) => {
        await mkdir(join(dir, "Chromium", "patches"), { recursive: true });
        await writeFile(join(dir, "Chromium", "patches", "series"), "gone.patch\n");
        return undefined;
      },
    },
  ];

  it.each(cases)("$name: $code", async (c) => {
    const { dir } = await tempProject();
    if (c.toml !== undefined) await writeFile(join(dir, "bdk.toml"), c.toml);
    const env = (await c.setup?.(dir)) ?? {};
    const d = await doctor(dir, {
      ...(c.toolchain === undefined ? {} : { toolchain: c.toolchain }),
      env,
    });
    const finding = d.diagnostics.find((x) => x.code === c.code);
    expect(finding, `${c.code} in ${codes(d).join(", ")}`).toBeDefined();
    expect(finding?.severity).toBe(c.severity);
    expect(finding?.message).toMatch(c.message);
    if (finding?.severity !== "note") expect(finding?.fix).toBeTruthy();
    expect(d.result.groups[c.group]?.codes).toContain(c.code);
    expect(d.code).toBe(c.severity === "error" ? ExitCode.ProjectFailure : ExitCode.Success);
  });
});

describe("bdk doctor: the configuration's own diagnostics", () => {
  it("lists every diagnostic the resolver reports, whatever its code, in the project group", async () => {
    const { dir } = await tempProject();
    const reported: Diagnostic[] = [
      {
        code: "web_apps.push_unconfigured",
        severity: "note",
        message: "Web apps are on with no push provider.",
      },
      {
        code: "services.push_google",
        severity: "warning",
        message: "Push goes through Google.",
        fix: "Set up the project in Google's console.",
      },
    ];
    const ctx = testContext({
      cwd: dir,
      env: { PATH: await bin(), BDK_HOME: join(dir, ".home") },
      assets: createAssets({ packaged: false }),
      platform: platform(GOOD),
      resolveConfig: () => Promise.resolve({ diagnostics: reported, resolved: { routes: {} } }),
    });
    expect(await run(["doctor", "--json"], ctx)).toBe(0);
    const document = JSON.parse(ctx.out.join(""));
    expect(document.result.groups.project.codes).toEqual(
      expect.arrayContaining(["web_apps.push_unconfigured", "services.push_google"]),
    );
  });

  it("checks the template's Info.plist and entitlements outside Xcode", async () => {
    const { dir } = await tempProject();
    await mkdir(join(dir, "Resources"));
    await writeFile(join(dir, "Resources", "Info.plist"), "x");
    await writeFile(join(dir, "Resources", "App.entitlements"), "x");
    let seen: NodeJS.ProcessEnv | undefined;
    const ctx = testContext({
      cwd: dir,
      env: { PATH: await bin(), BDK_HOME: join(dir, ".home") },
      assets: createAssets({ packaged: false }),
      platform: {
        ...platform(GOOD),
        bundleFacts: (env) => {
          seen = env;
          return Promise.resolve({ infoPlist: {} });
        },
      },
      resolveConfig,
    });
    expect(await run(["doctor", "--json"], ctx)).toBe(ExitCode.ProjectFailure);
    expect(seen).toMatchObject({
      SRCROOT: dir,
      INFOPLIST_FILE: "Resources/Info.plist",
      CODE_SIGN_ENTITLEMENTS: "Resources/App.entitlements",
    });
    expect(JSON.parse(ctx.out.join("")).diagnostics.map((d: Diagnostic) => d.code)).toContain(
      "plist.principal_class",
    );
  });

  it("lists the capabilities granted to each page", async () => {
    const { dir } = await tempProject();
    await writeFile(
      join(dir, "bdk.toml"),
      '[browser]\nname = "Demo"\n\n[pages.capabilities.settings]\nallow = ["settings.read"]\n',
    );
    await mkdir(join(dir, "Pages", "Settings"), { recursive: true });
    await writeFile(join(dir, "Pages", "Settings", "index.html"), "<!doctype html>\n");
    const d = await doctor(dir);
    const grant = d.diagnostics.find((x) => x.code === "doctor.page_capabilities");
    expect(grant?.message).toBe('The page "settings" is granted settings.read.');
  });
});

describe("bdk doctor --bundle", () => {
  async function app(dir: string, resolved?: object): Promise<string> {
    const path = join(dir, "build", "Demo.app");
    await mkdir(join(path, "Contents", "Resources"), { recursive: true });
    if (resolved !== undefined) {
      await writeFile(
        join(path, "Contents", "Resources", "bdk.resolved.json"),
        JSON.stringify(resolved),
      );
    }
    return path;
  }

  it("reports the bundle group, an ad hoc signature as a note, and a missing resolved file", async () => {
    const { dir } = await tempProject();
    await app(dir);
    const d = await doctor(dir, { args: ["--bundle", "build/Demo.app"] });
    expect(Object.keys(d.result.groups)).toContain("bundle");
    expect(d.result.groups.bundle?.codes).toEqual([
      "doctor.bundle_config_missing",
      "doctor.ad_hoc",
    ]);
    expect(d.code).toBe(ExitCode.ProjectFailure);
  });

  it.each([
    [
      "doctor.bundle_config_stale",
      { valid: true, helpers: [], stapled: false },
      /does not match bdk\.toml/,
    ],
    [
      "doctor.signature_invalid",
      { valid: false, detail: "code object is not signed at all", helpers: [], stapled: false },
      /not signed at all/,
    ],
    [
      "doctor.helper_team_mismatch",
      { valid: true, team: "AAAA", helpers: [{ path: "/h.app", team: "BBBB" }], stapled: true },
      /signed by BBBB, not the application's team AAAA/,
    ],
    [
      "doctor.not_notarized",
      { valid: true, team: "AAAA", helpers: [{ path: "/h.app", team: "AAAA" }], stapled: false },
      /No notarization ticket/,
    ],
  ] as const)("%s", async (code, signature, message) => {
    const { dir } = await tempProject();
    await app(dir, { config_digest: "sha256:stale" });
    const d = await doctor(dir, {
      args: ["--bundle", "build/Demo.app"],
      signature: { ...signature, helpers: [...signature.helpers] },
    });
    const finding = d.diagnostics.find((x) => x.code === code);
    expect(finding?.message).toMatch(message);
  });

  it("refuses a path that is not an application (usage error)", async () => {
    const { dir } = await tempProject();
    const d = await doctor(dir, { args: ["--bundle", "nowhere.app"] });
    expect(d.code).toBe(ExitCode.Usage);
    expect(codes(d)).toEqual(["doctor.bundle_missing"]);
  });
});

describe("bdk doctor: usage", () => {
  it("takes no positional arguments and no init or build flags", async () => {
    const { dir } = await tempProject();
    const ctx = testContext({ cwd: dir });
    expect(await run(["doctor", "extra"], ctx)).toBe(ExitCode.Usage);
    expect(await run(["doctor", "--out", "x"], ctx)).toBe(ExitCode.Usage);
    expect(ctx.err.join("")).toContain("--out applies to `bdk build` only");
  });
});
