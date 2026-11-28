import { existsSync, readdirSync, readFileSync, realpathSync, writeFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { isMain, repoRootFromHere, takeValue } from "../runtime/cli.js";

/**
 * Third-party notices for what BDK ships outside the Chromium runtime (T-REL-02): the npm packages bundled
 * into a workspace package (the `bdk` executable bundles its production dependencies) and, for the
 * executable, Node itself. The runtime's own notices are Chromium's credits page (chromium/gn/release.gn,
 * chromium/runtime-files.json). Licence policy: docs/release/licence-review.md.
 */
export interface NoticeEntry {
  readonly name: string;
  readonly version: string;
  /** The `license` field of package.json, an SPDX expression. */
  readonly license: string;
  /** The text of the package's licence file(s), empty when it ships none. */
  readonly text: string;
}

/** Licences BDK may ship without a separate review (docs/release/licence-review.md section 3). */
export const ACCEPTED_LICENCES: ReadonlySet<string> = new Set([
  "0BSD",
  "Apache-2.0",
  "BlueOak-1.0.0",
  "BSD-2-Clause",
  "BSD-3-Clause",
  "CC0-1.0",
  "ISC",
  "MIT",
  "Unlicense",
]);

/**
 * Whether an SPDX expression is acceptable: `OR` needs one accepted alternative, `AND` needs every part.
 * Parentheses are allowed around the whole expression or a group; `WITH` exceptions need review.
 */
export function licenceAccepted(expression: string): boolean {
  const e = expression.trim().replace(/^\((.*)\)$/, "$1");
  if (e === "" || / WITH /.test(e)) return false;
  if (/ OR /.test(e)) return e.split(/ OR /).some((part) => licenceAccepted(part));
  if (/ AND /.test(e)) return e.split(/ AND /).every((part) => licenceAccepted(part));
  return ACCEPTED_LICENCES.has(e);
}

interface PackageJson {
  readonly name?: string;
  readonly version?: string;
  readonly private?: boolean;
  readonly license?: unknown;
  readonly dependencies?: Readonly<Record<string, string>>;
  readonly optionalDependencies?: Readonly<Record<string, string>>;
}

function readPackage(dir: string): PackageJson {
  return JSON.parse(readFileSync(join(dir, "package.json"), "utf8")) as PackageJson;
}

/** Node's resolution: `node_modules/<name>` in the package's real directory or any parent. */
function resolveDependency(fromDir: string, name: string): string | undefined {
  let dir = fromDir;
  for (;;) {
    const candidate = join(dir, "node_modules", name);
    if (existsSync(join(candidate, "package.json"))) return realpathSync(candidate);
    const parent = dirname(dir);
    if (parent === dir) return undefined;
    dir = parent;
  }
}

const LICENCE_FILE = /^(licen[cs]e|copying|notice)([.-].*)?$/i;

function licenceText(dir: string): string {
  return readdirSync(dir)
    .filter((f) => LICENCE_FILE.test(f))
    .sort()
    .map((f) => readFileSync(join(dir, f), "utf8").trimEnd())
    .join("\n\n");
}

export class NoticesError extends Error {
  override readonly name = "NoticesError";
}

/**
 * The production dependency closure of `packageDirs` (dependencies and installed optional dependencies, never
 * devDependencies), sorted by name and version. Workspace packages (`workspace:` specifiers) are BDK's own:
 * their dependencies are followed, they are not listed.
 */
export function collectNotices(packageDirs: readonly string[]): NoticeEntry[] {
  const seen = new Map<string, NoticeEntry>();
  const visited = new Set<string>();
  const visit = (dir: string, own: boolean): void => {
    const real = realpathSync(dir);
    if (visited.has(real)) return;
    visited.add(real);
    const pkg = readPackage(real);
    if (!own) {
      const key = `${pkg.name}@${pkg.version}`;
      seen.set(key, {
        name: pkg.name ?? "(unnamed)",
        version: pkg.version ?? "",
        license: typeof pkg.license === "string" ? pkg.license : "",
        text: licenceText(real),
      });
    }
    const deps: [string, string, boolean][] = [
      ...Object.entries(pkg.dependencies ?? {}).map(([n, v]): [string, string, boolean] => [
        n,
        v,
        true,
      ]),
      ...Object.entries(pkg.optionalDependencies ?? {}).map(([n, v]): [string, string, boolean] => [
        n,
        v,
        false,
      ]),
    ];
    for (const [name, spec, required] of deps) {
      const found = resolveDependency(real, name);
      if (found === undefined) {
        if (required) {
          throw new NoticesError(`${pkg.name ?? real} depends on ${name}, which is not installed.`);
        }
        continue;
      }
      visit(found, spec.startsWith("workspace:"));
    }
  };
  for (const dir of packageDirs) visit(resolve(dir), true);
  return [...seen.values()].sort((a, b) =>
    a.name === b.name ? a.version.localeCompare(b.version) : a.name.localeCompare(b.name),
  );
}

/** Entries BDK cannot ship as they are: no licence field, a licence needing review, or no licence text. */
export function noticeProblems(entries: readonly NoticeEntry[]): string[] {
  const problems: string[] = [];
  for (const e of entries) {
    const id = `${e.name}@${e.version}`;
    if (e.license === "") problems.push(`${id}: no licence in package.json`);
    else if (!licenceAccepted(e.license))
      problems.push(`${id}: ${e.license} needs a licence review (docs/release/licence-review.md)`);
    if (e.text === "") problems.push(`${id}: ships no licence file to reproduce`);
  }
  return problems;
}

export interface ExtraNotice {
  readonly name: string;
  readonly license: string;
  readonly text: string;
}

export function renderNotices(
  title: string,
  entries: readonly NoticeEntry[],
  extra: readonly ExtraNotice[] = [],
): string {
  const rule = "-".repeat(72);
  const blocks = [
    ...extra.map((x) => `${x.name}\nLicence: ${x.license}\n\n${x.text}`),
    ...entries.map((e) => `${e.name} ${e.version}\nLicence: ${e.license}\n\n${e.text}`),
  ];
  return `${title}\n\nThis file lists the third-party software included in it and reproduces each licence.\n\n${blocks
    .map((b) => `${rule}\n${b}\n`)
    .join("\n")}`;
}

/** Node's licence, from the installation that runs this script (`<prefix>/LICENSE`): the executable embeds it. */
export function nodeNotice(execPath = process.execPath): ExtraNotice {
  const file = join(dirname(dirname(realpathSync(execPath))), "LICENSE");
  if (!existsSync(file)) throw new NoticesError(`Node's licence is not at ${file}.`);
  return {
    name: `Node.js ${process.version}`,
    license: "MIT and others (see text)",
    text: readFileSync(file, "utf8").trimEnd(),
  };
}

const USAGE =
  "usage: notices.ts --package <dir> [--package <dir>...] [--node] --out <file>\n" +
  "  Writes the third-party notices for the packages' production dependencies (and Node, with --node).\n" +
  "  Fails if any dependency has no licence, a licence that needs review, or no licence text.";

async function main(argv: string[]): Promise<number> {
  const packages: string[] = [];
  let out: string | undefined;
  let node = false;
  try {
    for (let i = 0; i < argv.length; i++) {
      const a = argv[i];
      if (a === "--package") packages.push(takeValue(argv, i++, a));
      else if (a === "--out") out = takeValue(argv, i++, a);
      else if (a === "--node") node = true;
      else throw new Error(`unknown argument "${a}"\n${USAGE}`);
    }
    if (packages.length === 0 || out === undefined) throw new Error(USAGE);
    const root = repoRootFromHere(import.meta.url);
    const entries = collectNotices(packages.map((p) => resolve(root, p)));
    const problems = noticeProblems(entries);
    if (problems.length > 0) {
      console.error(`notices: ${problems.length} problem(s):\n  ${problems.join("\n  ")}`);
      return 1;
    }
    writeFileSync(
      out,
      renderNotices(
        `Third-party notices for ${packages.join(", ")}`,
        entries,
        node ? [nodeNotice()] : [],
      ),
    );
    console.log(`notices: ${entries.length} packages${node ? " and Node" : ""} -> ${out}`);
    return 0;
  } catch (error) {
    console.error(`notices: ${(error as Error).message}`);
    return 2;
  }
}

if (isMain(import.meta.url)) process.exitCode = await main(process.argv.slice(2));
