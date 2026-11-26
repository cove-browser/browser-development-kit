import { existsSync, statSync } from "node:fs";
import { readdir, readFile, stat } from "node:fs/promises";
import { join, resolve } from "node:path";
import { loadSeries, PatchFormatError } from "@bdk/tooling/dist/chromium/common.js";
import { BdkHomeError, bdkPaths, resolveBdkHome } from "@bdk/tooling/dist/paths.js";
import { type Manifest, verifyRuntimeDir } from "@bdk/tooling/dist/runtime/manifest.js";
import type { SignatureReport } from "../../platform/platform.js";
import type { ParsedArgs } from "../args.js";
import { directoryDigest } from "../config/digest.js";
import { pathProblem } from "../config/paths.js";
import { isRecord } from "../config/reporter.js";
import type { Context } from "../context.js";
import type { Diagnostic } from "../diagnostics.js";
import { CliError, ExitCode, type ExitCodeValue } from "../exit-codes.js";
import { automationDirectory, pagesDirectory } from "../pages/routes.js";
import { sourceSettings } from "../patch/source.js";
import type { Project } from "../project.js";
import { resolveProject } from "../steps/config.js";
import { frameworkFindings, pageFindings, scriptFindings, sourceConfigDigest } from "./contents.js";
import { viteDependencyFindings } from "./dependencies.js";
import { DOCTOR_GROUPS, type Finding, Findings } from "./findings.js";
import { machineFindings } from "./machine.js";
import { type ReleaseFacts, readRelease } from "./release.js";

/**
 * Where `bdk init`'s template keeps the property lists, read when Xcode names none (outside an Xcode
 * build), so `bdk doctor` checks them as an Xcode build would.
 */
export const TEMPLATE_INFO_PLIST = "Resources/Info.plist";
export const TEMPLATE_ENTITLEMENTS = "Resources/App.entitlements";

export interface DoctorReport {
  exitCode: ExitCodeValue;
  diagnostics: Diagnostic[];
  data: Record<string, unknown>;
}

async function isFile(path: string): Promise<boolean> {
  try {
    return (await stat(path)).isFile();
  } catch {
    return false;
  }
}

/** Whether the project builds pages with Vite or scripts with Rolldown, so it needs Node to install them. */
async function needsNode(project: Project): Promise<boolean> {
  const pages = join(project.dir, await pagesDirectory(project.configFile));
  for (const ext of ["ts", "mts", "js", "mjs"]) {
    if (await isFile(join(pages, `vite.config.${ext}`))) return true;
  }
  try {
    const scripts = await readdir(join(project.dir, await automationDirectory(project.configFile)));
    return scripts.some((name) => /\.(ts|js)$/.test(name) && !name.endsWith(".d.ts"));
  } catch {
    return false;
  }
}

/** The environment the platform reads bundle facts from: Xcode's, or the template's paths outside Xcode. */
async function factsEnv(ctx: Context, project: Project): Promise<NodeJS.ProcessEnv> {
  if (ctx.platform.isIdeBuild(ctx.env)) return ctx.env;
  const env: NodeJS.ProcessEnv = { ...ctx.env, SRCROOT: project.dir };
  if (env.INFOPLIST_FILE === undefined && (await isFile(join(project.dir, TEMPLATE_INFO_PLIST)))) {
    env.INFOPLIST_FILE = TEMPLATE_INFO_PLIST;
  }
  if (
    env.CODE_SIGN_ENTITLEMENTS === undefined &&
    (await isFile(join(project.dir, TEMPLATE_ENTITLEMENTS)))
  ) {
    env.CODE_SIGN_ENTITLEMENTS = TEMPLATE_ENTITLEMENTS;
  }
  return env;
}

