/** The capability catalogue for internal pages (services.md section 4). */

/** Capabilities a page may be granted. */
const PAGE_GRANTABLE: ReadonlySet<string> = new Set([
  "runtime.read",
  "profiles.read",
  "profiles.manage",
  "settings.read",
  "settings.write",
  "history.read",
  "history.write",
  "bookmarks.read",
  "bookmarks.write",
  "downloads.read",
  "downloads.manage",
  "permissions.read",
  "permissions.manage",
  "cookies.read",
  "cookies.manage",
  "privacy.clear",
  "passwords.read",
  "passwords.manage",
  "passwords.reveal",
  "autofill.read",
  "autofill.manage",
  "extensions.list",
  "extensions.manage",
  "translation.read",
  "translation.manage",
  "search.read",
  "search.manage",
  "favicons.read",
  "top-sites.read",
  "system.default-browser",
]);

/** Capabilities that exist but are never granted to a page. */
const SESSION_ONLY: ReadonlySet<string> = new Set([
  "cookies.values",
  "contents.read",
  "contents.control",
  "contents.script",
  "contents.capture",
  "contents.debug",
]);

/** Families that no page may hold, whatever follows the prefix. */
const DENIED_PREFIXES: readonly string[] = ["contents.", "automation.", "devtools."];

/** `app.<name>` or `app.<prefix>.*`, with dot-separated segments. */
const APP_CAPABILITY = /^app\.[A-Za-z][A-Za-z0-9-]*(\.[A-Za-z][A-Za-z0-9-]*)*(\.\*)?$/;

export type CapabilityVerdict = "ok" | "unknown" | "denied";

export function classifyCapability(name: string): CapabilityVerdict {
  if (DENIED_PREFIXES.some((prefix) => name.startsWith(prefix)) || SESSION_ONLY.has(name)) {
    return "denied";
  }
  if (name.startsWith("app.")) return APP_CAPABILITY.test(name) ? "ok" : "unknown";
  return PAGE_GRANTABLE.has(name) ? "ok" : "unknown";
}
