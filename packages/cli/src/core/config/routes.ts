/** Logical route names: normalisation, validity, and the fixed lists (configuration.md section 4). */

/** Standard routes that have a built-in page (configuration.md 4.1). */
export const STANDARD_ROUTES: readonly string[] = [
  "new-tab",
  "settings",
  "extensions",
  "history",
  "bookmarks",
  "downloads",
  "passwords",
  "about",
  "credits",
  "flags",
];

/** Diagnostic routes, governed together by a single switch (configuration.md 4.2). */
export const DIAGNOSTIC_ROUTES: readonly string[] = [
  "gpu",
  "net-export",
  "net-internals",
  "sandbox",
  "process-internals",
  "inspect",
  "tracing",
  "crashes",
  "components",
  "media-internals",
  "webrtc-internals",
  "serviceworker-internals",
  "accessibility",
  "histograms",
  "quota-internals",
  "indexeddb-internals",
  "blob-internals",
  "extensions-internals",
];

/** Names Chromium needs internally (configuration.md 4.3). Names starting `bdk-` are reserved too. */
export const RESERVED_ROUTES: readonly string[] = [
  "resources",
  "theme",
  "favicon",
  "favicon2",
  "print",
  "devtools",
  "network-error",
  "error",
  "chrome-error",
  "chrome-search",
  "chrome-untrusted",
];

/** Pages that are never reachable because the feature is excluded (configuration.md 4.4). */
export const EXCLUDED_ROUTES: readonly string[] = [
  "signin",
  "sign-in",
  "sync-internals",
  "feedback",
  "whats-new",
  "welcome",
];

/** Routes whose navigation cannot be disabled. */
export const UNDISABLEABLE_ROUTES: readonly string[] = ["credits"];

const ROUTE_NAME = /^[a-z][a-z0-9-]*$/;
const RESERVED_PREFIX = "bdk-";

const BUILTIN_ROUTES = new Set([...STANDARD_ROUTES, ...DIAGNOSTIC_ROUTES]);

/**
 * Directory names are split on case boundaries and lower-cased with hyphens:
 * `NewTab` becomes `new-tab`, `HTMLPage` becomes `html-page`.
 */
export function normaliseRouteName(directoryName: string): string {
  return directoryName
    .replace(/([a-z0-9])([A-Z])/g, "$1-$2")
    .replace(/([A-Z]+)([A-Z][a-z])/g, "$1-$2")
    .toLowerCase();
}

export function isValidRouteName(name: string): boolean {
  return ROUTE_NAME.test(name);
}

export function isReservedRoute(name: string): boolean {
  return RESERVED_ROUTES.includes(name) || name.startsWith(RESERVED_PREFIX);
}

export function isExcludedRoute(name: string): boolean {
  return EXCLUDED_ROUTES.includes(name);
}

export function hasBuiltin(name: string): boolean {
  return BUILTIN_ROUTES.has(name);
}
