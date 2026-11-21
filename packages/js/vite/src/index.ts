/**
 * BDK's Vite preset for internal pages (javascript-api.md section 8). It sets one entry per page
 * directory, leaves `@bdk/internal/*` to the runtime, emits no inline script, uses relative asset URLs and
 * fails the build on anything the content security policy would block.
 *
 * This file is loaded by the application's own Vite, so it imports `vite` for types only.
 */
import { existsSync, readdirSync, statSync } from "node:fs";
import { join, resolve } from "node:path";
import type { Plugin } from "vite";
import { type CspIssue, cspIssues, formatCspIssues } from "./csp.js";
import { PAGE_ENV } from "./tooling.js";

export { type CspIssue, type CspRule, cspIssues, formatCspIssues, isRemoteUrl } from "./csp.js";
export { PAGE_ENV };

export interface BdkOptions {
  /** The directory that holds the page directories. Default: Vite's `root`. */
  root?: string;
}

/** The page directories under `root`: each directory with an `index.html`, except dot-directories and `node_modules`. */
export function pageDirectories(root: string): string[] {
  return readdirSync(root)
    .filter((name) => !name.startsWith(".") && name !== "node_modules")
    .filter((name) => {
      const dir = join(root, name);
      return statSync(dir).isDirectory() && existsSync(join(dir, "index.html"));
    })
    .sort();
}

const INTERNAL = /^@bdk\/internal\/(.+)$/;

/** Where the runtime serves the `@bdk/internal/<module>` SDK module to every bundled page. */
export function internalUrl(source: string): string | undefined {
  const module = INTERNAL.exec(source)?.[1];
  if (module === undefined) return undefined;
  return `/@bdk/internal/${module.endsWith(".js") ? module : `${module}.js`}`;
}

function fail(issues: readonly CspIssue[]): never {
  throw new Error(
    `[bdk] The content security policy blocks ${issues.length === 1 ? "this" : "these"} in a bundled page:\n${formatCspIssues(issues)}`,
  );
}

/** The Vite plugins of the preset. Add them to `plugins` in `Pages/vite.config.ts`. */
export function bdk(options: BdkOptions = {}): Plugin[] {
  let root = process.cwd();
  return [
    {
      name: "bdk:pages",
      config(user) {
        const pagesRoot = resolve(options.root ?? user.root ?? process.cwd());
        const only = process.env[PAGE_ENV];
        const alone = only !== undefined && only !== "";
        // One page alone is built with its own directory as the root, so its index.html and every file
        // it needs land at the top of the output and the output can be served as the page's whole origin.
        // Several pages share chunks in assets/; the CLI builds them one at a time for that reason.
        root = alone ? join(pagesRoot, only) : pagesRoot;
        if (alone && !existsSync(join(root, "index.html"))) {
          throw new Error(
            `[bdk] ${PAGE_ENV}="${only}" names no page: ${join(root, "index.html")} does not exist.`,
          );
        }
        const pages = alone ? [only] : pageDirectories(pagesRoot);
        return {
          ...(alone ? { root } : {}),
          base: "./",
          build: {
            assetsInlineLimit: 0,
            modulePreload: { polyfill: false },
            rollupOptions: {
              input: Object.fromEntries(
                pages.map((name) => [name, join(pagesRoot, name, "index.html")]),
              ),
            },
          },
        };
      },
      resolveId(source) {
        const id = internalUrl(source);
        return id === undefined ? null : { id, external: "absolute" };
      },
      transformIndexHtml: {
        order: "pre",
        handler(html, context) {
          // Vite turns an inline module script into a chunk, so this is the only place it can be seen.
          const issues = cspIssues(html, context.filename);
          if (issues.length > 0) fail(issues);
        },
      },
      generateBundle(_options, bundle) {
        const issues: CspIssue[] = [];
        for (const [name, output] of Object.entries(bundle)) {
          if (output.type === "asset" && name.endsWith(".html")) {
            issues.push(...cspIssues(String(output.source), join(root, name)));
          }
        }
        if (issues.length > 0) fail(issues);
      },
    },
  ];
}
