import { existsSync, lstatSync, readdirSync, readFileSync, realpathSync, statSync } from "node:fs";
import { cp, mkdir, readdir, readFile, rm, rmdir, writeFile } from "node:fs/promises";
import { dirname, join, relative, resolve } from "node:path";
import { directoryDigest } from "../config/digest.js";
import { isInsideProject } from "../config/pages.js";
import { pathProblem } from "../config/paths.js";
import type { Diagnostic } from "../diagnostics.js";
import { hasErrors } from "../diagnostics-format.js";
import { CliError, ExitCode, usageError } from "../exit-codes.js";
import { RESOLVED_FILE } from "./config.js";
import type { Step, StepResult } from "./step.js";

const IGNORED = ".DS_Store";
/** The manifest keys whose values the extension is granted, or may ask for, in the order they are printed. */
const PERMISSION_KEYS = [
  "permissions",
  "optional_permissions",
  "host_permissions",
  "optional_host_permissions",
] as const;

type Permissions = Record<(typeof PERMISSION_KEYS)[number], string[]>;

interface Entry {
  name: string;
  path: string;
  required: boolean;
  digest: string;
  source: string;
}

/**
 * The copies this step wrote, per application bundle: `Contents/Resources` → project-relative path → the
 * digest written. A copy it finds there with that digest is its own, so it may remove it.
 */
/** One bundled extension as the step reports it. */
interface Copied {
  name: string;
  path: string;
  required: boolean;
  unchanged: boolean;
  permissions: Permissions;
}

interface Cache {
  bundles: Record<string, Record<string, string>>;
}

function cacheFile(projectDir: string): string {
  return join(projectDir, ".bdk", "extensions-cache.json");
}

function readCache(projectDir: string): Cache {
  try {
    const parsed: unknown = JSON.parse(readFileSync(cacheFile(projectDir), "utf8"));
    const bundles = asRecord(asRecord(parsed).bundles);
    return {
      bundles: Object.fromEntries(
        Object.entries(bundles).map(([output, copies]) => [
          output,
          Object.fromEntries(
            Object.entries(asRecord(copies)).filter(([, digest]) => typeof digest === "string"),
          ) as Record<string, string>,
        ]),
      ),
    };
  } catch {
    return { bundles: {} };
  }
}

async function writeCache(projectDir: string, cache: Cache): Promise<void> {
  const file = cacheFile(projectDir);
  await mkdir(dirname(file), { recursive: true });
  await writeFile(file, `${JSON.stringify(cache, null, 2)}\n`);
}

/**
 * `bdk build extensions`: copy each `[extensions.bundled]` directory the resolved configuration lists
 * into the application bundle at `Contents/Resources/<path>`, where the runtime loads it, so the
 * application's signature covers it (extensions.md section 9, configuration.md section 3.7). Run
 * `bdk build config` first: each copy must match the digest it recorded. Prints each extension's
 * declared permissions, so a reviewer sees what the product grants itself.
 */
