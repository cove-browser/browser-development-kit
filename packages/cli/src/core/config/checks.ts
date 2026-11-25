/**
 * The semantic checks of bdk.toml that a JSON Schema cannot express and that need no file system:
 * internal URLs, credentials, paths, source-only keys, the deny list, the scheme, route names,
 * alias shapes and capability grants. Each is defensive about types, because it also runs on
 * documents the schema has already rejected.
 */
import { classifyCapability } from "./capabilities.js";
import { denyReason } from "./denylist.js";
import type { KeyPath } from "./locator.js";
import { pathProblem } from "./paths.js";
import { dotted, isRecord, type Reporter } from "./reporter.js";
import {
  hasBuiltin,
  isExcludedRoute,
  isReservedRoute,
  isValidRouteName,
  UNDISABLEABLE_ROUTES,
} from "./routes.js";

export const ROUTE_MODES = ["chromium", "native", "disabled"] as const;

const INTERNAL_URL = /(chrome|chrome-untrusted|devtools):\/\//i;

const CREDENTIAL_VALUES: readonly RegExp[] = [
  /AIza[0-9A-Za-z_-]{35}/,
  /\bsk-[A-Za-z0-9_-]{20,}/,
  /\bgh[pousr]_[A-Za-z0-9]{36,}/,
  /\bxox[abprs]-[A-Za-z0-9-]{10,}/,
  /\bAKIA[0-9A-Z]{16}\b/,
  /-----BEGIN [A-Z ]*PRIVATE KEY-----/,
  /\beyJ[A-Za-z0-9_-]{10,}\.eyJ[A-Za-z0-9_-]{10,}\./,
];
const CREDENTIAL_NAME = /(api[_-]?key|secret|token|passw(or)?d|credential|private[_-]?key)/i;
const OPAQUE_VALUE = /^[A-Za-z0-9_-]{16,}$/;

/** URL schemes an application's internal scheme may not take (configuration.md 3.1). */
const RESERVED_SCHEMES: ReadonlySet<string> = new Set([
  "http",
  "https",
  "file",
  "chrome",
  "chrome-untrusted",
  "chrome-extension",
  "chrome-error",
  "chrome-search",
  "chrome-native",
  "devtools",
  "about",
  "blob",
  "data",
  "javascript",
  "view-source",
  "ws",
  "wss",
  "ftp",
  "mailto",
  "tel",
  "sms",
  "filesystem",
  "isolated-app",
  "cid",
  "content",
  "urn",
]);

const SCHEME = /^[a-z][a-z0-9+.-]*$/;

export function isReservedScheme(scheme: string): boolean {
  return RESERVED_SCHEMES.has(scheme);
}

/** Lower-cases the name and drops what a URL scheme cannot contain (architecture.md 8.3). */
export function deriveScheme(name: string): string {
  return name.toLowerCase().replace(/[^a-z0-9+.-]/g, "");
}

function table(value: unknown, key: string): Record<string, unknown> {
  const child = isRecord(value) ? value[key] : undefined;
  return isRecord(child) ? child : {};
}

function walkStrings(
  value: unknown,
  path: (string | number)[],
  visit: (s: string, p: KeyPath) => void,
): void {
  if (typeof value === "string") visit(value, path);
  else if (Array.isArray(value)) {
    for (const [n, item] of value.entries()) walkStrings(item, [...path, n], visit);
  } else if (isRecord(value)) {
    for (const [key, child] of Object.entries(value)) walkStrings(child, [...path, key], visit);
  }
}

function checkValues(doc: Record<string, unknown>, reporter: Reporter): void {
  walkStrings(doc, [], (value, path) => {
    const name = dotted(path);
    const url = INTERNAL_URL.exec(value);
    if (url) {
      reporter.error(
        "config.internal_url",
        `"${name}" contains an internal Chromium URL ("${url[0]}"). Pages are named logically.`,
        path,
        'Use the logical route name, for example "settings" instead of chrome://settings.',
      );
    }
    const key = String(path[path.length - 1]);
    if (
      CREDENTIAL_VALUES.some((pattern) => pattern.test(value)) ||
      (CREDENTIAL_NAME.test(key) && OPAQUE_VALUE.test(value))
    ) {
      reporter.error(
        "config.secret",
        `"${name}" looks like a credential. Credentials do not belong in bdk.toml.`,
        path,
        "Supply the key from Swift or a build setting, and remove it from the file.",
      );
    }
  });
}

