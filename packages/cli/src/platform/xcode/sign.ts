import { mkdtemp, readdir, realpath, rm, stat, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { basename, join } from "node:path";
import { CliError, ExitCode } from "../../core/exit-codes.js";
import { checkAppEntitlements } from "../../core/signing/entitlements.js";
import { type BundleLayout, type PlanAction, planSigning } from "../../core/signing/plan.js";
import type { SignOutcome, SignRequest } from "../../core/signing/types.js";
import { ToolError, type ToolRunner } from "./tools.js";

export const AD_HOC = "-";

async function isDirectory(path: string): Promise<boolean> {
  try {
    return (await stat(path)).isDirectory();
  } catch {
    return false;
  }
}

function projectError(code: string, message: string, fix: string, file?: string): CliError {
  return new CliError(
    { code, severity: "error", message, ...(file === undefined ? {} : { file }), fix },
    ExitCode.ProjectFailure,
  );
}

/** Runs a tool; a tool that is not installed is an environment error (exit 3). */
async function run(runner: ToolRunner, command: string, args: readonly string[]) {
  try {
    return await runner(command, args);
  } catch (error) {
    if (error instanceof ToolError && error.code === "ENOENT") {
      throw new CliError(
        {
          code: "cli.tool_missing",
          severity: "error",
          message: `\`${command}\` was not found.`,
          fix: "Install the Xcode command line tools: `xcode-select --install`.",
        },
        ExitCode.Environment,
      );
    }
    throw error;
  }
}

async function readPlistJson(runner: ToolRunner, file: string): Promise<Record<string, unknown>> {
  try {
    const { stdout } = await run(runner, "plutil", ["-convert", "json", "-o", "-", file]);
    const parsed: unknown = JSON.parse(stdout);
    if (typeof parsed === "object" && parsed !== null && !Array.isArray(parsed)) {
      return parsed as Record<string, unknown>;
    }
  } catch (error) {
    if (error instanceof CliError) throw error;
  }
  throw projectError(
    "config.plist_unreadable",
    `${file} is not a valid property list dictionary.`,
    "Run `plutil -lint` on the file and fix what it reports.",
    file,
  );
}

/** Finds the runtime framework in the application and what is nested in it. */
export async function discoverLayout(appPath: string, runner: ToolRunner): Promise<BundleLayout> {
  const infoPlist = join(appPath, "Contents", "Info.plist");
  if (!(await isDirectory(appPath))) {
    throw projectError(
      "sign.bundle_missing",
      `${appPath} is not an application bundle.`,
      "Pass the built .app with --bundle, after the build has produced it.",
      appPath,
    );
  }
  let appBundleId: string;
  try {
    const { stdout } = await run(runner, "plutil", [
      "-extract",
      "CFBundleIdentifier",
      "raw",
      "-o",
      "-",
      infoPlist,
    ]);
    appBundleId = stdout.trim();
  } catch (error) {
    if (error instanceof CliError) throw error;
    appBundleId = "";
  }
  if (appBundleId === "") {
    throw projectError(
      "sign.bundle_id_missing",
      "The application's Info.plist has no CFBundleIdentifier.",
      "Set CFBundleIdentifier; helper identifiers are derived from it.",
      infoPlist,
    );
  }

  const frameworksDir = join(appPath, "Contents", "Frameworks");
  const frameworks = (await readdir(frameworksDir).catch(() => [] as string[])).filter((name) =>
    name.endsWith(".framework"),
  );
  if (frameworks.length !== 1) {
    throw projectError(
      "sign.no_runtime",
      frameworks.length === 0
        ? `${frameworksDir} holds no runtime framework.`
        : `${frameworksDir} holds more than one framework: ${frameworks.join(", ")}.`,
      "Embed the runtime in Contents/Frameworks before signing; the application embeds exactly one.",
      frameworksDir,
    );
  }
  const frameworkPath = await realpath(join(frameworksDir, frameworks[0] as string));
  const versionDir = (await isDirectory(join(frameworkPath, "Versions", "Current")))
    ? await realpath(join(frameworkPath, "Versions", "Current"))
    : frameworkPath;

  const helpersDir = join(versionDir, "Helpers");
  const librariesDir = join(versionDir, "Libraries");
  const helperEntries = (await readdir(helpersDir).catch(() => [] as string[])).sort();
  const libraryEntries = (await readdir(librariesDir).catch(() => [] as string[])).sort();
  return {
    appPath,
    appBundleId,
    frameworkPath,
    frameworkPlist: join(versionDir, "Resources", "Info.plist"),
    libraries: libraryEntries.filter((n) => n.endsWith(".dylib")).map((n) => join(librariesDir, n)),
    executables: helperEntries.filter((n) => !n.endsWith(".app")).map((n) => join(helpersDir, n)),
    helpers: helperEntries.filter((n) => n.endsWith(".app")).map((n) => join(helpersDir, n)),
  };
}

function plistXml(entitlements: Readonly<Record<string, boolean>>): string {
  const entries = Object.entries(entitlements)
    .map(([key, value]) => `\t<key>${key}</key>\n\t<${value}/>`)
    .join("\n");
  return `<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
${entries}
</dict>
</plist>
`;
}

/** The `codesign` arguments for one sign action. `entitlementsPath` is the file to pass, if any. */
export function codesignArgs(
  action: Extract<PlanAction, { kind: "sign" }>,
  identity: string,
  entitlementsPath: string | undefined,
): string[] {
  const args = ["--sign", identity, "--force"];
  // A secure timestamp is required for notarization; an ad hoc signature cannot have one.
  if (identity !== AD_HOC) args.push("--timestamp");
  if (action.identifier !== undefined) args.push("--identifier", action.identifier);
  if (action.options.length > 0) args.push("--options", action.options.join(","));
  if (entitlementsPath !== undefined) args.push("--entitlements", entitlementsPath);
  args.push(action.path);
  return args;
}

async function teamOf(runner: ToolRunner, path: string): Promise<string | undefined> {
  const { stderr, stdout } = await run(runner, "codesign", ["--display", "--verbose=2", path]);
  return /^TeamIdentifier=(.+)$/m.exec(`${stderr}\n${stdout}`)?.[1]?.trim();
}

/**
 * Signs the application and the runtime nested in it. Checks the application's entitlements first and
 * changes nothing when one is on the deny list.
 */
export async function signBundle(request: SignRequest, runner: ToolRunner): Promise<SignOutcome> {
  let appEntitlements: Record<string, unknown> | undefined;
  if (request.entitlementsFile !== undefined) {
    if (
      !(await stat(request.entitlementsFile).then(
        (s) => s.isFile(),
        () => false,
      ))
    ) {
      throw projectError(
        "sign.entitlements_missing",
        `The entitlements file ${request.entitlementsFile} does not exist.`,
        "Fix CODE_SIGN_ENTITLEMENTS or --entitlements.",
        request.entitlementsFile,
      );
    }
    appEntitlements = await readPlistJson(runner, request.entitlementsFile);
    const denied = checkAppEntitlements(appEntitlements, request.entitlementsFile);
    if (denied.length > 0) return { diagnostics: denied, signed: [] };
  }

  const layout = await discoverLayout(request.appPath, runner);
  const plan = planSigning({
    layout,
    adHoc: request.identity === AD_HOC,
    ...(request.entitlementsFile === undefined
      ? {}
      : { appEntitlementsFile: request.entitlementsFile }),
  });

  const scratch = await mkdtemp(join(tmpdir(), "bdk-sign-"));
  const signed: string[] = [];
  try {
    for (const [index, action] of plan.entries()) {
      if (action.kind === "set-identifier") {
        await run(runner, "plutil", [
          "-replace",
          "CFBundleIdentifier",
          "-string",
          action.identifier,
          action.plist,
        ]);
        continue;
      }
      let entitlementsPath = action.entitlementsFile;
      if (action.entitlements !== undefined) {
        entitlementsPath = join(scratch, `${index}-${basename(action.path)}.plist`);
        await writeFile(entitlementsPath, plistXml(action.entitlements));
      }
      await run(runner, "codesign", codesignArgs(action, request.identity, entitlementsPath));
      signed.push(action.path);
    }

    try {
      await run(runner, "codesign", [
        "--verify",
        "--deep",
        "--strict",
        "--verbose=2",
        request.appPath,
      ]);
    } catch (error) {
      if (error instanceof CliError) throw error;
      throw new CliError(
        {
          code: "sign.verify_failed",
          severity: "error",
          message: `The signature of ${request.appPath} does not verify: ${
            error instanceof ToolError ? error.stderr.trim() : String(error)
          }`,
          file: request.appPath,
          fix: "Re-run the build; if it persists, report the runtime and the signing identity.",
        },
        ExitCode.Integrity,
      );
    }
    if (request.team !== undefined && request.identity !== AD_HOC) {
      const team = await teamOf(runner, request.appPath);
      if (team !== request.team) {
        throw new CliError(
          {
            code: "sign.team_mismatch",
            severity: "error",
            message: `The application is signed by team ${team ?? "(none)"}, not ${request.team}.`,
            file: request.appPath,
            fix: "Use a signing identity that belongs to the team, or correct --team.",
          },
          ExitCode.Integrity,
        );
      }
    }
  } finally {
    await rm(scratch, { recursive: true, force: true });
  }
  return { diagnostics: [], signed };
}
