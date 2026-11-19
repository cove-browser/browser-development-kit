/**
 * Builds the route table from a validated bdk.toml and the project's `Pages/` directory:
 * discovery, name normalisation, the precedence rules of architecture.md 8.2, aliases and
 * capability grants. Swift registrations are applied by the application at run time and are
 * not part of this step.
 */
import { existsSync, readdirSync, realpathSync, statSync } from "node:fs";
import { join, sep } from "node:path";
import { checkRouteName, ROUTE_MODES } from "./checks.js";
import { directoryDigest } from "./digest.js";
import { normaliseProjectPath } from "./paths.js";
import { isRecord, type Reporter } from "./reporter.js";
import { hasBuiltin, normaliseRouteName } from "./routes.js";

export type RouteEntry =
  | { mode: "chromium" | "native" | "disabled" }
  | { mode: "bundled"; path: string; host: string; digest: string; capabilities: string[] };

export interface RouteTable {
  routes: Record<string, RouteEntry>;
  aliases: Record<string, string>;
}

function byCodeUnit(a: string, b: string): number {
  return a < b ? -1 : a > b ? 1 : 0;
}

/** Whether `absolute` resolves, through symlinks, to a place inside the project. */
export function isInsideProject(projectDir: string, absolute: string): boolean {
  const root = realpathSync(projectDir);
  const real = realpathSync(absolute);
  return real === root || real.startsWith(root + sep);
}

function isDirectory(path: string): boolean {
  return existsSync(path) && statSync(path).isDirectory();
}

function isFile(path: string): boolean {
  return existsSync(path) && statSync(path).isFile();
}

function bundled(projectDir: string, path: string, route: string): RouteEntry {
  return {
    mode: "bundled",
    path,
    host: `bdk-${route}`,
    digest: directoryDigest(join(projectDir, path)),
    capabilities: [],
  };
}

/** Each directory under the pages directory with an `index.html` is a page. */
function discover(
  projectDir: string,
  pagesDirectory: string,
  reporter: Reporter,
): Record<string, RouteEntry> {
  const found: Record<string, RouteEntry> = {};
  const root = join(projectDir, pagesDirectory);
  if (!isDirectory(root)) return found;
  const owners = new Map<string, string[]>();
  for (const name of readdirSync(root).sort(byCodeUnit)) {
    const path = `${pagesDirectory}/${name}`;
    const absolute = join(root, name);
    if (name.startsWith(".") || !isDirectory(absolute) || !isFile(join(absolute, "index.html"))) {
      continue;
    }
    if (!isInsideProject(projectDir, absolute)) {
      reporter.errorAt(
        "config.path_escape",
        `"${path}" resolves to a place outside the project.`,
        path,
        "Remove the link, or copy the page into the project.",
      );
      continue;
    }
    const route = normaliseRouteName(name);
    if (!checkRouteName(route, [], "Page directory", reporter, path)) continue;
    owners.set(route, [...(owners.get(route) ?? []), path]);
  }
  for (const [route, paths] of owners) {
    const [first = "", ...rest] = paths;
    if (rest.length > 0) {
      reporter.errorAt(
        "pages.collision",
        `${paths.map((p) => `"${p}"`).join(" and ")} both normalise to the route "${route}".`,
        first,
        "Rename or remove all but one of the directories.",
      );
    } else found[route] = bundled(projectDir, first, route);
  }
  return found;
}

/** Applies `[pages.overrides]` on top of the discovered pages. */
function applyOverrides(
  projectDir: string,
  overrides: Record<string, unknown>,
  routes: Record<string, RouteEntry>,
  reporter: Reporter,
): void {
  for (const route of Object.keys(overrides).sort(byCodeUnit)) {
    const value = String(overrides[route]);
    const key = ["pages", "overrides", route];
    if ((ROUTE_MODES as readonly string[]).includes(value)) {
      routes[route] = { mode: value as "chromium" | "native" | "disabled" };
      continue;
    }
    const path = normaliseProjectPath(value);
    const absolute = join(projectDir, path);
    if (!isFile(join(absolute, "index.html"))) {
      reporter.error(
        "pages.missing_entry",
        `"pages.overrides.${route}" points at "${path}", which has no index.html.`,
        key,
        "Point it at a directory that contains index.html.",
      );
    } else if (!isInsideProject(projectDir, absolute)) {
      reporter.error(
        "config.path_escape",
        `"pages.overrides.${route}" resolves to a place outside the project: "${path}".`,
        key,
        "Use a directory inside the project.",
      );
    } else routes[route] = bundled(projectDir, path, route);
  }
}