function checkPath(value: unknown, path: KeyPath, reporter: Reporter): void {
  if (typeof value !== "string") return;
  const problem = pathProblem(value);
  if (problem) {
    reporter.error(
      "config.path_escape",
      `"${dotted(path)}" is not a path inside the project: "${value}". ${problem}`,
      path,
      "Use a relative path with forward slashes that stays inside the directory of bdk.toml.",
    );
  }
}

function checkPaths(doc: Record<string, unknown>, reporter: Reporter): void {
  checkPath(table(doc, "pages").directory, ["pages", "directory"], reporter);
  checkPath(table(doc, "automation").directory, ["automation", "directory"], reporter);
  checkPath(table(doc, "devtools").overlay, ["devtools", "overlay"], reporter);
  const frontend = table(doc, "devtools").frontend;
  if (frontend !== "default" && frontend !== "disabled") {
    checkPath(frontend, ["devtools", "frontend"], reporter);
  }
  const patches = table(doc, "chromium").patches;
  if (Array.isArray(patches)) {
    for (const [n, patch] of patches.entries())
      checkPath(patch, ["chromium", "patches", n], reporter);
  } else checkPath(patches, ["chromium", "patches"], reporter);
  for (const [route, value] of Object.entries(table(table(doc, "pages"), "overrides"))) {
    if (typeof value === "string" && !isRouteMode(value)) {
      checkPath(value, ["pages", "overrides", route], reporter);
    }
  }
  for (const [name, value] of Object.entries(table(table(doc, "extensions"), "bundled"))) {
    if (typeof value === "string") checkPath(value, ["extensions", "bundled", name], reporter);
    else if (isRecord(value))
      checkPath(value.path, ["extensions", "bundled", name, "path"], reporter);
  }
}

function isRouteMode(value: string): boolean {
  return (ROUTE_MODES as readonly string[]).includes(value);
}

function checkChromium(doc: Record<string, unknown>, reporter: Reporter): void {
  const chromium = table(doc, "chromium");
  const mode = table(doc, "runtime").mode;
  if (mode !== "source") {
    for (const key of Object.keys(chromium)) {
      reporter.error(
        "config.source_only",
        `"chromium.${key}" is only valid when runtime.mode = "source".`,
        ["chromium", key],
        'Set runtime.mode = "source", or remove the key. A prebuilt runtime fixes the Chromium revision.',
      );
    }
  }
  for (const [arg, value] of Object.entries(table(chromium, "gn_args"))) {
    const tokens =
      typeof value === "string" ? value.split(/\s+/).filter((t) => t.startsWith("-")) : [];
    for (const name of [arg, ...tokens]) {
      const reason = denyReason(name);
      if (reason) {
        reporter.error(
          "config.denied_switch",
          `"${name}" is on the Chromium deny list: ${reason}.`,
          ["chromium", "gn_args", arg],
          "Remove it. Denied switches cannot be enabled by configuration.",
        );
      }
    }
  }
}

function checkScheme(doc: Record<string, unknown>, reporter: Reporter): void {
  const browser = table(doc, "browser");
  const explicit = browser.internal_scheme;
  if (typeof explicit === "string") {
    if (SCHEME.test(explicit) && isReservedScheme(explicit)) {
      reporter.error(
        "browser.scheme_invalid",
        `"browser.internal_scheme" is a reserved scheme: "${explicit}".`,
        ["browser", "internal_scheme"],
        "Choose a scheme that is not used by the web platform or Chromium.",
      );
    }
    return;
  }
  const name = browser.name;
  if (explicit === undefined && typeof name === "string" && name.length > 0) {
    const derived = deriveScheme(name);
    if (!SCHEME.test(derived)) {
      reporter.error(
        "browser.scheme_invalid",
        `The scheme derived from "browser.name" is not valid: "${derived}". A scheme starts with a letter.`,
        ["browser", "name"],
        "Set browser.internal_scheme explicitly.",
      );
    } else if (isReservedScheme(derived)) {
      reporter.error(
        "browser.scheme_invalid",
        `The scheme derived from "browser.name" is reserved: "${derived}".`,
        ["browser", "name"],
        "Set browser.internal_scheme explicitly.",
      );
    }
  }
}