/** The BDK version a project's `Package.resolved` pins, if it has one. */
async function resolvedBdkVersion(
  project: Project,
): Promise<{ file: string; version?: string } | undefined> {
  const candidates = [join(project.dir, "Package.resolved")];
  try {
    for (const entry of await readdir(project.dir)) {
      if (entry.endsWith(".xcodeproj")) {
        candidates.push(
          join(
            project.dir,
            entry,
            "project.xcworkspace",
            "xcshareddata",
            "swiftpm",
            "Package.resolved",
          ),
        );
      }
    }
  } catch {
    // The project directory exists; findProject found bdk.toml in it.
  }
  for (const file of candidates) {
    let parsed: unknown;
    try {
      parsed = JSON.parse(await readFile(file, "utf8"));
    } catch {
      continue;
    }
    const pins = isRecord(parsed) && Array.isArray(parsed.pins) ? parsed.pins : [];
    for (const pin of pins) {
      if (!isRecord(pin)) continue;
      const identity = String(pin.identity ?? "");
      const location = String(pin.location ?? "");
      if (identity === "bdk" || /browser-development-kit(\.git)?$/.test(location)) {
        const state = isRecord(pin.state) ? pin.state : {};
        return typeof state.version === "string" ? { file, version: state.version } : { file };
      }
    }
  }
  return undefined;
}

/** The runtime this project resolved (`.bdk/runtime.json`), verified against its manifest. */
async function resolvedRuntime(
  ctx: Context,
  project: Project,
  f: Findings,
): Promise<Manifest | undefined> {
  const pointer = join(project.dir, ".bdk", "runtime.json");
  let key: string;
  try {
    const parsed: unknown = JSON.parse(await readFile(pointer, "utf8"));
    if (!isRecord(parsed) || typeof parsed.source_key !== "string") throw new Error("no key");
    key = parsed.source_key;
  } catch {
    return undefined;
  }
  let home: string;
  try {
    home = resolveBdkHome({ env: ctx.env });
  } catch (error) {
    if (!(error instanceof BdkHomeError)) throw error;
    f.error(
      "doctor.bdk_home_invalid",
      error.message,
      "Set BDK_HOME to an absolute path with no spaces.",
    );
    return undefined;
  }
  const dir = bdkPaths(home).runtime(key);
  if (!existsSync(dir)) {
    f.error(
      "doctor.runtime_missing",
      `The project resolved runtime ${key}, which is not in the store (${dir}).`,
      "Resolve the runtime again; it is downloaded or rebuilt into the store.",
      pointer,
    );
    return undefined;
  }
  try {
    return (await verifyRuntimeDir(dir, key)).manifest;
  } catch (error) {
    f.error(
      "doctor.runtime_modified",
      `The runtime in the store does not match its manifest: ${(error as Error).message}`,
      `Delete it (chmod -R u+w "${dir}" && rm -rf "${dir}") and resolve it again.`,
      pointer,
    );
    return undefined;
  }
}

/** Project: the configuration, the property lists, the Swift package's BDK version, the runtime, grants. */
async function projectFindings(
  ctx: Context,
  project: Project,
  release: ReleaseFacts,
  mode: "prebuilt" | "source",
): Promise<{ findings: Findings; resolved?: Record<string, unknown>; manifest?: Manifest }> {
  const f = new Findings("project");
  // Every diagnostic the resolver has, which includes those added after this command was written.
  const result = await resolveProject(ctx, project, await factsEnv(ctx, project));
  for (const diagnostic of result.diagnostics) f.add(diagnostic);
  const routes = isRecord(result.resolved?.routes) ? result.resolved.routes : {};
  for (const [route, entry] of Object.entries(routes)) {
    if (!isRecord(entry) || entry.mode !== "bundled" || !Array.isArray(entry.capabilities))
      continue;
    const grants = entry.capabilities.filter((c): c is string => typeof c === "string");
    f.note(
      "doctor.page_capabilities",
      grants.length === 0
        ? `The page "${route}" is granted no capabilities.`
        : `The page "${route}" is granted ${grants.join(", ")}.`,
      project.configFile,
    );
  }
  const pinned = await resolvedBdkVersion(project);
  if (pinned === undefined) {
    f.note(
      "doctor.package_unresolved",
      "No Package.resolved pins the BDK package yet; Xcode writes it when it first resolves packages.",
    );
  } else if (pinned.version !== undefined && pinned.version !== ctx.version) {
    f.warning(
      "doctor.bdk_version_mismatch",
      `Package.resolved pins BDK ${pinned.version}, but this bdk is ${ctx.version}; the CLI, SDK and runtime must be one version.`,
      "Run the bdk that ships with the resolved package, or update the package to this version.",
      pinned.file,
    );
  }
  const manifest = await resolvedRuntime(ctx, project, f);
  if (manifest === undefined && mode === "prebuilt" && release.runtime === null) {
    f.warning(
      "doctor.runtime_unpublished",
      `BDK ${ctx.version} has no published runtime, so the application can run only on the stub runtime.`,
      "Build one in source mode, or use a BDK release that publishes a runtime.",
    );
  }
  return {
    findings: f,
    ...(result.resolved === undefined ? {} : { resolved: result.resolved }),
    ...(manifest === undefined ? {} : { manifest }),
  };
}

