import { existsSync } from "node:fs";
import { chmod, cp, lstat, mkdir, readdir, rm } from "node:fs/promises";
import { join, resolve } from "node:path";
import { BdkHomeError, currentBdkPaths } from "@bdk/tooling/dist/paths.js";
import { scanFiles, verifyRuntimeDir } from "@bdk/tooling/dist/runtime/manifest.js";
import { frameworkDifferences } from "../doctor/contents.js";
import { CliError, ExitCode, type ExitCodeValue, usageError } from "../exit-codes.js";
import {
  EMBEDDED_DIR,
  embeddedFiles,
  RUNTIME_POINTER,
  readRuntimePointer,
} from "../runtime/project-runtime.js";
import { runtimeStep } from "./runtime.js";
import type { Step, StepResult } from "./step.js";

function fail(code: string, message: string, fix: string, exitCode: ExitCodeValue): CliError {
  return new CliError({ code, severity: "error", message, fix }, exitCode);
}

/** Makes a copied tree writable by its owner: the store is read-only, and signing rewrites the copy. */
async function makeWritable(path: string): Promise<void> {
  const info = await lstat(path);
  if (info.isSymbolicLink()) return;
  await chmod(path, info.mode | 0o200);
  if (info.isDirectory()) {
    for (const name of await readdir(path)) await makeWritable(join(path, name));
  }
}

/** What the bundle holds of the runtime's entries, as `scanFiles` describes files. */
async function copiedFiles(frameworks: string, tops: readonly string[]) {
  if (!existsSync(frameworks)) return {};
  const scanned = await scanFiles(frameworks);
  return Object.fromEntries(
    Object.entries(scanned).filter(([path]) => tops.includes(path.split("/")[0] ?? path)),
  );
}

/**
 * `bdk build embed`: copy the frameworks of the runtime the project resolved (`.bdk/runtime.json`) into the
 * application's `Contents/Frameworks`, symlinks as they are, and check the copy against the runtime's
 * manifest (sdk/cli.md section 5). Incremental: a copy that already matches, signed or not, is left.
 */
export const embedStep: Step = {
  name: "embed",
  flags: ["bundle"],
  // With a bundle to write into and a runtime to embed: a release with no runtime builds for the stub.
  applies: (input) =>
    (input.args.bundle ?? input.ctx.env.CODESIGNING_FOLDER_PATH) !== undefined &&
    (runtimeStep.applies?.(input) ?? true),
  async run({ project, ctx, args }): Promise<StepResult> {
    const bundle = args.bundle ?? ctx.env.CODESIGNING_FOLDER_PATH;
    if (bundle === undefined || bundle === "") {
      throw usageError(
        "`bdk build embed` needs the application bundle.",
        "Pass --bundle <App.app>. Xcode build phases set CODESIGNING_FOLDER_PATH.",
      );
    }
    const pointer = await readRuntimePointer(project.dir);
    if (pointer === undefined) {
      throw fail(
        "embed.no_runtime",
        `The project has resolved no runtime (${RUNTIME_POINTER}).`,
        "Run `bdk build runtime` first; `bdk build` runs it before embed.",
        ExitCode.ProjectFailure,
      );
    }
    let dir: string;
    try {
      dir = currentBdkPaths({ env: ctx.env }).runtime(pointer.source_key);
    } catch (error) {
      if (!(error instanceof BdkHomeError)) throw error;
      throw fail(
        "embed.bdk_home_invalid",
        error.message,
        "Set BDK_HOME to an absolute path with no spaces.",
        ExitCode.Environment,
      );
    }
    if (!existsSync(dir)) {
      throw fail(
        "embed.runtime_missing",
        `The project resolved runtime ${pointer.source_key}, which is not in the store (${dir}).`,
        "Run `bdk build runtime` again; it downloads or rebuilds it into the store.",
        ExitCode.ProjectFailure,
      );
    }
    let runtime: Awaited<ReturnType<typeof verifyRuntimeDir>>;
    try {
      runtime = await verifyRuntimeDir(dir, pointer.source_key);
    } catch (error) {
      throw fail(
        "embed.runtime_modified",
        `The runtime in the store does not match its manifest: ${(error as Error).message}`,
        `Delete it (chmod -R u+w "${dir}" && rm -rf "${dir}") and run \`bdk build runtime\` again.`,
        ExitCode.Integrity,
      );
    }
    if (runtime.manifestSha256 !== pointer.manifest_sha256) {
      throw fail(
        "embed.runtime_changed",
        `The runtime ${pointer.source_key} in the store is not the one the project resolved (manifest ${runtime.manifestSha256}, recorded ${pointer.manifest_sha256}).`,
        "Run `bdk build runtime` again.",
        ExitCode.Integrity,
      );
    }
    const want = embeddedFiles(runtime.manifest.files);
    const tops = [...new Set(Object.keys(want).map((path) => path.split("/")[0] ?? path))].sort();
    const appPath = resolve(ctx.cwd, bundle);
    const frameworks = join(appPath, "Contents", "Frameworks");

    // A copy that matches the manifest (allowing for what signing rewrites) is this runtime already.
    if (frameworkDifferences(want, await copiedFiles(frameworks, tops)).length === 0) {
      return {
        diagnostics: [],
        exitCode: ExitCode.Success,
        data: { sourceKey: pointer.source_key, frameworks: tops, unchanged: true },
      };
    }
    await mkdir(frameworks, { recursive: true });
    for (const top of tops) {
      const target = join(frameworks, top);
      await rm(target, { recursive: true, force: true });
      await cp(join(dir, EMBEDDED_DIR, top), target, { recursive: true, verbatimSymlinks: true });
      await makeWritable(target);
    }
    // Before signing, the copy is exactly the runtime's files.
    const copy = await copiedFiles(frameworks, tops);
    const differing = Object.keys(want).filter((path) => copy[path] !== want[path]);
    const extra = Object.keys(copy).filter((path) => !(path in want));
    if (differing.length > 0 || extra.length > 0) {
      throw fail(
        "embed.copy_mismatch",
        `The copy in ${frameworks} does not match runtime ${pointer.source_key}: ${[
          ...differing,
          ...extra,
        ]
          .slice(0, 5)
          .join(", ")}.`,
        "Check the disk and the bundle's permissions, then build again.",
        ExitCode.Integrity,
      );
    }
    return {
      diagnostics: [],
      exitCode: ExitCode.Success,
      data: { sourceKey: pointer.source_key, frameworks: tops, unchanged: false },
    };
  },
};
