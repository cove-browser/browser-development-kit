import { existsSync } from "node:fs";
import { join } from "node:path";
import { type Manifest, scanFiles } from "@bdk/tooling/dist/runtime/manifest.js";
import { fileDigest } from "../config/digest.js";
import { isRecord } from "../config/reporter.js";
import { configDigest } from "../config/resolve.js";
import { collectFiles } from "../pages/files.js";
import { pagesDirectory } from "../pages/routes.js";
import { builtWithVite, findViteConfig } from "../pages/vite.js";
import type { Project } from "../project.js";
import { embeddedFiles } from "../runtime/project-runtime.js";
import type { Findings } from "./findings.js";

/** How many problems a message lists before it says how many more there are. */
const SHOWN = 5;

function listed(problems: readonly string[]): string {
  const more = problems.length > SHOWN ? `, and ${problems.length - SHOWN} more` : "";
  return `${problems.slice(0, SHOWN).join("; ")}${more}`;
}

/**
 * The `config_digest` the resolver gave the bundle's resolved file. `bdk build automation` adds each
 * script's `bundle` and `bundle_digest` and recomputes the digest, so they are taken out to compare it
 * with `bdk.toml` as it is now.
 */
export function sourceConfigDigest(inBundle: Record<string, unknown>): string {
  const { config_digest: _digest, ...body } = inBundle;
  const automation = isRecord(body.automation) ? body.automation : undefined;
  if (automation === undefined || !isRecord(automation.scripts)) return configDigest(body);
  const scripts = Object.fromEntries(
    Object.entries(automation.scripts).map(([name, entry]) => {
      if (!isRecord(entry)) return [name, entry];
      const { bundle: _bundle, bundle_digest: _bundleDigest, ...source } = entry;
      return [name, source];
    }),
  );
  return configDigest({ ...body, automation: { ...automation, scripts } });
}

/** A digest per file under `dir`, as `bdk build pages` collects them. */
async function pageDigests(dir: string): Promise<Map<string, string>> {
  return new Map(
    (await collectFiles(dir)).map((file) => [file.relative, fileDigest(file.absolute)]),
  );
}

/** What differs between a page's source files and its copy in the bundle. */
export function pageDifferences(
  source: ReadonlyMap<string, string>,
  copy: ReadonlyMap<string, string>,
): string[] {
  const problems: string[] = [];
  for (const [file, digest] of source) {
    const copied = copy.get(file);
    if (copied === undefined) problems.push(`missing: ${file}`);
    else if (copied !== digest) problems.push(`modified: ${file}`);
  }
  for (const file of copy.keys()) {
    if (!source.has(file)) problems.push(`unexpected: ${file}`);
  }
  return problems;
}

/**
 * Pages: every bundled route of the bundle's resolved file is in `Contents/Resources/<path>`. A page that
 * `bdk build pages` copies is compared file by file with its source, when the source is the one the bundle
 * was built from (the route's digest is unchanged). A page Vite builds is not compared: the build records
 * no digest of Vite's output.
 */
export async function pageFindings(
  f: Findings,
  project: Project,
  resources: string,
  inBundle: Record<string, unknown>,
  resolved: Record<string, unknown> | undefined,
): Promise<void> {
  const routes = isRecord(inBundle.routes) ? inBundle.routes : {};
  const current = isRecord(resolved?.routes) ? resolved.routes : {};
  const pagesRoot = join(project.dir, await pagesDirectory(project.configFile));
  const vite = findViteConfig(pagesRoot) !== undefined;
  for (const [route, entry] of Object.entries(routes)) {
    if (!isRecord(entry) || entry.mode !== "bundled" || typeof entry.path !== "string") continue;
    const copy = join(resources, entry.path);
    if (!existsSync(copy)) {
      f.error(
        "doctor.page_missing",
        `The page "${route}" is not in the application (${copy}).`,
        "Build the application again; `bdk build pages` writes the pages into its resources.",
        copy,
      );
      continue;
    }
    const source = join(project.dir, entry.path);
    if (vite && builtWithVite(pagesRoot, source)) {
      f.note(
        "doctor.page_unverified",
        `The page "${route}" is built by Vite, and the build records no digest of its output, so its files are not checked.`,
        copy,
      );
      continue;
    }
    const now = current[route];
    if (!isRecord(now) || now.digest !== entry.digest || !existsSync(source)) continue;
    const problems = pageDifferences(await pageDigests(source), await pageDigests(copy));
    if (problems.length > 0) {
      f.error(
        "doctor.page_modified",
        `The page "${route}" in the application does not match its source: ${listed(problems)}.`,
        "Build the application again, and change pages in the project, not in the built application.",
        copy,
      );
    }
  }
}