/**
 * Source mode: the source-built runtime's ABI hash against the prebuilt runtime's for this BDK version, which
 * the release facts record when one is published.
 */
function sourceAbiFindings(f: Findings, release: ReleaseFacts, manifest: Manifest): void {
  if (release.runtime === null) {
    f.note(
      "doctor.prebuilt_abi_unknown",
      "This BDK version has no published prebuilt runtime, so the source-built runtime's ABI hash is not compared with one.",
    );
    return;
  }
  if (manifest.bdk_abi.hash !== release.runtime.abi_hash) {
    f.warning(
      "doctor.source_abi_differs",
      `The source-built runtime reports ABI hash ${manifest.bdk_abi.hash.slice(0, 12)}, but the prebuilt runtime of this BDK version reports ${release.runtime.abi_hash.slice(0, 12)}; the two are not the same contract.`,
      "Rebuild the runtime from this BDK version's sources; a patch must not change BDK's public C headers.",
    );
  }
}

/**
 * Compatibility: the runtime's ABI against the SDK's; in source mode, the application's patches and the
 * source-built ABI against the prebuilt one; the pages' pins against the Vite preset.
 */
async function compatibilityFindings(
  project: Project,
  release: ReleaseFacts,
  manifest: Manifest | undefined,
): Promise<Findings> {
  const f = new Findings("compatibility");
  if (manifest !== undefined) {
    if (
      manifest.bdk_abi.version !== release.abi.version ||
      manifest.bdk_abi.hash !== release.abi.hash
    ) {
      f.error(
        "doctor.abi_mismatch",
        `The runtime reports ABI ${manifest.bdk_abi.version} (${manifest.bdk_abi.hash.slice(0, 12)}), but this BDK's Swift package was compiled against ABI ${release.abi.version} (${release.abi.hash.slice(0, 12)}); the application would fail at start.`,
        "Resolve the runtime of this BDK version, or rebuild it from this version's sources.",
      );
    }
    if (manifest.chromium.commit !== release.chromium.commit) {
      f.warning(
        "doctor.runtime_other_chromium",
        `The runtime is Chromium ${manifest.chromium.tag}, not ${release.chromium.tag}, the pin of this BDK version.`,
        "Resolve the runtime again for this version.",
      );
    }
  }
  const settings = await sourceSettings(project.dir, project.configFile);
  if (settings.mode === "source" && manifest !== undefined) {
    sourceAbiFindings(f, release, manifest);
  }
  await viteDependencyFindings(f, project);
  if (settings.mode === "source" && settings.patches.kind === "directory") {
    try {
      const { patches, orphans } = loadSeries(settings.patches.dir);
      for (const orphan of orphans) {
        f.warning(
          "doctor.patch_unlisted",
          `${orphan} is in ${settings.patches.dir} but not in its series, so it is not applied.`,
          "Add it to series, or delete it.",
          join(settings.patches.dir, orphan),
        );
      }
      if (patches.length > 0) {
        f.note(
          "doctor.application_patches",
          `The application applies ${patches.length} patch(es) after BDK's: ${patches.map((p) => p.name).join(", ")}. \`bdk patch check\` tries them against the pinned Chromium.`,
        );
      }
    } catch (error) {
      if (!(error instanceof PatchFormatError)) throw error;
      f.error(
        "doctor.patch_invalid",
        `The application's patches cannot be read: ${error.message}`,
        "Fix the patch or the series; `bdk patch export` writes both.",
        join(settings.patches.dir, "series"),
      );
    }
  }
  return f;
}

/**
 * Each bundled extension the application's resolved file lists, against the copy the runtime loads from
 * `Contents/Resources/<path>` (extensions.md section 9).
 */
