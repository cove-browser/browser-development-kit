import { existsSync } from "node:fs";
import { mkdir, mkdtemp, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { isAbsolute, join } from "node:path";
import {
  APP_PATCH_PREFIX,
  loadSeries,
  loadSeriesWithApp,
  PatchFormatError,
} from "@bdk/tooling/dist/chromium/common.js";
import {
  applySeries,
  PatchConflictError,
  seriesState,
  UnsafeCheckoutError,
} from "@bdk/tooling/dist/chromium/patch-apply.js";
import { checkPatchSeries, type PatchCheckResult } from "@bdk/tooling/dist/chromium/patch-check.js";
import {
  exportSeries,
  patchIdOf,
  patchIdsForCommits,
} from "@bdk/tooling/dist/chromium/patch-export.js";
import { git } from "@bdk/tooling/dist/git.js";
import { LockBusyError } from "@bdk/tooling/dist/lock.js";
import { BdkHomeError, type BdkPaths, bdkPaths, resolveBdkHome } from "@bdk/tooling/dist/paths.js";
import type { ParsedArgs } from "../args.js";
import type { Context } from "../context.js";
import type { Diagnostic } from "../diagnostics.js";
import { SYNC_FIX } from "../doctor/machine.js";
import { readRelease } from "../doctor/release.js";
import { CliError, ExitCode, type ExitCodeValue, usageError } from "../exit-codes.js";
import type { Project } from "../project.js";
import { DEFAULT_PATCHES_DIRECTORY, seriesDirectory, sourceSettings } from "./source.js";

/** The subcommands of sdk/cli.md section 7. */
export const PATCH_COMMANDS = ["status", "apply", "new", "export", "check", "rebase"] as const;
type PatchCommand = (typeof PATCH_COMMANDS)[number];
/** The subcommands that change the managed checkout, and so hold its lock. */
const LOCKED: readonly PatchCommand[] = ["apply", "new", "export"];

/** BDK's own series as it is embedded: `chromium/patches/<file>`. */
const BDK_PATCHES_PREFIX = "chromium/patches/";

export type PatchOutcome =
  | {
      kind: "report";
      exitCode: ExitCodeValue;
      diagnostics: Diagnostic[];
      data: Record<string, unknown>;
    }
  /** The run went to a child holding the checkout lock, which printed its own report. */
  | { kind: "delegated"; exitCode: number };

function fail(
  code: string,
  message: string,
  fix: string,
  exitCode: ExitCodeValue,
  file?: string,
): CliError {
  return new CliError(
    { code, severity: "error", message, fix, ...(file === undefined ? {} : { file }) },
    exitCode,
  );
}

/** Writes BDK's embedded series to a temporary directory, which is what the tooling reads. */
async function bdkSeriesDir(ctx: Context): Promise<string> {
  const names = ctx.assets.list(BDK_PATCHES_PREFIX);
  if (!names.includes(`${BDK_PATCHES_PREFIX}series`)) {
    throw fail(
      "cli.asset_missing",
      "This bdk has no embedded Chromium patch series.",
      "Reinstall bdk from a release, or rebuild it with `pnpm cli:package`.",
      ExitCode.Environment,
    );
  }
  const dir = await mkdtemp(join(tmpdir(), "bdk-patches-"));
  for (const name of names) {
    await writeFile(join(dir, name.slice(BDK_PATCHES_PREFIX.length)), ctx.assets.readText(name));
  }
  return dir;
}

function homePaths(ctx: Context): BdkPaths {
  try {
    return bdkPaths(resolveBdkHome({ env: ctx.env }));
  } catch (error) {
    if (!(error instanceof BdkHomeError)) throw error;
    throw fail(
      "patch.bdk_home_invalid",
      error.message,
      "Set BDK_HOME to an absolute path with no spaces.",
      ExitCode.Environment,
    );
  }
}

/** The managed checkout's `src`, which must exist for every subcommand that uses it. */
function checkoutSrc(paths: BdkPaths): string {
  const src = join(paths.chromiumManaged, "src");
  if (!existsSync(join(src, ".git"))) {
    throw fail(
      "patch.checkout_missing",
      `There is no managed Chromium checkout at ${src}.`,
      `${SYNC_FIX} \`bdk patch check\` needs no checkout.`,
      ExitCode.Environment,
    );
  }
  return src;
}

/** The file a report names for a patch: the application's own file; BDK's are embedded. */
function patchFile(name: string, appDir: string | undefined): string | undefined {
  return name.startsWith(APP_PATCH_PREFIX) && appDir !== undefined
    ? join(appDir, name.slice(APP_PATCH_PREFIX.length))
    : undefined;
}

function checkDiagnostics(result: PatchCheckResult, appDir: string | undefined): Diagnostic[] {
  const diagnostics: Diagnostic[] = [];
  for (const entry of result.patches) {
    if (entry.status === "applied") continue;
    const file = patchFile(entry.patch, appDir);
    diagnostics.push({
      code: entry.status === "conflict" ? "patch.conflict" : "patch.invalid",
      severity: "error",
      message:
        entry.status === "conflict"
          ? `${entry.patch} does not apply to Chromium ${result.tag}. Conflicting files: ${entry.files.join(", ") || "(see git's output)"}.`
          : `${entry.patch}: ${entry.detail}`,
      ...(file === undefined ? {} : { file }),
      fix: entry.patch.startsWith(APP_PATCH_PREFIX)
        ? "Rebase the patch: `bdk patch apply`, resolve it in the checkout, commit, `bdk patch export`."
        : "BDK's own series does not apply; use the BDK release made for this Chromium version.",
    });
  }
  for (const orphan of result.orphans) {
    diagnostics.push({
      code: "patch.unlisted",
      severity: "warning",
      message: `${orphan} is not listed in its series and is not applied.`,
      fix: "Add it to series, or delete it.",
    });
  }
  return diagnostics;
}

/** The commit after BDK's series in the checkout: the base of the application's patches. */
async function applicationBase(src: string, pin: string, bdkDir: string): Promise<string> {
  const { patches } = loadSeries(bdkDir);
  const commits = (await git(src, ["rev-list", "--reverse", `${pin}..HEAD`]))
    .split("\n")
    .filter(Boolean);
  const head = commits.slice(0, patches.length);
  const have = await patchIdsForCommits(src, head);
  const want = await Promise.all(patches.map((p) => patchIdOf(src, p.text)));
  if (head.length !== patches.length || have.some((id, i) => id !== want[i])) {
    throw fail(
      "patch.base_unknown",
      "The managed checkout does not start with BDK's patch series on the pinned Chromium, so the application's commits cannot be told apart.",
      "Run `bdk patch apply` first, then commit your changes on top.",
      ExitCode.ProjectFailure,
    );
  }
  return head.at(-1) ?? pin;
}

/**
 * `bdk patch <command>` for source mode (sdk/cli.md section 7). It reuses the repository tooling: the series
 * format, `git am --3way`, the deterministic export, the sparse check and the checkout lock.
 */
export async function runPatch(
  ctx: Context,
  args: ParsedArgs,
  project: Project,
  argv: readonly string[],
): Promise<PatchOutcome> {
  const [, sub, operand, ...rest] = args.positionals;
  if (sub === undefined || !(PATCH_COMMANDS as readonly string[]).includes(sub)) {
    throw usageError(
      sub === undefined ? "No patch command given." : `Unknown patch command "${sub}".`,
      `Patch commands: ${PATCH_COMMANDS.join(", ")}.`,
    );
  }
  const command = sub as PatchCommand;
  const takesOperand = command === "new" || command === "rebase";
  if (rest.length > 0 || (operand !== undefined && !takesOperand)) {
    throw usageError(`Too many arguments for \`bdk patch ${command}\`.`);
  }
  if (takesOperand && operand === undefined) {
    throw usageError(
      `\`bdk patch ${command}\` needs ${command === "new" ? "a name" : "a Chromium tag"}.`,
    );
  }
  const settings = await sourceSettings(project.dir, project.configFile);
  if (settings.mode !== "source") {
    throw fail(
      "patch.not_source_mode",
      "`bdk patch` manages Chromium patches for source mode, and this project uses the prebuilt runtime.",
      'Set `mode = "source"` in the [runtime] table of bdk.toml.',
      ExitCode.ProjectFailure,
      project.configFile,
    );
  }
  const release = readRelease(ctx);
  const paths = homePaths(ctx);

  if (LOCKED.includes(command) && ctx.env.BDK_CHECKOUT_LOCKED !== "1") {
    try {
      return { kind: "delegated", exitCode: await ctx.underCheckoutLock(paths, argv) };
    } catch (error) {
      if (!(error instanceof LockBusyError)) throw error;
      throw fail(
        "patch.checkout_locked",
        error.message,
        "Wait for it to finish, then run the command again.",
        ExitCode.Environment,
      );
    }
  }

  const bdkDir = await bdkSeriesDir(ctx);
  const appDir = await seriesDirectory(settings.patches);
  const pin = release.chromium;
  const diagnostics: Diagnostic[] = [];

  try {
    switch (command) {
      case "status": {
        const { patches, orphans } = loadSeriesWithApp(bdkDir, appDir);
        const list = patches.map((p) => ({
          name: p.name,
          origin: p.name.startsWith(APP_PATCH_PREFIX) ? "application" : "bdk",
          files: p.files,
        }));
        const src = join(paths.chromiumManaged, "src");
        const checkout = existsSync(join(src, ".git"))
          ? await seriesState(src, pin.commit, patches)
          : undefined;
        for (const orphan of orphans) {
          diagnostics.push({
            code: "patch.unlisted",
            severity: "warning",
            message: `${orphan} is not listed in its series and is not applied.`,
            fix: "Add it to series, or delete it.",
          });
        }
        if (!args.json) {
          const lines = [`Chromium ${pin.tag} (${pin.commit.slice(0, 12)})`];
          for (const p of list.filter((x) => x.origin === "application")) {
            lines.push(`  ${p.name}`, ...p.files.map((f) => `      ${f}`));
          }
          if (!list.some((x) => x.origin === "application"))
            lines.push("  the application has no patches");
          lines.push(
            `  and BDK's ${list.filter((x) => x.origin === "bdk").length} patches before them`,
          );
          lines.push(
            checkout === undefined
              ? `Managed checkout: none at ${src}`
              : checkout.applied
                ? "Managed checkout: at the pin with every patch applied"
                : `Managed checkout: not the pin plus the series (${checkout.extraCommits} commit(s) above the pin, ${checkout.dirty.length} changed file(s))`,
          );
          lines.push("Run `bdk patch check` to try the series against the pinned Chromium.");
          ctx.stdout(`${lines.join("\n")}\n`);
        }
        return {
          kind: "report",
          exitCode: ExitCode.Success,
          diagnostics,
          data: {
            chromium: pin,
            patches: list,
            checkout:
              checkout === undefined
                ? null
                : {
                    path: src,
                    applied: checkout.applied,
                    commitsAbovePin: checkout.extraCommits,
                    changedFiles: checkout.dirty.length,
                  },
          },
        };
      }
      case "apply":
      case "new": {
        const src = checkoutSrc(paths);
        if (command === "new" && !/^[a-z0-9][a-z0-9-]*$/.test(operand ?? "")) {
          throw usageError(
            `"${operand}" is not a patch name: lower case letters, digits and hyphens.`,
          );
        }
        const result = await applySeries({
          srcDir: src,
          pinCommit: pin.commit,
          patchesDir: bdkDir,
          ...(appDir === undefined ? {} : { appPatchesDir: appDir }),
        });
        if (!args.json) {
          ctx.stdout(
            command === "apply"
              ? `bdk patch apply: ${result.status}, ${result.patches.length} patches, HEAD ${result.head.slice(0, 12)}\n`
              : `Edit files under ${src}, then commit them with the subject "${operand}" and a body that says why:\n` +
                  `  git -C ${src} commit -a\n` +
                  "Then run `bdk patch export` to write the patch into the project.\n",
          );
        }
        return {
          kind: "report",
          exitCode: ExitCode.Success,
          diagnostics,
          data: {
            status: result.status,
            patches: result.patches,
            head: result.head,
            ...(command === "new" ? { base: result.head, name: operand } : {}),
          },
        };
      }
      case "export": {
        if (settings.patches.kind === "list") {
          throw fail(
            "patch.list_not_exportable",
            "[chromium] patches is a list of files, so there is no directory to export into.",
            `Make it a directory with a series file (the default is ${DEFAULT_PATCHES_DIRECTORY}).`,
            ExitCode.ProjectFailure,
            project.configFile,
          );
        }
        const src = checkoutSrc(paths);
        const target = settings.patches.dir;
        await mkdir(target, { recursive: true });
        const dirty = (await git(src, ["status", "--porcelain", "--untracked-files=no"])).trim();
        if (dirty !== "") {
          diagnostics.push({
            code: "patch.uncommitted",
            severity: "warning",
            message: `${src} has uncommitted changes; they are not exported.`,
            fix: "Commit them with a message that says why, then export again.",
          });
        }
        const base = await applicationBase(src, pin.commit, bdkDir);
        const result = await exportSeries({ srcDir: src, pinCommit: base, patchesDir: target });
        if (!args.json) {
          ctx.stdout(
            `bdk patch export: ${result.names.length} patch(es) in ${target}; ${result.written.length} written, ${result.removed.length} removed\n`,
          );
        }
        return {
          kind: "report",
          exitCode: ExitCode.Success,
          diagnostics,
          data: { directory: target, ...result },
        };
      }
      case "check":
      case "rebase": {
        const remote = ctx.env.BDK_PATCH_CHECK_REMOTE;
        if (args.offline && (remote === undefined || !isAbsolute(remote))) {
          throw fail(
            "patch.offline",
            `\`bdk patch ${command}\` fetches Chromium's files, and --offline forbids the network.`,
            "Run it without --offline.",
            ExitCode.Environment,
          );
        }
        const tag = command === "check" ? pin.tag : (operand as string);
        const result = await checkPatchSeries({
          tag,
          patchesDir: bdkDir,
          ...(appDir === undefined ? {} : { appPatchesDir: appDir }),
          ...(command === "check" ? { expectCommit: pin.commit } : {}),
          ...(remote === undefined ? {} : { remote }),
        });
        diagnostics.push(...checkDiagnostics(result, appDir));
        if (!args.json) {
          const failed = result.patches.filter((p) => p.status !== "applied").length;
          ctx.stdout(
            `bdk patch ${command}: ${result.patches.length - failed} of ${result.patches.length} patches apply to Chromium ${tag}${result.commit ? ` (${result.commit.slice(0, 12)})` : ""}\n`,
          );
        }
        return {
          kind: "report",
          exitCode: result.ok ? ExitCode.Success : ExitCode.ProjectFailure,
          diagnostics,
          data: { ...result },
        };
      }
    }
  } catch (error) {
    if (error instanceof PatchConflictError) {
      const file = patchFile(error.patch, appDir);
      throw fail(
        "patch.conflict",
        `${error.patch} does not apply. Conflicting files: ${error.files.join(", ") || "(see git's output)"}.`,
        "Resolve it: apply the series without it, make the change again on top, commit and `bdk patch export`.",
        ExitCode.ProjectFailure,
        file,
      );
    }
    if (error instanceof UnsafeCheckoutError) {
      throw fail(
        "patch.checkout_unsafe",
        error.message,
        "Commit and export your work, or discard it.",
        ExitCode.ProjectFailure,
      );
    }
    if (error instanceof PatchFormatError) {
      throw fail(
        "patch.invalid",
        error.message,
        "Fix the patch or its series; `bdk patch export` writes both.",
        ExitCode.ProjectFailure,
      );
    }
    throw error;
  }
}
