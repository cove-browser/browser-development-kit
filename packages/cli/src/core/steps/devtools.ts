import { existsSync, readdirSync, readFileSync, statSync } from "node:fs";
import { cp, mkdir, readFile, rm, writeFile } from "node:fs/promises";
import { join, resolve } from "node:path";
import { directoryDigest, sha256 } from "../config/digest.js";
import { isInsideProject } from "../config/pages.js";
import { pathProblem } from "../config/paths.js";
import type { Diagnostic } from "../diagnostics.js";
import { hasErrors } from "../diagnostics-format.js";
import { CliError, ExitCode, usageError } from "../exit-codes.js";
import { RESOLVED_FILE } from "./config.js";
import type { Step, StepResult } from "./step.js";

/** Inside the application bundle's `Contents/Resources`. */
const BUNDLE_DIRECTORY = "BDK/DevTools";
const FRONTEND = "Frontend";
const OVERLAY = "Overlay";
const OVERLAY_FILES = ["overlay.css", "overlay.js"] as const;
const IGNORED = ".DS_Store";
/** A scheme (two or more characters, so a drive letter is not one) or a protocol-relative `//host`. */
const URL_LIKE = /^([a-z][a-z0-9+.-]+:|\/\/)/i;

interface Cache {
  /** Digest of the configured mode and of every input file. */
  inputs: string;
  /** Absolute path of the packaged directory. */
  output: string;
  /** Digest of the packaged tree as written. */
  output_digest: string;
}

function cacheFile(projectDir: string): string {
  return join(projectDir, ".bdk", "devtools-cache.json");
}

function readCache(projectDir: string): Cache | undefined {
  try {
    const parsed: unknown = JSON.parse(readFileSync(cacheFile(projectDir), "utf8"));
    return typeof parsed === "object" && parsed !== null ? (parsed as Cache) : undefined;
  } catch {
    return undefined;
  }
}

/** What the engine will use: a replacement front end, an overlay on the default one, or neither. */
type Plan =
  | { kind: "replaced"; source: string; sourceKey: string }
  | { kind: "overlay"; source: string; sourceKey: string; files: string[]; extra: string[] }
  | { kind: "none"; reason: string };

/**
 * `bdk build devtools`: package the DevTools front end or overlay the resolved configuration names
 * inside the application bundle (javascript-api.md section 6, configuration.md section 3.12). Run
 * `bdk build config` first. Front-end resources are packaged in the signed bundle, never loaded
 * from a URL.
 */
export const devtoolsStep: Step = {
  name: "devtools",
  flags: ["bundle", "out"],
  async run({ project, ctx, args }): Promise<StepResult> {
    const resolvedFile =
      args.out === undefined ? resolve(project.dir, RESOLVED_FILE) : resolve(ctx.cwd, args.out);
    const resolved = await readResolved(resolvedFile);
    const devtools = asRecord(resolved.devtools);

    const bundle = args.bundle ?? ctx.env.CODESIGNING_FOLDER_PATH;
    if (bundle === undefined || bundle === "") {
      throw usageError(
        "`bdk build devtools` needs the application bundle.",
        "Pass --bundle <App.app>. Xcode build phases set CODESIGNING_FOLDER_PATH.",
      );
    }
    const outDir = join(resolve(ctx.cwd, bundle), "Contents", "Resources", BUNDLE_DIRECTORY);

    const diagnostics: Diagnostic[] = [];
    const plan = planFor(project.dir, devtools, diagnostics);
    const mode = modeOf(devtools);
    if (hasErrors(diagnostics) || plan === undefined) {
      return { diagnostics, exitCode: ExitCode.ProjectFailure, data: { mode, packaged: false } };
    }

    const inputs = sha256(JSON.stringify([plan.kind, planDigests(plan)]));
    const cached = readCache(project.dir);
    const unchanged =
      cached !== undefined &&
      cached.inputs === inputs &&
      cached.output === outDir &&
      (plan.kind === "none"
        ? !existsSync(outDir)
        : existsSync(outDir) && directoryDigest(outDir) === cached.output_digest);

    if (!unchanged) await apply(plan, outDir);

    if (plan.kind === "none") {
      await writeCache(project.dir, undefined);
      if (!args.json) {
        ctx.stdout(`bdk build devtools: nothing packaged, ${plan.reason}.\n`);
      }
      return {
        diagnostics,
        exitCode: ExitCode.Success,
        data: { mode, packaged: false, unchanged, skipped: plan.reason },
      };
    }

    const files = listTree(outDir);
    await writeCache(project.dir, {
      inputs,
      output: outDir,
      output_digest: directoryDigest(outDir),
    });
    if (!args.json) {
      ctx.stdout(
        unchanged
          ? `bdk build devtools: ${mode} unchanged, ${files.length} file(s).\n`
          : `bdk build devtools: packaged ${mode}, ${files.length} file(s).\n`,
      );
    }
    return {
      diagnostics,
      exitCode: ExitCode.Success,
      data: { mode, packaged: true, unchanged, files },
    };
  },
};