function checkAliases(
  aliases: Record<string, unknown>,
  routes: Record<string, RouteEntry>,
  reporter: Reporter,
): Record<string, string> {
  const resolved: Record<string, string> = {};
  for (const alias of Object.keys(aliases).sort(byCodeUnit)) {
    const target = String(aliases[alias]);
    const key = ["pages", "routes", alias];
    if (Object.hasOwn(routes, alias)) {
      reporter.error(
        "pages.alias_collision",
        `Alias "${alias}" has the same name as a page.`,
        key,
        "Rename the alias, or remove the page.",
      );
    } else if (!Object.hasOwn(routes, target) && !hasBuiltin(target)) {
      reporter.error(
        "pages.alias_target",
        `Alias "${alias}" points at "${target}", which is not a page.`,
        key,
        "Point the alias at a standard route, or at a directory under Pages/ or an entry in [pages.overrides].",
      );
    } else resolved[alias] = target;
  }
  return resolved;
}

function attachCapabilities(
  grants: Record<string, unknown>,
  routes: Record<string, RouteEntry>,
  aliases: Record<string, unknown>,
  reporter: Reporter,
): void {
  for (const route of Object.keys(grants).sort(byCodeUnit)) {
    const key = ["pages", "capabilities", route];
    const entry = Object.hasOwn(routes, route) ? routes[route] : undefined;
    if (entry?.mode === "bundled") {
      const grant = grants[route];
      const allow = isRecord(grant) && Array.isArray(grant.allow) ? grant.allow.map(String) : [];
      entry.capabilities = [...new Set(allow)];
    } else if (Object.hasOwn(aliases, route)) {
      reporter.error(
        "pages.capability_target",
        `"pages.capabilities.${route}" grants capabilities to an alias. An alias carries its target's capabilities.`,
        key,
        `Grant them to "${String(aliases[route])}" instead.`,
      );
    } else if (entry || hasBuiltin(route)) {
      reporter.error(
        "pages.capability_target",
        `"pages.capabilities.${route}" grants capabilities to a route served in "${entry?.mode ?? "chromium"}" mode. Only bundled pages can hold capabilities.`,
        key,
        "Serve the route from a bundled page, or remove the grant.",
      );
    } else {
      reporter.error(
        "pages.capability_target",
        `"pages.capabilities.${route}" grants capabilities to a route that does not exist.`,
        key,
        `Add a directory for "${route}" under the pages directory, or point [pages.overrides] at one.`,
      );
    }
  }
}

export interface PagesInput {
  projectDir: string;
  pages: Record<string, unknown>;
  extensionsEnabled: boolean;
}

/** Builds the route table. Errors go to `reporter`; the result is only meaningful without any. */
export function buildRouteTable(input: PagesInput, reporter: Reporter): RouteTable {
  const { projectDir, pages } = input;
  const directory = normaliseProjectPath(String(pages.directory ?? "Pages"));
  const overrides = isRecord(pages.overrides) ? pages.overrides : {};
  const aliases = isRecord(pages.routes) ? pages.routes : {};
  const grants = isRecord(pages.capabilities) ? pages.capabilities : {};

  const routes: Record<string, RouteEntry> =
    pages.auto_discover === false ? {} : discover(projectDir, directory, reporter);
  applyOverrides(projectDir, overrides, routes, reporter);
  if (!input.extensionsEnabled && !Object.hasOwn(routes, "extensions")) {
    routes.extensions = { mode: "disabled" };
  }
  const resolvedAliases = checkAliases(aliases, routes, reporter);
  attachCapabilities(grants, routes, aliases, reporter);

  const sorted: Record<string, RouteEntry> = {};
  for (const route of Object.keys(routes).sort(byCodeUnit))
    sorted[route] = routes[route] as RouteEntry;
  return { routes: sorted, aliases: resolvedAliases };
}
