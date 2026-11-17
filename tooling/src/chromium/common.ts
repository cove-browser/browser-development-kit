import { existsSync, readdirSync, readFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { type CheckoutId, checkoutRoot, LockBusyError, runUnderCheckoutLock } from "../lock.js";
import { type BdkPaths, currentBdkPaths } from "../paths.js";

/** The pin record, `chromium/pin.json`. Other fields (runtime, ABI) belong to other commands. */
export interface Pin {
  readonly tag: string;
  readonly commit: string;
}

export const DEFAULT_CHROMIUM_URL = "https://chromium.googlesource.com/chromium/src.git";
export const DEFAULT_DEPOT_TOOLS_URL =
  "https://chromium.googlesource.com/chromium/tools/depot_tools.git";
/** The GitHub mirror serves blobless sparse fetches far faster than googlesource. */
export const DEFAULT_PATCH_CHECK_REMOTE = "https://github.com/chromium/chromium.git";
/** The symbol `//bdk` exports; `chromium:build` checks the framework for it. */
export const OVERLAY_MARKER_SYMBOL = "_bdk_overlay_marker";

/**
 * The BDK workspace root: `tooling/src/chromium/common.ts` is three levels below it.
 * `BDK_REPO_ROOT` overrides it (tests point it at a fixture tree).
 */
export function repoRoot(): string {
  if (process.env.BDK_REPO_ROOT) return resolve(process.env.BDK_REPO_ROOT);
  return resolve(dirname(fileURLToPath(import.meta.url)), "..", "..", "..");
}

export interface ChromiumLayout {
  readonly repoRoot: string;
  readonly pinFile: string;
  readonly patchesDir: string;
  readonly seriesFile: string;
  readonly overlayDir: string;
  readonly gnDir: string;
  readonly gclientDir: string;
}

export function chromiumLayout(root: string = repoRoot()): ChromiumLayout {
  const c = join(root, "chromium");
  return {
    repoRoot: root,
    pinFile: join(c, "pin.json"),
    patchesDir: join(c, "patches"),
    seriesFile: join(c, "patches", "series"),
    overlayDir: join(c, "bdk"),
    gnDir: join(c, "gn"),
    gclientDir: join(c, "gclient"),
  };
}

export function readPin(layout: ChromiumLayout = chromiumLayout()): Pin {
  const raw = JSON.parse(readFileSync(layout.pinFile, "utf8")) as Partial<Pin>;
  if (typeof raw.tag !== "string" || !/^[0-9a-f]{40}$/.test(String(raw.commit))) {
    throw new Error(`${layout.pinFile} must have a "tag" and a 40-hex "commit".`);
  }
  return { tag: raw.tag, commit: raw.commit as string };
}

// ---------------------------------------------------------------------------------------------
// Patch files

export class PatchFormatError extends Error {
  override readonly name = "PatchFormatError";
}

export interface PatchFile {
  /** File name, as listed in `series`. */
  readonly name: string;
  readonly path: string;
  readonly text: string;
  readonly subject: string;
  /** Commit message body: why the hook is needed and which BDK component uses it. */
  readonly body: string;
  /** Every upstream path the patch touches (both sides of a rename). Sorted. */
  readonly files: readonly string[];
}

/** Lines of a series file: blank lines and `#` comments are ignored. */
export function parseSeries(text: string): string[] {
  return text
    .split(/\r?\n/)
    .map((l) => l.trim())
    .filter((l) => l !== "" && !l.startsWith("#"));
}

/** The files a unified diff touches: `diff --git` headers plus rename/copy lines. */
export function patchTouchedFiles(text: string): string[] {
  const files = new Set<string>();
  for (const line of text.split("\n")) {
    if (line.startsWith("diff --git ")) {
      const rest = line.slice("diff --git ".length);
      if (rest.startsWith('"')) {
        throw new PatchFormatError(
          `Quoted path in "${line}" is not supported; avoid unusual file names.`,
        );
      }
      // "a/P b/P" for everything except renames, where rename lines follow. The two halves of a
      // non-rename are equal, which gives an unambiguous split even if the path has spaces.
      const half = (rest.length - 5) / 2;
      if (Number.isInteger(half) && half > 0 && rest.slice(2, 2 + half) === rest.slice(5 + half)) {
        files.add(rest.slice(2, 2 + half));
      } else {
        const m = /^a\/(\S+) b\/(\S+)$/.exec(rest);
        if (m) {
          files.add(m[1] as string);
          files.add(m[2] as string);
        }
      }
    } else {
      const m = /^(?:rename|copy) (?:from|to) (.+)$/.exec(line);
      if (m) files.add(m[1] as string);
    }
  }
  return [...files].sort();
}

/** Splits a `git format-patch` mail into its subject and the commit-message body. */
export function parsePatchMessage(text: string): { subject: string; body: string } {
  if (!text.startsWith("From ")) {
    throw new PatchFormatError(
      'not in "git format-patch" format (no leading "From <sha> ..." line)',
    );
  }
  const lines = text.split("\n");
  let i = 0;
  let subject = "";
  for (; i < lines.length && lines[i] !== ""; i++) {
    const line = lines[i] as string;
    if (line.startsWith("Subject:")) {
      subject = line.slice("Subject:".length).trim();
      while (/^[ \t]/.test(lines[i + 1] ?? "")) subject += ` ${(lines[++i] as string).trim()}`;
    }
  }
  if (subject === "") throw new PatchFormatError("no Subject header");
  const bodyLines: string[] = [];
  for (i += 1; i < lines.length; i++) {
    const line = lines[i] as string;
    if (line === "---" || line.startsWith("diff --git ")) break;
    bodyLines.push(line);
  }
  return { subject: subject.replace(/^\[PATCH[^\]]*\]\s*/, ""), body: bodyLines.join("\n").trim() };
}

