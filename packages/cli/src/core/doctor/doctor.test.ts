import { execFileSync } from "node:child_process";
import { chmod, cp, mkdir, readFile, writeFile } from "node:fs/promises";
import { dirname, join } from "node:path";
import { scanFiles } from "@bdk/tooling/dist/runtime/manifest.js";
import { SUPPORTED_DEPENDENCIES } from "@bdk/vite/tooling";
import { describe, expect, it } from "vitest";
import { type Assets, createAssets } from "../../assets.js";
import type { HostToolchain, Platform, SignatureReport } from "../../platform/platform.js";
import {
  FIXTURE_RUNTIME_POINTER as RUNTIME_POINTER,
  STORE_ENTRY,
  storeRuntime,
  tempDir,
  tempProject,
  testContext,
  withPublishedRuntime,
} from "../../test-support.js";
import { directoryDigest, sha256 } from "../config/digest.js";
import { resolveConfig } from "../config/index.js";
import { configDigest } from "../config/resolve.js";
import type { Diagnostic } from "../diagnostics.js";
import { ExitCode } from "../exit-codes.js";
import { run } from "../run.js";
import { frameworkDifferences, pageDifferences } from "./contents.js";
import { inRange, pinnedVersion } from "./dependencies.js";
import { RELEASE_ASSET, type ReleaseFacts } from "./release.js";

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
    assets?: Assets;
  } = {},
): Promise<Doctor> {
  const ctx = testContext({
    cwd: dir,
    env: { PATH: await bin(), BDK_HOME: join(dir, ".home"), ...options.env },
    assets: options.assets ?? createAssets({ packaged: false }),
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
    assets?: () => Assets;
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
      name: "a source-built runtime whose ABI hash is not the prebuilt runtime's",
      code: "doctor.source_abi_differs",
      group: "compatibility",
      severity: "warning",
      message: /prebuilt runtime of this BDK version reports eeeeeeeeeeee/,
      toml: '[browser]\nname = "Demo"\n\n[runtime]\nmode = "source"\n',
      setup: async (dir) => {
        await withDepotTools(dir);
        await storeRuntime(dir, {});
        return undefined;
      },
      assets: () =>
        withPublishedRuntime({
          source_key: "prebuilt",
          manifest_sha256: "0".repeat(64),
          abi_hash: "e".repeat(64),
          url: "https://example.invalid/runtime.tar.gz",
          archive_format: "tar.gz",
          archive_sha256: "1".repeat(64),
          size: 1,
        }),
    },
    {
      name: "a source-built runtime and no prebuilt runtime to compare its ABI with",
      code: "doctor.prebuilt_abi_unknown",
      group: "compatibility",
      severity: "note",
      message: /not compared with one/,
      toml: '[browser]\nname = "Demo"\n\n[runtime]\nmode = "source"\n',
      setup: async (dir) => {
        await withDepotTools(dir);
        await storeRuntime(dir, {});
        return undefined;
      },
    },
    {
      name: "pages that pin a Vite the preset does not support",
      code: "doctor.vite_dependency_unsupported",
      group: "compatibility",
      severity: "warning",
      message:
        /pin vite \^9\.1\.0, which BDK's Vite preset does not support: .* 8\.0\.0 up to, not including, 9\.0\.0/,
      setup: async (dir) => {
        await mkdir(join(dir, "Pages"));
        await writeFile(
          join(dir, "Pages", "package.json"),
          JSON.stringify({ devDependencies: { vite: "^9.1.0", typescript: "7.0.2" } }),
        );
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
      ...(c.assets === undefined ? {} : { assets: c.assets() }),
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

  it.each([
    [
      "doctor.not_developer_id",
      { authority: "Apple Development: A Person (AAAA)" },
      /signed with Apple Development: A Person \(AAAA\), not a Developer ID/,
    ],
    [
      "doctor.gatekeeper_rejected",
      {
        authority: "Developer ID Application: A Company (AAAA)",
        gatekeeper: { accepted: false, source: "Unnotarized Developer ID" },
      },
      /Gatekeeper rejects the application \(Unnotarized Developer ID\)/,
    ],
    [
      "doctor.gatekeeper_accepted",
      {
        authority: "Developer ID Application: A Company (AAAA)",
        gatekeeper: { accepted: true, source: "Notarized Developer ID" },
      },
      /accepts the application: Notarized Developer ID, signed by Developer ID Application: A Company/,
    ],
  ] as const)("%s on a team signature", async (code, over, message) => {
    const { dir } = await tempProject();
    await app(dir, {});
    const d = await doctor(dir, {
      args: ["--bundle", "build/Demo.app"],
      signature: {
        valid: true,
        team: "AAAA",
        helpers: [],
        stapled: true,
        ...over,
        ...("gatekeeper" in over ? { gatekeeper: { ...over.gatekeeper } } : {}),
      },
    });
    const finding = d.diagnostics.find((x) => x.code === code);
    expect(finding?.message).toMatch(message);
    expect(d.result.groups.bundle?.codes).toContain(code);
  });

  it("does not ask Gatekeeper about a signature that is not Developer ID", async () => {
    const { dir } = await tempProject();
    await app(dir, {});
    const d = await doctor(dir, {
      args: ["--bundle", "build/Demo.app"],
      signature: {
        valid: true,
        team: "AAAA",
        authority: "Apple Development: A Person (AAAA)",
        helpers: [],
        stapled: false,
        gatekeeper: { accepted: false, source: "no usable signature" },
      },
    });
    expect(codes(d)).not.toContain("doctor.gatekeeper_rejected");
  });

  it("checks each bundled extension's copy against the digest in the application's resolved file", async () => {
    const { dir } = await tempProject();
    const path = await app(dir);
    const resources = join(path, "Contents", "Resources");
    await mkdir(join(resources, "Extensions", "Reader"), { recursive: true });
    await writeFile(join(resources, "Extensions", "Reader", "manifest.json"), "{}");
    const entry = (digest: string) => ({ path: "Extensions/Reader", required: false, digest });
    const check = async (bundled: object) => {
      await writeFile(
        join(resources, "bdk.resolved.json"),
        JSON.stringify({ extensions: { bundled } }),
      );
      return doctor(dir, { args: ["--bundle", "build/Demo.app"] });
    };

    const good = await check({
      reader: entry(directoryDigest(join(resources, "Extensions/Reader"))),
    });
    expect(codes(good)).not.toContain("doctor.bundled_extension_modified");
    expect(codes(good)).not.toContain("doctor.bundled_extension_missing");

    const modified = await check({ reader: entry("sha256:other") });
    expect(
      modified.diagnostics.find((x) => x.code === "doctor.bundled_extension_modified")?.message,
    ).toBe(
      'The bundled extension "reader" in the application does not match the digest in its bdk.resolved.json.',
    );

    const missing = await check({ notes: { path: "Extensions/Notes", digest: "sha256:x" } });
    expect(
      missing.diagnostics.find((x) => x.code === "doctor.bundled_extension_missing")?.message,
    ).toBe(
      'The application has no copy of the bundled extension "notes" at Contents/Resources/Extensions/Notes.',
    );
    expect(missing.code).toBe(ExitCode.ProjectFailure);
  });

  it("refuses a path that is not an application (usage error)", async () => {
    const { dir } = await tempProject();
    const d = await doctor(dir, { args: ["--bundle", "nowhere.app"] });
    expect(d.code).toBe(ExitCode.Usage);
    expect(codes(d)).toEqual(["doctor.bundle_missing"]);
  });
});

describe("bdk doctor --bundle: pages, scripts and the runtime against what the build recorded", () => {
  const APP = "build/Demo.app";

  /** `bdk build config` in the project: the resolved file the build copies into the bundle. */
  async function resolveIn(dir: string): Promise<Record<string, unknown>> {
    const ctx = testContext({
      cwd: dir,
      env: { PATH: await bin() },
      assets: createAssets({ packaged: false }),
      platform: platform(GOOD),
      resolveConfig,
    });
    expect(await run(["build", "config"], ctx)).toBe(0);
    return JSON.parse(await readFile(join(dir, "bdk.resolved.json"), "utf8"));
  }

  /** An application whose Resources hold `resolved` and, unless `pages` is false, a copy of `Pages/`. */
  async function built(
    dir: string,
    resolved: Record<string, unknown>,
    pages = true,
  ): Promise<string> {
    const resources = join(dir, APP, "Contents", "Resources");
    await mkdir(resources, { recursive: true });
    await writeFile(join(resources, "bdk.resolved.json"), JSON.stringify(resolved));
    if (pages) await cp(join(dir, "Pages"), join(resources, "Pages"), { recursive: true });
    return resources;
  }

  async function withPage(dir: string): Promise<void> {
    await mkdir(join(dir, "Pages", "Settings"), { recursive: true });
    await writeFile(join(dir, "Pages", "Settings", "index.html"), "<!doctype html>\n");
    await writeFile(join(dir, "Pages", "Settings", "main.js"), "console.log(1);\n");
  }

  function bundleCodes(d: Doctor): string[] {
    return d.result.groups.bundle?.codes ?? [];
  }

  it("reports nothing for pages copied as they are and a matching configuration", async () => {
    const { dir } = await tempProject();
    await withPage(dir);
    await built(dir, await resolveIn(dir));
    const d = await doctor(dir, { args: ["--bundle", APP] });
    expect(bundleCodes(d)).toEqual(["doctor.ad_hoc"]);
  });

  it("doctor.page_missing: a bundled page that is not in the application", async () => {
    const { dir } = await tempProject();
    await withPage(dir);
    await built(dir, await resolveIn(dir), false);
    const d = await doctor(dir, { args: ["--bundle", APP] });
    const finding = d.diagnostics.find((x) => x.code === "doctor.page_missing");
    expect(finding?.message).toMatch(/The page "settings" is not in the application/);
    expect(finding?.fix).toBeTruthy();
    expect(d.code).toBe(ExitCode.ProjectFailure);
  });

  it("doctor.page_modified: a copied page whose files differ from its source", async () => {
    const { dir } = await tempProject();
    await withPage(dir);
    const resources = await built(dir, await resolveIn(dir));
    await writeFile(join(resources, "Pages", "Settings", "main.js"), "console.log(2);\n");
    await writeFile(join(resources, "Pages", "Settings", "extra.js"), "\n");
    const d = await doctor(dir, { args: ["--bundle", APP] });
    const finding = d.diagnostics.find((x) => x.code === "doctor.page_modified");
    expect(finding?.message).toBe(
      'The page "settings" in the application does not match its source: modified: main.js; unexpected: extra.js.',
    );
    expect(codes(d)).not.toContain("doctor.bundle_config_stale");
    expect(d.code).toBe(ExitCode.ProjectFailure);
  });

  it("does not compare a page whose source changed after the build (the configuration is stale)", async () => {
    const { dir } = await tempProject();
    await withPage(dir);
    await built(dir, await resolveIn(dir));
    await writeFile(join(dir, "Pages", "Settings", "main.js"), "console.log(3);\n");
    const d = await doctor(dir, { args: ["--bundle", APP] });
    expect(bundleCodes(d)).toContain("doctor.bundle_config_stale");
    expect(bundleCodes(d)).not.toContain("doctor.page_modified");
  });

  it("doctor.page_unverified: a page Vite builds, whose output has no recorded digest", async () => {
    const { dir } = await tempProject();
    await withPage(dir);
    await writeFile(join(dir, "Pages", "vite.config.ts"), "export default {};\n");
    await built(dir, await resolveIn(dir));
    const d = await doctor(dir, {
      args: ["--bundle", APP],
      env: { PATH: await bin("node", "pnpm") },
    });
    const finding = d.diagnostics.find((x) => x.code === "doctor.page_unverified");
    expect(finding?.severity).toBe("note");
    expect(finding?.message).toMatch(/built by Vite/);
  });

  /** A resolved file with one automation script, as `bdk build automation` completes it. */
  function withScript(bundle: Record<string, unknown>): Record<string, unknown> {
    return {
      config_digest: "sha256:x",
      routes: {},
      automation: {
        enabled: true,
        scripts: { hello: { path: "Automation/hello.ts", digest: "sha256:s", ...bundle } },
      },
    };
  }

  it.each([
    ["doctor.script_not_bundled", {}, undefined, /"hello" was not bundled/],
    [
      "doctor.script_missing",
      { bundle: "BDK/Automation/hello.js", bundle_digest: sha256("x") },
      undefined,
      /"hello" is not in the application/,
    ],
    [
      "doctor.script_modified",
      { bundle: "BDK/Automation/hello.js", bundle_digest: sha256("x") },
      "y",
      /"hello" in the application does not match the digest/,
    ],
  ] as const)("%s", async (code, bundle, content, message) => {
    const { dir } = await tempProject();
    const resources = await built(dir, withScript(bundle), false);
    if (content !== undefined) {
      await mkdir(join(resources, "BDK", "Automation"), { recursive: true });
      await writeFile(join(resources, "BDK", "Automation", "hello.js"), content);
    }
    const d = await doctor(dir, { args: ["--bundle", APP] });
    const finding = d.diagnostics.find((x) => x.code === code);
    expect(finding?.message).toMatch(message);
    expect(finding?.severity).toBe("error");
    expect(finding?.fix).toBeTruthy();
  });

  it("accepts a script whose bundle has the recorded digest", async () => {
    const { dir } = await tempProject();
    const resources = await built(
      dir,
      withScript({ bundle: "BDK/Automation/hello.js", bundle_digest: sha256("x") }),
      false,
    );
    await mkdir(join(resources, "BDK", "Automation"), { recursive: true });
    await writeFile(join(resources, "BDK", "Automation", "hello.js"), "x");
    const d = await doctor(dir, { args: ["--bundle", APP] });
    expect(bundleCodes(d).filter((c) => c.startsWith("doctor.script_"))).toEqual([]);
  });

  it("compares the configuration without the scripts' bundles, which `bdk build automation` adds", async () => {
    const { dir } = await tempProject();
    await writeFile(
      join(dir, "bdk.toml"),
      '[browser]\nname = "Demo"\n\n[automation]\nenabled = true\n',
    );
    await mkdir(join(dir, "Automation"));
    await writeFile(join(dir, "Automation", "hello.ts"), "export default () => 1;\n");
    const { config_digest: _digest, ...resolved } = await resolveIn(dir);
    const automation = resolved.automation as { scripts: Record<string, object> };
    // As `bdk build automation` writes it: the scripts' bundles added and the digest recomputed.
    const body = {
      ...resolved,
      automation: {
        ...automation,
        scripts: {
          hello: {
            ...automation.scripts.hello,
            bundle: "BDK/Automation/hello.js",
            bundle_digest: sha256("x"),
          },
        },
      },
    };
    const resources = await built(dir, { ...body, config_digest: configDigest(body) }, false);
    await mkdir(join(resources, "BDK", "Automation"), { recursive: true });
    await writeFile(join(resources, "BDK", "Automation", "hello.js"), "x");
    const d = await doctor(dir, {
      args: ["--bundle", APP],
      env: { PATH: await bin("node", "pnpm") },
    });
    expect(bundleCodes(d)).toEqual(["doctor.ad_hoc"]);
  });

  const RUNTIME_FILES = {
    "Chromium.app/Contents/Info.plist": "<plist/>",
    "Chromium.app/Contents/Frameworks/Lib.framework/Resources/Info.plist": "<plist/>",
    "Chromium.app/Contents/Frameworks/Lib.framework/Resources/resources.pak": "pak",
  };

  it("doctor.runtime_not_embedded: a resolved runtime the application does not embed", async () => {
    const { dir } = await tempProject();
    await storeRuntime(dir, { files: RUNTIME_FILES });
    await built(dir, {}, false);
    const d = await doctor(dir, { args: ["--bundle", APP] });
    const finding = d.diagnostics.find((x) => x.code === "doctor.runtime_not_embedded");
    expect(finding?.message).toMatch(/embeds no runtime \(Lib\.framework is not in/);
  });

  it("accepts an embedded runtime that signing changed only where it signs", async () => {
    const { dir } = await tempProject();
    const store = await storeRuntime(dir, { files: RUNTIME_FILES });
    await built(dir, {}, false);
    const frameworks = join(dir, APP, "Contents", "Frameworks");
    await cp(
      join(store, "Chromium.app/Contents/Frameworks/Lib.framework"),
      join(frameworks, "Lib.framework"),
      { recursive: true },
    );
    await writeFile(join(frameworks, "Lib.framework", "Resources", "Info.plist"), "<plist id/>");
    await mkdir(join(frameworks, "Lib.framework", "_CodeSignature"));
    await writeFile(join(frameworks, "Lib.framework", "_CodeSignature", "CodeResources"), "");
    await mkdir(join(frameworks, "Other.framework"));
    const d = await doctor(dir, { args: ["--bundle", APP] });
    expect(codes(d)).not.toContain("doctor.framework_modified");
    expect(codes(d)).not.toContain("doctor.runtime_not_embedded");
  });

  it("doctor.framework_modified: an embedded runtime whose files are not the manifest's", async () => {
    const { dir } = await tempProject();
    const store = await storeRuntime(dir, { files: RUNTIME_FILES });
    await built(dir, {}, false);
    const frameworks = join(dir, APP, "Contents", "Frameworks");
    await cp(
      join(store, "Chromium.app/Contents/Frameworks/Lib.framework"),
      join(frameworks, "Lib.framework"),
      { recursive: true },
    );
    await writeFile(join(frameworks, "Lib.framework", "Resources", "resources.pak"), "x");
    const d = await doctor(dir, { args: ["--bundle", APP] });
    const finding = d.diagnostics.find((x) => x.code === "doctor.framework_modified");
    expect(finding?.message).toBe(
      `The runtime in the application does not match the manifest of runtime ${STORE_ENTRY}: modified: Lib.framework/Resources/resources.pak.`,
    );
    expect(d.code).toBe(ExitCode.ProjectFailure);
  });
});

describe("the bundle checks' comparisons", () => {
  it("compares a runtime's files by kind and executable bit where signing rewrites them", () => {
    const manifest = {
      "R.app/Contents/MacOS/R": "sha256:a:exec",
      "R.app/Contents/Info.plist": "sha256:b",
      "R.app/Contents/Libraries/l.dylib": "sha256:c",
      "R.app/Contents/Resources/x.pak": "sha256:d",
      "R.app/Current": "symlink:A",
      "R.app/Contents/Resources/gone.pak": "sha256:e",
    };
    expect(
      frameworkDifferences(manifest, {
        "R.app/Contents/MacOS/R": "sha256:signed:exec",
        "R.app/Contents/Info.plist": "sha256:rewritten",
        "R.app/Contents/Libraries/l.dylib": "sha256:signed",
        "R.app/Contents/Resources/x.pak": "sha256:d",
        "R.app/Current": "symlink:B",
        "R.app/Contents/_CodeSignature/CodeResources": "sha256:f",
        "R.app/Contents/Resources/new.pak": "sha256:g",
      }),
    ).toEqual([
      "relinked: R.app/Current",
      "missing: R.app/Contents/Resources/gone.pak",
      "unexpected: R.app/Contents/Resources/new.pak",
    ]);
    expect(frameworkDifferences({ "R.app/x": "sha256:a:exec" }, { "R.app/x": "sha256:a" })).toEqual(
      ["executable bit differs: R.app/x"],
    );
  });

  it("lists a page's missing, modified and unexpected files", () => {
    expect(
      pageDifferences(
        new Map([
          ["a", "1"],
          ["b", "2"],
        ]),
        new Map([
          ["b", "3"],
          ["c", "4"],
        ]),
      ),
    ).toEqual(["missing: a", "modified: b", "unexpected: c"]);
  });

  it("reads the version a specifier pins, and leaves out tags, paths and ranges it cannot", () => {
    expect(pinnedVersion("8.3.8")).toBe("8.3.8");
    expect(pinnedVersion("^8.3.8")).toBe("8.3.8");
    expect(pinnedVersion("~8.3")).toBe("8.3");
    expect(pinnedVersion("latest")).toBeUndefined();
    expect(pinnedVersion("file:../vite")).toBeUndefined();
    expect(pinnedVersion(">=8 <10")).toBeUndefined();
    expect(inRange("8.3.8", { from: "8.0.0", before: "9.0.0" })).toBe(true);
    expect(inRange("9.0.0", { from: "8.0.0", before: "9.0.0" })).toBe(false);
  });

  it("supports every Vite and Vite plugin version bdk init's templates pin", () => {
    const versions = JSON.parse(
      createAssets({ packaged: false }).readText("templates/pages/versions.json"),
    );
    const pins: Record<string, string> = { ...versions.common };
    for (const pin of Object.values(versions.styling)) Object.assign(pins, pin);
    for (const framework of Object.values(versions.framework) as Record<string, object>[]) {
      for (const list of Object.values(framework)) Object.assign(pins, list);
    }
    const vite = Object.entries(pins).filter(([name]) => name === "vite" || /vite/.test(name));
    expect(vite.length).toBeGreaterThan(1);
    for (const [name, version] of vite) {
      const range = SUPPORTED_DEPENDENCIES[name];
      expect(range, `${name} has no supported range in @bdk/vite`).toBeDefined();
      if (range !== undefined) expect(inRange(version, range), `${name} ${version}`).toBe(true);
    }
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