function modeOf(devtools: Record<string, unknown>): string {
  const frontend = devtools.frontend ?? "default";
  if (frontend === "disabled" || frontend === "default") {
    return frontend === "default" && devtools.overlay !== undefined ? "overlay" : String(frontend);
  }
  return "replacement";
}

/** Validate the configured values and decide what to package; errors and warnings go to `out`. */
function planFor(
  projectDir: string,
  devtools: Record<string, unknown>,
  out: Diagnostic[],
): Plan | undefined {
  const frontend = devtools.frontend ?? "default";
  const overlay = devtools.overlay;
  for (const [key, value] of [
    ["frontend", frontend],
    ["overlay", overlay],
  ] as const) {
    if (typeof value === "string" && URL_LIKE.test(value)) {
      out.push({
        code: "devtools.url_not_allowed",
        severity: "error",
        message: `"devtools.${key}" is a URL: "${value}". DevTools resources are packaged in the signed bundle, never loaded from a URL.`,
        file: "bdk.toml",
        fix: "Point it at a directory inside the project.",
      });
    }
  }
  if (hasErrors(out)) return undefined;

  if (frontend !== "default" && frontend !== "disabled") {
    if (typeof frontend !== "string") return undefined;
    const source = directoryOf(projectDir, "frontend", frontend, out);
    if (source === undefined) return undefined;
    if (!existsSync(join(source, "index.html"))) {
      out.push({
        code: "devtools.entry_missing",
        severity: "error",
        message: `The DevTools front end "${frontend}" has no index.html.`,
        file: frontend,
        fix: "Add an index.html; it is the entry the engine serves in place of the default front end.",
      });
      return undefined;
    }
    if (overlay !== undefined) {
      out.push({
        code: "devtools.overlay_ignored",
        severity: "warning",
        message: `"devtools.overlay" is ignored because "devtools.frontend" replaces the front end.`,
        file: "bdk.toml",
        fix: 'Remove the overlay, or set frontend to "default".',
      });
    }
    return { kind: "replaced", source, sourceKey: directoryDigest(source) };
  }

  if (frontend === "default" && typeof overlay === "string") {
    const source = directoryOf(projectDir, "overlay", overlay, out);
    if (source === undefined) return undefined;
    const names = readdirSync(source).filter((name) => name !== IGNORED);
    const files = OVERLAY_FILES.filter((name) => names.includes(name));
    if (files.length === 0) {
      out.push({
        code: "devtools.overlay_empty",
        severity: "error",
        message: `The DevTools overlay "${overlay}" has neither overlay.css nor overlay.js.`,
        file: overlay,
        fix: "Add overlay.css, overlay.js or both.",
      });
      return undefined;
    }
    const extra = names.filter((name) => !(OVERLAY_FILES as readonly string[]).includes(name));
    if (extra.length > 0) {
      out.push({
        code: "devtools.overlay_files_ignored",
        severity: "warning",
        message: `Only overlay.css and overlay.js are packaged; not copied from "${overlay}": ${extra.join(", ")}.`,
        file: overlay,
        fix: "Import what the overlay needs from overlay.js, or inline it.",
      });
    }
    return { kind: "overlay", source, sourceKey: "", files, extra };
  }

  if (frontend === "disabled" && overlay !== undefined) {
    out.push({
      code: "devtools.overlay_ignored",
      severity: "warning",
      message: `"devtools.overlay" is ignored because "devtools.frontend" is "disabled".`,
      file: "bdk.toml",
      fix: 'Remove the overlay, or set frontend to "default".',
    });
  }
  return {
    kind: "none",
    reason:
      frontend === "disabled" ? 'devtools.frontend is "disabled"' : "the default front end is used",
  };
}