/** Reports a route name that is malformed or reserved. Returns whether it is usable. */
export function checkRouteName(
  name: string,
  path: KeyPath,
  what: string,
  reporter: Reporter,
  file?: string,
): boolean {
  const report = (code: string, message: string, fix: string) =>
    file === undefined
      ? reporter.error(code, message, path, fix)
      : reporter.errorAt(code, message, file, fix);
  if (isExcludedRoute(name)) {
    report(
      "pages.reserved",
      `${what} "${name}" uses a reserved name. The feature is excluded from the runtime, so the route is never reachable.`,
      "Choose another name.",
    );
    return false;
  }
  if (isReservedRoute(name)) {
    report(
      "pages.reserved",
      `${what} "${name}" uses a reserved name. Chromium needs it internally.`,
      "Choose another name. Reserved names include resources, theme, favicon, print, devtools, error and anything starting with bdk-.",
    );
    return false;
  }
  if (!isValidRouteName(name)) {
    report(
      "pages.name_invalid",
      `${what} "${name}" is not a valid route name. Route names match [a-z][a-z0-9-]*.`,
      "Use lower-case letters, digits and hyphens, starting with a letter.",
    );
    return false;
  }
  return true;
}

function checkPages(doc: Record<string, unknown>, reporter: Reporter): void {
  const pages = table(doc, "pages");
  const overrides = table(pages, "overrides");
  const aliases = table(pages, "routes");

  for (const [route, value] of Object.entries(overrides)) {
    const path = ["pages", "overrides", route];
    if (!checkRouteName(route, path, "Route", reporter)) continue;
    if (value === "chromium" && !hasBuiltin(route)) {
      reporter.error(
        "pages.no_builtin",
        `"pages.overrides.${route}" is "chromium", but the route "${route}" has no built-in page.`,
        path,
        `Use a path to a bundled page, or "native" or "disabled".`,
      );
    }
    if (value === "disabled" && UNDISABLEABLE_ROUTES.includes(route)) {
      reporter.error(
        "config.type",
        `"pages.overrides.${route}" cannot be "disabled": the third-party notices have to ship.`,
        path,
        "Replace the page with a bundled one that shows the same notices, or remove the override.",
      );
    }
  }

  for (const [alias, target] of Object.entries(aliases)) {
    const path = ["pages", "routes", alias];
    if (!checkRouteName(alias, path, "Alias", reporter)) continue;
    if (typeof target === "string" && Object.hasOwn(aliases, target)) {
      reporter.error(
        "pages.alias_chain",
        `Alias "${alias}" points at "${target}", which is also an alias. An alias must point at a page.`,
        path,
        `Point "${alias}" at the page that "${target}" resolves to.`,
      );
    } else if (Object.hasOwn(overrides, alias) || hasBuiltin(alias)) {
      reporter.error(
        "pages.alias_collision",
        `Alias "${alias}" has the same name as a page.`,
        path,
        "Rename the alias, or remove the page's override.",
      );
    }
  }

  for (const [route, grant] of Object.entries(table(pages, "capabilities"))) {
    const base = ["pages", "capabilities", route];
    checkRouteName(route, base, "Route", reporter);
    const allow = isRecord(grant) ? grant.allow : undefined;
    if (!Array.isArray(allow)) continue;
    allow.forEach((capability, n) => {
      if (typeof capability !== "string") return;
      const path = [...base, "allow", n];
      const verdict = classifyCapability(capability);
      if (verdict === "denied") {
        reporter.error(
          "pages.capability_denied",
          `"${capability}" can never be granted to a page. Only Swift and automation sessions can hold it.`,
          path,
          'A page that needs this calls an application command ("app.…") and the application decides.',
        );
      } else if (verdict === "unknown") {
        reporter.error(
          "pages.capability_unknown",
          `"${capability}" is not a BDK capability. A trailing "*" is allowed only after "app.".`,
          path,
          "See the capability list in services.md section 4.",
        );
      }
    });
  }
}

/** Runs every file-system-free semantic check on a parsed document. */
export function checkDocument(doc: Record<string, unknown>, reporter: Reporter): void {
  checkValues(doc, reporter);
  checkPaths(doc, reporter);
  checkChromium(doc, reporter);
  checkScheme(doc, reporter);
  checkPages(doc, reporter);
}