export const EMPTY_BODY_HELP =
  "a patch must carry a commit message that says why the hook is needed and which BDK component uses it";

export function parsePatch(name: string, path: string, text: string): PatchFile {
  let message: { subject: string; body: string };
  try {
    message = parsePatchMessage(text);
  } catch (e) {
    throw new PatchFormatError(`${name}: ${(e as Error).message}`);
  }
  if (message.body === "") {
    throw new PatchFormatError(`${name}: empty commit message body; ${EMPTY_BODY_HELP}.`);
  }
  return { name, path, text, ...message, files: patchTouchedFiles(text) };
}

export interface Series {
  readonly patches: readonly PatchFile[];
  /** `*.patch` files in the directory that `series` does not list. */
  readonly orphans: readonly string[];
}

/** Reads `series` and every patch it lists. Throws PatchFormatError on any malformed entry. */
export function loadSeries(patchesDir: string): Series {
  const seriesFile = join(patchesDir, "series");
  const names = existsSync(seriesFile) ? parseSeries(readFileSync(seriesFile, "utf8")) : [];
  const seen = new Set<string>();
  const patches = names.map((name) => {
    if (name.includes("/") || name.includes("\\") || name.startsWith(".")) {
      throw new PatchFormatError(
        `series entry "${name}" must be a plain file name in ${patchesDir}`,
      );
    }
    if (seen.has(name)) throw new PatchFormatError(`series lists "${name}" twice`);
    seen.add(name);
    const path = join(patchesDir, name);
    if (!existsSync(path))
      throw new PatchFormatError(`series lists "${name}" but ${path} does not exist`);
    return parsePatch(name, path, readFileSync(path, "utf8"));
  });
  const orphans = existsSync(patchesDir)
    ? readdirSync(patchesDir)
        .filter((f) => f.endsWith(".patch") && !seen.has(f))
        .sort()
    : [];
  return { patches, orphans };
}

// ---------------------------------------------------------------------------------------------
// Checkouts, depot_tools, locking

export interface CheckoutContext {
  readonly checkout: CheckoutId;
  readonly paths: BdkPaths;
  /** `$BDK_HOME/chromium/managed` or `.../exp/<id>`: holds `.gclient` and `src/`. */
  readonly root: string;
  readonly src: string;
  readonly layout: ChromiumLayout;
}

export function checkoutContext(
  checkout: CheckoutId,
  paths: BdkPaths = currentBdkPaths(),
  layout: ChromiumLayout = chromiumLayout(),
): CheckoutContext {
  const root = checkoutRoot(checkout, paths);
  return { checkout, paths, root, src: join(root, "src"), layout };
}

export function depotToolsDir(paths: BdkPaths): string {
  return join(paths.home, "depot_tools");
}

/** `env` with `$BDK_HOME/depot_tools` first on PATH. */
export function depotToolsEnv(
  paths: BdkPaths,
  base: NodeJS.ProcessEnv = process.env,
): NodeJS.ProcessEnv {
  return { ...base, PATH: `${depotToolsDir(paths)}:${base.PATH ?? ""}` };
}

/** The per-command flags every checkout-aware command takes. */
export function checkoutFromFlag(experiment: string | undefined): CheckoutId {
  return experiment === undefined ? "managed" : { experiment };
}

/** Marks the process as already holding the lock; set by relaunchUnderLock. */
export const LOCKED_FLAG = "--locked";

/**
 * Re-executes the current script (same runtime flags, same arguments plus `--locked`) under the
 * checkout lock, so a whole sync, patch and build sequence happens under one lock acquisition.
 * Returns the exit code to exit with; a busy lock is reported on stderr and gives 75.
 */
export async function relaunchUnderLock(
  checkout: CheckoutId,
  scriptUrl: string,
  args: readonly string[],
  paths: BdkPaths = currentBdkPaths(),
): Promise<number> {
  try {
    return await runUnderCheckoutLock(
      checkout,
      [process.execPath, ...process.execArgv, fileURLToPath(scriptUrl), ...args, LOCKED_FLAG],
      { paths, env: process.env },
    );
  } catch (e) {
    if (e instanceof LockBusyError) {
      console.error(e.message);
      return 75;
    }
    throw e;
  }
}

/** True when `scriptUrl` is the entry point (so a module can export a library and a CLI). */
export function isMain(scriptUrl: string): boolean {
  const entry = process.argv[1];
  return entry !== undefined && resolve(entry) === fileURLToPath(scriptUrl);
}

export function fail(message: string, code = 1): never {
  console.error(message);
  process.exit(code);
}

export function seconds(ms: number): string {
  return `${(ms / 1000).toFixed(1)}s`;
}