function bundledExtensionFindings(
  f: Findings,
  appPath: string,
  inBundle: Record<string, unknown>,
): void {
  const extensions = isRecord(inBundle.extensions) ? inBundle.extensions : {};
  const bundled = isRecord(extensions.bundled) ? extensions.bundled : {};
  for (const [name, entry] of Object.entries(bundled)) {
    // The resolver writes a path inside the project; anything else is not read outside the bundle.
    if (!isRecord(entry) || typeof entry.path !== "string" || pathProblem(entry.path)) continue;
    const copy = join(appPath, "Contents", "Resources", entry.path);
    if (!existsSync(copy) || !statSync(copy).isDirectory()) {
      f.error(
        "doctor.bundled_extension_missing",
        `The application has no copy of the bundled extension "${name}" at Contents/Resources/${entry.path}.`,
        "Build the application again; its `bdk build extensions` step copies the extension.",
        copy,
      );
    } else if (directoryDigest(copy) !== entry.digest) {
      f.error(
        "doctor.bundled_extension_modified",
        `The bundled extension "${name}" in the application does not match the digest in its bdk.resolved.json.`,
        "Build the application again; its `bdk build extensions` step copies the extension.",
        copy,
      );
    }
  }
}

/**
 * Built application: its configuration against `bdk.toml`, its pages, scripts and runtime against the
 * digests the build recorded, and its signature.
 */
async function bundleFindings(
  ctx: Context,
  project: Project,
  appPath: string,
  resolved: Record<string, unknown> | undefined,
  manifest: Manifest | undefined,
): Promise<Findings> {
  const f = new Findings("bundle");
  if (!existsSync(join(appPath, "Contents"))) {
    throw new CliError(
      {
        code: "doctor.bundle_missing",
        severity: "error",
        message: `${appPath} is not an application bundle.`,
        file: appPath,
        fix: "Pass the built .app, for example build/Release/<Name>.app.",
      },
      ExitCode.Usage,
    );
  }
  const bundled = join(appPath, "Contents", "Resources", "bdk.resolved.json");
  let inBundle: unknown;
  try {
    inBundle = JSON.parse(await readFile(bundled, "utf8"));
  } catch {
    f.error(
      "doctor.bundle_config_missing",
      "The application carries no readable bdk.resolved.json, so the runtime cannot start.",
      "Build it with the template's build phases, which copy the resolved file into Resources.",
      bundled,
    );
  }
  if (isRecord(inBundle) && resolved !== undefined) {
    if (sourceConfigDigest(inBundle) !== resolved.config_digest) {
      f.error(
        "doctor.bundle_config_stale",
        "The application's bdk.resolved.json does not match bdk.toml as it is now.",
        "Build the application again.",
        bundled,
      );
    }
  }
  const resources = join(appPath, "Contents", "Resources");
  if (isRecord(inBundle)) {
    await pageFindings(f, project, resources, inBundle, resolved);
    scriptFindings(f, resources, inBundle);
    bundledExtensionFindings(f, appPath, inBundle);
  }
  await frameworkFindings(f, appPath, manifest);
  await signatureFindings(f, ctx, appPath);
  return f;
}

/** The signature: it verifies, its helpers' teams, Developer ID, Gatekeeper and the stapled ticket. */
async function signatureFindings(f: Findings, ctx: Context, appPath: string): Promise<void> {
  const signature = await ctx.platform.inspectSignature(appPath);
  if (!signature.valid) {
    f.error(
      "doctor.signature_invalid",
      `The application's signature does not verify: ${signature.detail ?? "no detail"}.`,
      "Sign it again with `bdk build sign`, which signs the runtime inside-out.",
      appPath,
    );
  }
  if (signature.team === undefined) {
    f.note("doctor.ad_hoc", "The application is signed ad hoc; it runs only on this Mac.", appPath);
  } else {
    for (const helper of signature.helpers) {
      if (helper.team !== signature.team) {
        f.error(
          "doctor.helper_team_mismatch",
          `${helper.path} is signed by ${helper.team ?? "no team"}, not the application's team ${signature.team}.`,
          "Run `bdk build sign` with the application's identity.",
          helper.path,
        );
      }
    }
    if (!signature.stapled) {
      f.warning(
        "doctor.not_notarized",
        "No notarization ticket is stapled to the application, so Gatekeeper checks it online or refuses it.",
        "Notarize the application and staple the ticket (`xcrun notarytool`, `xcrun stapler staple`).",
        appPath,
      );
    }
    developerIdFindings(f, signature, appPath);
  }
}

/** The leaf certificate of a signature that Gatekeeper accepts from outside the App Store. */
export const DEVELOPER_ID_PREFIX = "Developer ID Application:";

