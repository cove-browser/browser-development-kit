import { existsSync } from "node:fs";
import { copyFile, mkdtemp, readFile, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { basename, join, resolve } from "node:path";
import { parse } from "smol-toml";
import { normaliseProjectPath } from "../config/paths.js";
import { CliError, ExitCode } from "../exit-codes.js";

/** `[chromium] patches` defaults to this directory when it exists (configuration.md, `[chromium]`). */
export const DEFAULT_PATCHES_DIRECTORY = "Chromium/patches";

/** Where the application's own Chromium patches are (sdk/cli.md section 7). */
export type AppPatches =
  /** A directory with a `series` file, as `bdk patch export` writes it. */
  | { kind: "directory"; dir: string }
  /** An ordered list of patch files, as `[chromium] patches = [...]` gives it. */
  | { kind: "list"; files: string[] }
  /** None configured, and no `Chromium/patches`. */
  | { kind: "none"; dir: string };

export interface SourceSettings {
  mode: "prebuilt" | "source";
  patches: AppPatches;
}

/**
 * `runtime.mode` and `[chromium] patches` from `bdk.toml`. An unreadable file gives the defaults:
 * `bdk build config` and `bdk doctor` report why it is unreadable.
 */
export async function sourceSettings(
  projectDir: string,
  configFile: string,
): Promise<SourceSettings> {
  let doc: Record<string, unknown> = {};
  try {
    doc = parse(await readFile(configFile, "utf8"));
  } catch {
    // Reported by the configuration resolver.
  }
  const runtime = doc.runtime as { mode?: unknown } | undefined;
  const mode = runtime?.mode === "source" ? "source" : "prebuilt";
  const chromium = doc.chromium as { patches?: unknown } | undefined;
  const configured = chromium?.patches;
  if (typeof configured === "string") {
    return {
      mode,
      patches: { kind: "directory", dir: resolve(projectDir, normaliseProjectPath(configured)) },
    };
  }
  if (Array.isArray(configured)) {
    const files = configured
      .filter((entry): entry is string => typeof entry === "string")
      .map((entry) => resolve(projectDir, normaliseProjectPath(entry)));
    return { mode, patches: { kind: "list", files } };
  }
  const dir = join(projectDir, DEFAULT_PATCHES_DIRECTORY);
  return { mode, patches: existsSync(dir) ? { kind: "directory", dir } : { kind: "none", dir } };
}

/**
 * A directory with a `series` the tooling can read: the configured directory itself, or, for the list
 * form, a temporary copy of the listed files with a `series` in their order.
 */
export async function seriesDirectory(patches: AppPatches): Promise<string | undefined> {
  if (patches.kind === "none") return undefined;
  if (patches.kind === "directory") return patches.dir;
  const dir = await mkdtemp(join(tmpdir(), "bdk-app-patches-"));
  const names: string[] = [];
  for (const file of patches.files) {
    const name = basename(file);
    if (names.includes(name)) {
      throw new CliError(
        {
          code: "patch.duplicate_name",
          severity: "error",
          message: `[chromium] patches lists two files named ${name}.`,
          fix: "Give each patch file its own name.",
        },
        ExitCode.ProjectFailure,
      );
    }
    try {
      await copyFile(file, join(dir, name));
    } catch {
      throw new CliError(
        {
          code: "patch.file_missing",
          severity: "error",
          message: `[chromium] patches lists ${file}, which does not exist.`,
          file,
          fix: "Fix the path in bdk.toml, or remove the entry.",
        },
        ExitCode.ProjectFailure,
      );
    }
    names.push(name);
  }
  await writeFile(join(dir, "series"), names.map((name) => `${name}\n`).join(""));
  return dir;
}
