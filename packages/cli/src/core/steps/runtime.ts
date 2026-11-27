import { existsSync } from "node:fs";
import { BdkHomeError, type BdkPaths, currentBdkPaths } from "@bdk/tooling/dist/paths.js";
import { download } from "@bdk/tooling/dist/runtime/backends.js";
import { computeSourceIdentity, sourceKey } from "@bdk/tooling/dist/runtime/identity.js";
import { type VerifiedRuntime, verifyRuntimeDir } from "@bdk/tooling/dist/runtime/manifest.js";
import type { RuntimeRecord } from "@bdk/tooling/dist/runtime/pin.js";
import { fetchRuntime, RuntimeResolveError } from "@bdk/tooling/dist/runtime/resolver.js";
import type { Context } from "../context.js";
import { readRelease } from "../doctor/release.js";
import { CliError, ExitCode, type ExitCodeValue, usageError } from "../exit-codes.js";
import { seriesDirectory, sourceSettings } from "../patch/source.js";
import { writeRuntimePointer } from "../runtime/project-runtime.js";
import type { Step, StepResult } from "./step.js";

function fail(code: string, message: string, fix: string, exitCode: ExitCodeValue): CliError {
  return new CliError({ code, severity: "error", message, fix }, exitCode);
}

function storePaths(ctx: Context): BdkPaths {
  try {
    return currentBdkPaths({ env: ctx.env });
  } catch (error) {
    if (!(error instanceof BdkHomeError)) throw error;
    throw fail(
      "runtime.bdk_home_invalid",
      error.message,
      "Set BDK_HOME to an absolute path with no spaces.",
      ExitCode.Environment,
    );
  }
}

/** The runtime mode: `BDK_RUNTIME_MODE` when set, else `runtime.mode` from `bdk.toml`. */
export function runtimeMode(
  env: NodeJS.ProcessEnv,
  configured: "prebuilt" | "source",
): "prebuilt" | "source" {
  const given = env.BDK_RUNTIME_MODE;
  if (given === undefined || given === "") return configured;
  if (given === "prebuilt" || given === "source") return given;
  throw usageError(
    `BDK_RUNTIME_MODE must be "prebuilt" or "source", not "${given}".`,
    "Unset it to use runtime.mode from bdk.toml.",
  );
}

/** Verifies a store entry, or explains how to replace it. */
async function verified(dir: string, key: string): Promise<VerifiedRuntime> {
  try {
    return await verifyRuntimeDir(dir, key);
  } catch (error) {
    throw fail(
      "runtime.store_modified",
      `The runtime ${key} in the store does not match its manifest: ${(error as Error).message}`,
      `Delete it (chmod -R u+w "${dir}" && rm -rf "${dir}") and run \`bdk build runtime\` again.`,
      ExitCode.Integrity,
    );
  }
}

/** Prebuilt: the runtime this BDK release publishes, from the store or downloaded into it. */
async function prebuilt(ctx: Context, args: { offline: boolean }, paths: BdkPaths) {
  const record: RuntimeRecord | null = readRelease(ctx).runtime;
  if (record === null) {
    throw fail(
      "runtime.unpublished",
      `BDK ${ctx.version} publishes no runtime, so a prebuilt one cannot be resolved.`,
      'Run on the stub runtime, or set runtime.mode = "source" and run `bdk build runtime --allow-source-build` with BDK_SOURCE_ROOT set to a BDK checkout.',
      ExitCode.ProjectFailure,
    );
  }
  const key = record.source_key;
  const dir = paths.runtime(key);
  let origin: "store" | "downloaded" = "store";
  if (!existsSync(dir)) {
    if (args.offline) {
      throw fail(
        "runtime.offline",
        `The runtime ${key} is not in the store, and --offline forbids downloading it.`,
        "Run once without --offline to download it.",
        ExitCode.Environment,
      );
    }
    try {
      await fetchRuntime(key, record, paths, ctx.downloadRuntime ?? download);
    } catch (error) {
      if (!(error instanceof RuntimeResolveError)) throw error;
      if (error.code === "download" || error.code === "missing") {
        throw fail(
          "runtime.download_failed",
          error.message,
          "Check the network, then run `bdk build runtime` again.",
          ExitCode.Environment,
        );
      }
      // "digest": the archive, its manifest or its ABI is not what the release records; "integrity": the
      // unpacked files are not their manifest's.
      throw fail(
        error.code === "digest" ? "runtime.digest_mismatch" : "runtime.archive_invalid",
        error.message,
        "Nothing was installed. Report the release: its archive does not match what it records.",
        ExitCode.Integrity,
      );
    }
    origin = "downloaded";
  }
  const runtime = await verified(dir, key);
  if (runtime.manifestSha256 !== record.manifest_sha256) {
    throw fail(
      "runtime.store_modified",
      `The runtime ${key} in the store has manifest digest ${runtime.manifestSha256}, but this BDK release publishes ${record.manifest_sha256}.`,
      `Delete it (chmod -R u+w "${dir}" && rm -rf "${dir}") and run \`bdk build runtime\` again.`,
      ExitCode.Integrity,
    );
  }
  return { key, runtime, origin };
}

