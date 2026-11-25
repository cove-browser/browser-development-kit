/**
 * Checks every internal link of the static export in `apps/docs/out`: each `href` and `src` that names a
 * path on the site must resolve to a file the export wrote, and each `#fragment` to an `id` on the page
 * it names. External links are not fetched; the build uses no network. Exits 1 with every broken link.
 *
 *   pnpm exec tsx apps/docs/scripts/check-links.ts [<out directory>]
 *
 * BDK_DOCS_BASE_PATH is the base path the site was built with (next.config.mjs).
 */
import { existsSync, readdirSync, readFileSync, realpathSync, statSync } from "node:fs";
import { dirname, join, relative, resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";

export interface LinkReport {
  pages: number;
  checked: number;
  broken: string[];
}

function htmlFiles(dir: string): string[] {
  return readdirSync(dir, { recursive: true, withFileTypes: true })
    .filter((e) => e.isFile() && e.name.endsWith(".html"))
    .map((e) => join(e.parentPath, e.name))
    .sort();
}

/** Every broken internal link in the export at `out`, which was built with `basePath`. */
export function checkLinks(out: string, basePath = ""): LinkReport {
  const root = resolve(out);
  /** The file a site path is served from, as a static host serves the export, or undefined. */
  const served = (path: string): string | undefined => {
    const local = join(root, decodeURIComponent(path));
    if (local !== root && !local.startsWith(`${root}${sep}`)) return undefined;
    for (const candidate of [local, join(local, "index.html"), `${local}.html`]) {
      if (existsSync(candidate) && statSync(candidate).isFile()) return candidate;
    }
    return undefined;
  };
  const ids = new Map<string, Set<string>>();
  const idsOf = (file: string): Set<string> => {
    let found = ids.get(file);
    if (found === undefined) {
      const html = readFileSync(file, "utf8");
      found = new Set([...html.matchAll(/\sid="([^"]+)"/g)].map((m) => m[1] ?? ""));
      ids.set(file, found);
    }
    return found;
  };

  const broken: string[] = [];
  let checked = 0;
  const pages = htmlFiles(root);
  for (const file of pages) {
    const html = readFileSync(file, "utf8");
    const page = `/${relative(root, file).split(sep).join("/")}`;
    for (const match of html.matchAll(/\s(?:href|src)="([^"]+)"/g)) {
      const raw = (match[1] ?? "").replaceAll("&amp;", "&");
      // External sites, mailto: and data: URLs are not the export's.
      if (/^[a-z][a-z0-9+.-]*:/i.test(raw) || raw.startsWith("//")) continue;
      checked++;
      const [pathPart = "", fragment] = raw.split("#", 2);
      const pathOnly = pathPart.split("?")[0] ?? "";
      let target = file;
      if (pathOnly !== "") {
        if (!pathOnly.startsWith("/")) {
          broken.push(`${page}: relative link "${raw}" (links in the export are site-absolute)`);
          continue;
        }
        if (basePath !== "" && pathOnly !== basePath && !pathOnly.startsWith(`${basePath}/`)) {
          broken.push(`${page}: "${raw}" is outside the base path ${basePath}`);
          continue;
        }
        const resolved = served(pathOnly.slice(basePath.length));
        if (resolved === undefined) {
          broken.push(`${page}: "${raw}" names nothing in the export`);
          continue;
        }
        target = resolved;
      }
      if (fragment !== undefined && fragment !== "" && target.endsWith(".html")) {
        if (!idsOf(target).has(decodeURIComponent(fragment))) {
          broken.push(`${page}: "${raw}" names no element with id "${fragment}"`);
        }
      }
    }
  }
  return { pages: pages.length, checked, broken };
}

function main(): void {
  const here = dirname(fileURLToPath(import.meta.url));
  const out = resolve(process.argv[2] ?? join(here, "..", "out"));
  if (!existsSync(out)) {
    console.error(`check-links: ${out} does not exist. Run the build first.`);
    process.exit(1);
  }
  const report = checkLinks(out, process.env.BDK_DOCS_BASE_PATH ?? "");
  if (report.broken.length > 0) {
    console.error(
      `check-links: ${report.broken.length} broken link(s):\n  ${report.broken.join("\n  ")}`,
    );
    process.exit(1);
  }
  console.log(
    `check-links: ${report.checked} internal links in ${report.pages} pages, all resolve.`,
  );
}

const entry = process.argv[1];
if (entry !== undefined && realpathSync(entry) === fileURLToPath(import.meta.url)) main();