export const extensionsStep: Step = {
  name: "extensions",
  flags: ["bundle", "out"],
  // Without a bundle there is nothing to write into: a plain `bdk build` leaves the step out.
  applies: ({ ctx, args }) => (args.bundle ?? ctx.env.CODESIGNING_FOLDER_PATH) !== undefined,
  async run({ project, ctx, args }): Promise<StepResult> {
    const resolvedFile =
      args.out === undefined ? resolve(project.dir, RESOLVED_FILE) : resolve(ctx.cwd, args.out);
    const resolved = await readResolved(resolvedFile);
    const bundled = asRecord(asRecord(resolved.extensions).bundled);

    const bundle = args.bundle ?? ctx.env.CODESIGNING_FOLDER_PATH;
    if (bundle === undefined || bundle === "") {
      throw usageError(
        "`bdk build extensions` needs the application bundle.",
        "Pass --bundle <App.app>. Xcode build phases set CODESIGNING_FOLDER_PATH.",
      );
    }
    const resources = join(resolve(ctx.cwd, bundle), "Contents", "Resources");

    const diagnostics: Diagnostic[] = [];
    const entries: Entry[] = [];
    for (const [name, value] of Object.entries(bundled)) {
      const entry = checkEntry(project.dir, resolvedFile, name, asRecord(value), diagnostics);
      if (entry !== undefined) entries.push(entry);
    }
    if (hasErrors(diagnostics)) {
      return { diagnostics, exitCode: ExitCode.ProjectFailure, data: { copied: false } };
    }

    const cache = readCache(project.dir);
    const owned = cache.bundles[resources] ?? {};
    const copies: Record<string, string> = {};
    const removed: string[] = [];
    const kept: string[] = [];
    const listed = new Set(entries.map((e) => e.path));
    for (const [path, digest] of Object.entries(owned)) {
      if (listed.has(path)) continue;
      const target = join(resources, path);
      if (!existsSync(target)) continue;
      if (isDirectory(target) && directoryDigest(target) === digest) {
        await rm(target, { recursive: true, force: true });
        await removeEmptyParents(dirname(target), resources);
        removed.push(path);
      } else {
        kept.push(path);
        diagnostics.push({
          code: "extensions.stale_copy_kept",
          severity: "warning",
          message: `Contents/Resources/${path} is no longer a bundled extension, but it changed since this step copied it, so it is left in the application.`,
          file: target,
          fix: "Remove it from the application if nothing else needs it, or clean the build.",
        });
      }
    }

    const built: Copied[] = [];
    for (const entry of entries) {
      const target = join(resources, entry.path);
      const unchanged = isDirectory(target) && directoryDigest(target) === entry.digest;
      if (!unchanged) {
        await rm(target, { recursive: true, force: true });
        await cp(entry.source, target, {
          recursive: true,
          dereference: true,
          filter: (source) => !source.endsWith(`/${IGNORED}`),
        });
        const written = directoryDigest(target);
        if (written !== entry.digest) {
          await rm(target, { recursive: true, force: true });
          diagnostics.push({
            code: "extensions.digest_mismatch",
            severity: "error",
            message: `The copy of the bundled extension "${entry.name}" has digest ${written}, not the ${entry.digest} in ${RESOLVED_FILE}; it was removed.`,
            file: entry.source,
            fix: "Leave the extension's directory unchanged while the build runs, then build again.",
          });
          await writeCache(project.dir, { bundles: { ...cache.bundles, [resources]: copies } });
          return { diagnostics, exitCode: ExitCode.Integrity, data: { copied: false } };
        }
      }
      copies[entry.path] = entry.digest;
      built.push({
        name: entry.name,
        path: entry.path,
        required: entry.required,
        unchanged,
        permissions: declaredPermissions(entry.source),
      });
    }
    await writeCache(project.dir, { bundles: { ...cache.bundles, [resources]: copies } });

    if (!args.json) ctx.stdout(summary(built, removed));
    return {
      diagnostics,
      exitCode: ExitCode.Success,
      data: { copied: true, bundle: resolve(ctx.cwd, bundle), extensions: built, removed, kept },
    };
  },
};

/** The entry with its source directory, or `undefined` after reporting why it cannot be copied. */
function checkEntry(
  projectDir: string,
  resolvedFile: string,
  name: string,
  value: Record<string, unknown>,
  out: Diagnostic[],
): Entry | undefined {
  const path = value.path;
  const digest = value.digest;
  if (typeof path !== "string" || typeof digest !== "string") {
    out.push({
      code: "extensions.resolved_invalid",
      severity: "error",
      message: `The bundled extension "${name}" in ${RESOLVED_FILE} has no path or digest.`,
      file: resolvedFile,
      fix: "Run `bdk build config` again; it writes both.",
    });
    return undefined;
  }
  const pathEscape = (message: string): undefined => {
    out.push({
      code: "config.path_escape",
      severity: "error",
      message,
      file: "bdk.toml",
      fix: "Remove the link, or copy the extension into the project.",
    });
    return undefined;
  };
  const problem = pathProblem(path);
  if (problem !== undefined || path === "") {
    return pathEscape(
      `"extensions.bundled.${name}" is not a path inside the project: "${path}". ${problem ?? "It names the project directory itself."}`,
    );
  }
  const source = resolve(projectDir, path);
  if (!isDirectory(source)) {
    out.push({
      code: "extensions.bundled_missing",
      severity: "error",
      message: `The bundled extension "${name}" at "${path}" is not a directory.`,
      file: "bdk.toml",
      fix: "Point it at the extension's directory, then run `bdk build config` again.",
    });
    return undefined;
  }
  if (!isInsideProject(projectDir, source)) {
    return pathEscape(`"extensions.bundled.${name}" resolves to a place outside the project.`);
  }
  const link = linkProblem(source);
  if (link !== undefined) {
    if (link.kind === "outside") {
      return pathEscape(
        `"${path}/${link.path}" in the bundled extension "${name}" is a link that leaves the extension's directory.`,
      );
    }
    out.push({
      code: "extensions.bad_link",
      severity: "error",
      message:
        link.kind === "broken"
          ? `"${path}/${link.path}" in the bundled extension "${name}" is a link to nothing.`
          : `"${path}/${link.path}" in the bundled extension "${name}" links back to a directory it is inside.`,
      file: join(source, link.path),
      fix: "Remove the link, or replace it with the files it should contain.",
    });
    return undefined;
  }
  if (directoryDigest(source) !== digest) {
    out.push({
      code: "extensions.resolved_stale",
      severity: "error",
      message: `The bundled extension "${name}" at "${path}" changed since \`bdk build config\` recorded its digest, so ${RESOLVED_FILE} is stale.`,
      file: source,
      fix: "Run `bdk build config` again, then this step.",
    });
    return undefined;
  }
  return { name, path, required: value.required === true, digest, source };
}