/** The absolute directory for a configured path, or `undefined` after reporting why it is not usable. */
function directoryOf(
  projectDir: string,
  key: string,
  value: string,
  out: Diagnostic[],
): string | undefined {
  const pathEscape = (message: string): undefined => {
    out.push({
      code: "config.path_escape",
      severity: "error",
      message,
      file: "bdk.toml",
      fix: "Use a directory inside the project.",
    });
    return undefined;
  };
  const problem = pathProblem(value);
  if (problem)
    return pathEscape(`"devtools.${key}" is not a path inside the project: "${value}". ${problem}`);
  const absolute = resolve(projectDir, value);
  if (!existsSync(absolute) || !statSync(absolute).isDirectory()) {
    out.push({
      code: "devtools.not_directory",
      severity: "error",
      message: `"devtools.${key}" is "${value}", which is not a directory.`,
      file: "bdk.toml",
      fix: "Point it at a directory.",
    });
    return undefined;
  }
  if (!isInsideProject(projectDir, absolute)) {
    return pathEscape(`"devtools.${key}" resolves to a place outside the project: "${value}".`);
  }
  return absolute;
}

function planDigests(plan: Plan): string {
  if (plan.kind === "replaced") return plan.sourceKey;
  if (plan.kind === "none") return plan.reason;
  return JSON.stringify(
    plan.files.map((name) => [name, sha256(readFileSync(join(plan.source, name)))]),
  );
}

/** Make `outDir` hold exactly what the plan says. */
async function apply(plan: Plan, outDir: string): Promise<void> {
  if (plan.kind === "none") {
    await rm(outDir, { recursive: true, force: true });
    return;
  }
  await rm(outDir, { recursive: true, force: true });
  if (plan.kind === "replaced") {
    await cp(plan.source, join(outDir, FRONTEND), {
      recursive: true,
      filter: (source) => !source.endsWith(`/${IGNORED}`),
    });
    return;
  }
  await mkdir(join(outDir, OVERLAY), { recursive: true });
  for (const name of plan.files) {
    await cp(join(plan.source, name), join(outDir, OVERLAY, name));
  }
}

/** Every file under `root`, relative with forward slashes, sorted. */
function listTree(root: string, relative = ""): string[] {
  const out: string[] = [];
  for (const name of readdirSync(join(root, relative)).sort()) {
    const child = relative === "" ? name : `${relative}/${name}`;
    if (statSync(join(root, child)).isDirectory()) out.push(...listTree(root, child));
    else out.push(child);
  }
  return out;
}

async function writeCache(projectDir: string, cache: Cache | undefined): Promise<void> {
  const file = cacheFile(projectDir);
  if (cache === undefined) {
    await rm(file, { force: true });
    return;
  }
  await mkdir(join(file, ".."), { recursive: true });
  await writeFile(file, `${JSON.stringify(cache, null, 2)}\n`);
}

async function readResolved(file: string): Promise<Record<string, unknown>> {
  const fail = (message: string): CliError =>
    new CliError(
      {
        code: "devtools.resolved_missing",
        severity: "error",
        message,
        file,
        fix: "Run `bdk build config` first; it writes bdk.resolved.json.",
      },
      ExitCode.ProjectFailure,
    );
  let text: string;
  try {
    text = await readFile(file, "utf8");
  } catch {
    throw fail(`There is no ${RESOLVED_FILE} to read the DevTools settings from.`);
  }
  try {
    return asRecord(JSON.parse(text));
  } catch {
    throw fail(`${RESOLVED_FILE} is not valid JSON.`);
  }
}

function asRecord(value: unknown): Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value)
    ? (value as Record<string, unknown>)
    : {};
}