/**
 * A team signature: whether it is a Developer ID one, and, when it is, whether Gatekeeper accepts it as
 * it would on another Mac (that is, notarized).
 */
function developerIdFindings(f: Findings, signature: SignatureReport, appPath: string): void {
  const authority = signature.authority ?? "an unknown certificate";
  if (!authority.startsWith(DEVELOPER_ID_PREFIX)) {
    f.warning(
      "doctor.not_developer_id",
      `The application is signed with ${authority}, not a Developer ID Application certificate, so Gatekeeper refuses it on other Macs.`,
      "To distribute it outside the App Store, sign it with a Developer ID Application identity (`bdk build sign --identity`).",
      appPath,
    );
    return;
  }
  const gatekeeper = signature.gatekeeper;
  if (gatekeeper === undefined) return;
  if (!gatekeeper.accepted) {
    f.warning(
      "doctor.gatekeeper_rejected",
      `Gatekeeper rejects the application (${gatekeeper.source ?? gatekeeper.detail ?? "no reason given"}), so it does not open on other Macs.`,
      "Notarize the application with `xcrun notarytool submit --wait`, then staple the ticket with `xcrun stapler staple`.",
      appPath,
    );
  } else {
    f.note(
      "doctor.gatekeeper_accepted",
      `Gatekeeper accepts the application: ${gatekeeper.source ?? "accepted"}, signed by ${authority}.`,
      appPath,
    );
  }
}

/** `bdk doctor`: the machine, the project, compatibility and, with `--bundle`, the built application. */
export async function runDoctor(
  ctx: Context,
  args: ParsedArgs,
  project: Project,
): Promise<DoctorReport> {
  const release = readRelease(ctx);
  const settings = await sourceSettings(project.dir, project.configFile);
  const toolchain = await ctx.platform.toolchain(ctx.env);
  const projectResult = await projectFindings(ctx, project, release, settings.mode);
  const groups: Finding[][] = [
    (
      await machineFindings({
        env: ctx.env,
        toolchain,
        needsNode: await needsNode(project),
        sourceMode: settings.mode === "source",
        release,
        ...(projectResult.manifest === undefined
          ? {}
          : { runtimeMinOs: projectResult.manifest.target.min_os }),
      })
    ).list,
    projectResult.findings.list,
    (await compatibilityFindings(project, release, projectResult.manifest)).list,
  ];
  if (args.bundle !== undefined) {
    groups.push(
      (
        await bundleFindings(
          ctx,
          project,
          resolve(ctx.cwd, args.bundle),
          projectResult.resolved,
          projectResult.manifest,
        )
      ).list,
    );
  }
  const findings = groups.flat();
  const diagnostics = findings.map((f) => f.diagnostic);
  const summary = Object.fromEntries(
    DOCTOR_GROUPS.filter((g) => g !== "bundle" || args.bundle !== undefined).map((group) => {
      const inGroup = findings.filter((f) => f.group === group).map((f) => f.diagnostic);
      return [
        group,
        {
          errors: inGroup.filter((d) => d.severity === "error").length,
          warnings: inGroup.filter((d) => d.severity === "warning").length,
          notes: inGroup.filter((d) => d.severity === "note").length,
          codes: inGroup.map((d) => d.code),
        },
      ];
    }),
  );
  return {
    exitCode: diagnostics.some((d) => d.severity === "error")
      ? ExitCode.ProjectFailure
      : ExitCode.Success,
    diagnostics,
    data: { mode: settings.mode, toolchain, groups: summary },
  };
}

/** The human summary on standard output: one line per group. */
export function doctorSummary(data: Record<string, unknown>): string {
  const groups = isRecord(data.groups) ? data.groups : {};
  const lines = Object.entries(groups).map(([group, counts]) => {
    const c = counts as { errors: number; warnings: number; notes: number };
    const parts = [
      c.errors > 0 ? `${c.errors} error${c.errors === 1 ? "" : "s"}` : "",
      c.warnings > 0 ? `${c.warnings} warning${c.warnings === 1 ? "" : "s"}` : "",
      c.notes > 0 ? `${c.notes} note${c.notes === 1 ? "" : "s"}` : "",
    ].filter(Boolean);
    return `${group.padEnd(14)} ${parts.length === 0 ? "ok" : parts.join(", ")}`;
  });
  return `bdk doctor\n${lines.map((l) => `  ${l}`).join("\n")}\n`;
}