/**
 * Source: a runtime built from a BDK checkout (`BDK_SOURCE_ROOT`) with the application's patches. A
 * Chromium build never starts unless asked for (`--allow-source-build` or `BDK_RUNTIME_MODE=source`).
 */
async function source(
  ctx: Context,
  allowed: boolean,
  paths: BdkPaths,
  patches: Parameters<typeof seriesDirectory>[0],
) {
  const root = ctx.env.BDK_SOURCE_ROOT;
  if (root === undefined || root === "") {
    throw fail(
      "runtime.source_root_missing",
      "runtime.mode is source, and no BDK checkout is named to build the runtime from.",
      "Set BDK_SOURCE_ROOT to a checkout of BDK at this version (its chromium/ directory holds the pin, the patches and the overlay).",
      ExitCode.Environment,
    );
  }
  const appPatches = await seriesDirectory(patches);
  const identity = await computeSourceIdentity({
    repoRoot: root,
    ...(appPatches === undefined ? {} : { appPatchesDir: appPatches }),
  });
  const key = sourceKey(identity);
  const dir = paths.runtime(key);
  let origin: "store" | "built" = "store";
  if (!existsSync(dir)) {
    if (!allowed) {
      throw fail(
        "runtime.source_build_not_allowed",
        `The source-built runtime ${key} is not in the store, and building it takes hours, so a build does not start by itself.`,
        "Run `bdk build runtime --allow-source-build` (or set BDK_RUNTIME_MODE=source) once; later builds of the same inputs are found in the store.",
        ExitCode.ProjectFailure,
      );
    }
    const extra = appPatches === undefined ? [] : ["--app-patches", appPatches];
    for (const target of ["chromium:build", "chromium:package"]) {
      const result = await ctx.exec(
        "pnpm",
        [
          "exec",
          "nx",
          "run",
          target,
          "--output-style=stream",
          "--",
          "--config",
          "release",
          ...extra,
        ],
        { cwd: root, env: ctx.env },
      );
      if (result.code !== 0) {
        const tail = `${result.stdout}\n${result.stderr}`.trim().split("\n").slice(-6).join("\n");
        throw fail(
          "runtime.source_build_failed",
          `\`${target}\` failed in ${root} (exit ${result.code ?? "signal"}):\n${tail}`,
          "Fix what it reports (a patch that no longer applies names the patch and the file), then run `bdk build runtime --allow-source-build` again.",
          ExitCode.ProjectFailure,
        );
      }
    }
    if (!existsSync(dir)) {
      throw fail(
        "runtime.source_build_failed",
        `The build finished, but the store has no runtime ${key} (${dir}).`,
        "Check that BDK_HOME is the same for this command and the checkout's tooling.",
        ExitCode.Environment,
      );
    }
    origin = "built";
  }
  return { key, runtime: await verified(dir, key), origin };
}

/**
 * `bdk build runtime`: the verified runtime this project runs on, in the store under `BDK_HOME`, recorded
 * in `.bdk/runtime.json` for `bdk build embed` and `bdk doctor` (sdk/cli.md sections 5 and 7).
 */
export const runtimeStep: Step = {
  name: "runtime",
  flags: [],
  // With no step it runs only when there is a runtime to resolve: a published one, or a source build
  // asked for. A BDK release with neither builds applications for the stub.
  applies: ({ ctx, args }) =>
    args.allowSourceBuild ||
    ctx.env.BDK_RUNTIME_MODE === "source" ||
    (() => {
      try {
        return readRelease(ctx).runtime !== null;
      } catch {
        return false;
      }
    })(),
  async run({ project, ctx, args }): Promise<StepResult> {
    const settings = await sourceSettings(project.dir, project.configFile);
    const mode = runtimeMode(ctx.env, settings.mode);
    const paths = storePaths(ctx);
    const resolved =
      mode === "prebuilt"
        ? await prebuilt(ctx, args, paths)
        : await source(
            ctx,
            args.allowSourceBuild || ctx.env.BDK_RUNTIME_MODE === "source",
            paths,
            settings.patches,
          );
    await writeRuntimePointer(project.dir, {
      source_key: resolved.key,
      manifest_sha256: resolved.runtime.manifestSha256,
    });
    return {
      diagnostics: [],
      exitCode: ExitCode.Success,
      data: {
        mode,
        sourceKey: resolved.key,
        manifestSha256: resolved.runtime.manifestSha256,
        origin: resolved.origin,
      },
    };
  },
};