/**
 * Automation scripts: each script the bundle's resolved file lists was bundled by `bdk build automation`,
 * and the file at its `bundle` path has its `bundle_digest`.
 */
export function scriptFindings(
  f: Findings,
  resources: string,
  inBundle: Record<string, unknown>,
): void {
  const automation = isRecord(inBundle.automation) ? inBundle.automation : {};
  if (automation.enabled !== true || !isRecord(automation.scripts)) return;
  for (const [name, entry] of Object.entries(automation.scripts)) {
    if (!isRecord(entry)) continue;
    if (typeof entry.bundle !== "string" || typeof entry.bundle_digest !== "string") {
      f.error(
        "doctor.script_not_bundled",
        `The automation script "${name}" was not bundled into the application.`,
        "Build the application again; `bdk build automation` bundles the scripts and records their digests.",
      );
      continue;
    }
    const file = join(resources, entry.bundle);
    if (!existsSync(file)) {
      f.error(
        "doctor.script_missing",
        `The automation script "${name}" is not in the application (${file}).`,
        "Build the application again.",
        file,
      );
    } else if (fileDigest(file) !== entry.bundle_digest) {
      f.error(
        "doctor.script_modified",
        `The automation script "${name}" in the application does not match the digest the build recorded.`,
        "Build the application again, and change scripts in the project, not in the built application.",
        file,
      );
    }
  }
}

/** Files that signing writes or rewrites, so their digests differ from the runtime manifest's. */
function signedPath(path: string): boolean {
  return (
    path.includes("_CodeSignature/") || path.endsWith("/Info.plist") || path.endsWith(".dylib")
  );
}

/**
 * What differs between the runtime manifest's files and their copy in the application. Signing rewrites
 * every executable and library (its signature), the `Info.plist` files (helper and framework identifiers) and adds
 * `_CodeSignature`, so those are compared by kind and executable bit, not content.
 */
export function frameworkDifferences(
  manifest: Readonly<Record<string, string>>,
  copy: Readonly<Record<string, string>>,
): string[] {
  const problems: string[] = [];
  for (const [path, want] of Object.entries(manifest)) {
    const got = copy[path];
    if (got === undefined) {
      problems.push(`missing: ${path}`);
    } else if (want.startsWith("symlink:") || got.startsWith("symlink:")) {
      if (got !== want) problems.push(`relinked: ${path}`);
    } else if (want.endsWith(":exec") !== got.endsWith(":exec")) {
      problems.push(`executable bit differs: ${path}`);
    } else if (got !== want && !want.endsWith(":exec") && !signedPath(path)) {
      problems.push(`modified: ${path}`);
    }
  }
  for (const path of Object.keys(copy)) {
    if (!(path in manifest) && !signedPath(path)) problems.push(`unexpected: ${path}`);
  }
  return problems;
}

/**
 * The runtime in `Contents/Frameworks` against the manifest of the runtime the project resolved: each entry
 * of the runtime's `Chromium.app/Contents/Frameworks` is where `bdk build embed` copies it,
 * `Contents/Frameworks/<entry>`.
 */
export async function frameworkFindings(
  f: Findings,
  appPath: string,
  manifest: Manifest | undefined,
): Promise<void> {
  if (manifest === undefined) return;
  const frameworks = join(appPath, "Contents", "Frameworks");
  const want = embeddedFiles(manifest.files);
  const tops = [...new Set(Object.keys(want).map((path) => path.split("/")[0] ?? path))];
  const embedded = tops.filter((top) => existsSync(join(frameworks, top)));
  if (embedded.length === 0) {
    f.note(
      "doctor.runtime_not_embedded",
      `The application embeds no runtime (${tops.join(", ")} is not in ${frameworks}), so its framework files are not checked.`,
      appPath,
    );
    return;
  }
  const scanned = await scanFiles(frameworks);
  // Only what is under the runtime's own entries; the application may embed other frameworks.
  const copy = Object.fromEntries(
    Object.entries(scanned).filter(([path]) => tops.includes(path.split("/")[0] ?? path)),
  );
  const problems = frameworkDifferences(want, copy);
  if (problems.length > 0) {
    f.error(
      "doctor.framework_modified",
      `The runtime in the application does not match the manifest of runtime ${manifest.source_key}: ${listed(problems)}.`,
      "Build the application again, so `bdk build embed` copies the runtime from the store and `bdk build sign` signs it.",
      frameworks,
    );
  }
}