type LinkProblem = { path: string; kind: "outside" | "broken" | "loop" };

/**
 * The first link under `root` the copy cannot carry: one that leaves `root`, points nowhere, or leads
 * back to a directory it is already inside. Links are copied as what they point at.
 */
function linkProblem(root: string): LinkProblem | undefined {
  const walk = (directory: string, ancestors: Set<string>): LinkProblem | undefined => {
    for (const name of readdirSync(directory).sort()) {
      const absolute = join(directory, name);
      const child = relative(root, absolute);
      if (lstatSync(absolute).isSymbolicLink()) {
        if (!existsSync(absolute)) return { path: child, kind: "broken" };
        if (!isInsideProject(root, absolute)) return { path: child, kind: "outside" };
      }
      if (!isDirectory(absolute)) continue;
      const real = realpathSync(absolute);
      if (ancestors.has(real)) return { path: child, kind: "loop" };
      const found = walk(absolute, new Set([...ancestors, real]));
      if (found) return found;
    }
    return undefined;
  };
  return walk(root, new Set([realpathSync(root)]));
}

/** What the manifest declares under each permission key; a key it does not have is an empty list. */
function declaredPermissions(source: string): Permissions {
  let manifest: Record<string, unknown> = {};
  try {
    manifest = asRecord(JSON.parse(readFileSync(join(source, "manifest.json"), "utf8")));
  } catch {
    // The resolver checked the manifest and the digest says it has not changed since.
  }
  return Object.fromEntries(
    PERMISSION_KEYS.map((key) => {
      const value = manifest[key];
      return [
        key,
        Array.isArray(value) ? value.filter((v): v is string => typeof v === "string") : [],
      ];
    }),
  ) as Permissions;
}

function summary(built: readonly Copied[], removed: readonly string[]): string {
  if (built.length === 0 && removed.length === 0) {
    return "bdk build extensions: nothing to do, there are no bundled extensions.\n";
  }
  const copied = built.filter((b) => !b.unchanged).length;
  const lines = [
    `bdk build extensions: copied ${copied} bundled extension(s), ${built.length - copied} unchanged${
      removed.length === 0 ? "" : `, removed ${removed.length} no longer listed`
    }.`,
  ];
  for (const b of built) {
    const granted = PERMISSION_KEYS.filter((key) => b.permissions[key].length > 0).map(
      (key) => `${key.replaceAll("_", " ")}: ${b.permissions[key].join(", ")}`,
    );
    lines.push(
      `  ${b.name} (${b.path}${b.required ? ", required" : ""}): ${
        granted.length === 0 ? "declares no permissions" : granted.join("; ")
      }`,
    );
  }
  return `${lines.join("\n")}\n`;
}

/** Removes each empty directory from `directory` up to, not including, `stop`. */
async function removeEmptyParents(directory: string, stop: string): Promise<void> {
  let current = directory;
  while (current !== stop && current.startsWith(stop)) {
    if ((await readdir(current)).length > 0) return;
    await rmdir(current);
    current = dirname(current);
  }
}

function isDirectory(path: string): boolean {
  return existsSync(path) && statSync(path).isDirectory();
}

async function readResolved(file: string): Promise<Record<string, unknown>> {
  const fail = (message: string): CliError =>
    new CliError(
      {
        code: "extensions.resolved_missing",
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
    throw fail(`There is no ${RESOLVED_FILE} to read the bundled extensions from.`);
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
