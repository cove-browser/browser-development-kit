/**
 * Assembles `bdk.resolved.json` (configuration.md section 7) from a validated bdk.toml and the
 * project directory. Every default is written out, because the engine has none of its own.
 */
import { existsSync, readdirSync, statSync } from "node:fs";
import { join } from "node:path";
import { deriveScheme } from "./checks.js";
import { fileDigest, sha256 } from "./digest.js";
import type { BundleFacts } from "./index.js";
import { buildRouteTable, isInsideProject } from "./pages.js";
import { normaliseProjectPath } from "./paths.js";
import { isRecord, type Reporter } from "./reporter.js";

/** The `schema` field of the resolved file. */
export const RESOLVED_SCHEMA_VERSION = 1;

export const DEFAULT_BDK_VERSION = "0.0.0";

type Doc = Record<string, unknown>;

function table(value: unknown, key: string): Doc {
  const child = isRecord(value) ? value[key] : undefined;
  return isRecord(child) ? child : {};
}

function pick<T>(value: unknown, fallback: T): T {
  return value === undefined ? fallback : (value as T);
}

/** The property names of a table in the schema, in schema order. */
function schemaKeys(schema: unknown, path: string[]): string[] {
  let node: unknown = schema;
  for (const key of path) node = isRecord(node) ? node[key] : undefined;
  return isRecord(node) ? Object.keys(node) : [];
}

function surfaceModes(
  schema: unknown,
  path: string[],
  configured: Doc,
  fallback: string,
): Record<string, string> {
  const out: Record<string, string> = {};
  for (const key of schemaKeys(schema, path)) out[key] = pick(configured[key], fallback);
  return out;
}

function optional<T extends Record<string, unknown>>(
  extra: Doc,
  key: string,
): T | Record<string, never> {
  return extra[key] === undefined ? {} : ({ [key]: extra[key] } as T);
}

function discoverScripts(
  projectDir: string,
  directory: string,
  reporter: Reporter,
): Record<string, { path: string; digest: string }> {
  // A Map, not an object: a script may be called `__proto__`.
  const scripts = new Map<string, { path: string; digest: string }>();
  const root = join(projectDir, directory);
  if (!existsSync(root) || !statSync(root).isDirectory()) return {};
  const owners = new Map<string, string[]>();
  for (const name of readdirSync(root).sort((a, b) => (a < b ? -1 : a > b ? 1 : 0))) {
    const match = /^(.+?)(?<!\.d)\.(ts|js)$/.exec(name);
    if (!match || !statSync(join(root, name)).isFile()) continue;
    owners.set(match[1] as string, [...(owners.get(match[1] as string) ?? []), name]);
  }
  for (const [script, names] of owners) {
    const [first = ""] = names;
    if (names.length > 1) {
      reporter.errorAt(
        "automation.collision",
        `${names.map((n) => `"${directory}/${n}"`).join(" and ")} are both the script "${script}".`,
        `${directory}/${first}`,
        "Keep one file per script name.",
      );
    } else if (!isInsideProject(projectDir, join(root, first))) {
      reporter.errorAt(
        "config.path_escape",
        `"${directory}/${first}" resolves to a place outside the project.`,
        `${directory}/${first}`,
        "Remove the link, or copy the script into the project.",
      );
    } else {
      scripts.set(script, {
        path: `${directory}/${first}`,
        digest: fileDigest(join(root, first)),
      });
    }
  }
  // fromEntries defines own properties, so `__proto__` stays a key instead of a prototype.
  return Object.fromEntries(scripts);
}

function sortedObject<T>(value: Record<string, T>): Record<string, T> {
  return Object.fromEntries(Object.entries(value).sort(([a], [b]) => (a < b ? -1 : a > b ? 1 : 0)));
}

/** Checks `Info.plist` and entitlements facts the build step passes in. */
export function checkBundleFacts(
  facts: BundleFacts | undefined,
  scheme: string,
  schemeKey: string[],
  reporter: Reporter,
): void {
  if (!facts) return;
  const { infoPlist, entitlements } = facts;
  if (infoPlist) {
    const types = Array.isArray(infoPlist.CFBundleURLTypes) ? infoPlist.CFBundleURLTypes : [];
    const declared = types.some(
      (entry) =>
        isRecord(entry) &&
        Array.isArray(entry.CFBundleURLSchemes) &&
        entry.CFBundleURLSchemes.includes(scheme),
    );
    if (declared) {
      reporter.error(
        "browser.scheme_conflict",
        `The internal scheme "${scheme}" is also declared in Info.plist (CFBundleURLTypes).`,
        schemeKey,
        "Choose another internal scheme, or remove the URL type from Info.plist.",
      );
    }
    const principal = infoPlist.NSPrincipalClass;
    if (typeof principal !== "string" || principal === "") {
      reporter.errorAt(
        "plist.principal_class",
        "Info.plist has no NSPrincipalClass. It must be BDKApplication or a subclass.",
        "Info.plist",
        "Set NSPrincipalClass to BDKApplication.",
      );
    } else if (principal !== "BDKApplication") {
      reporter.diagnostics.push({
        code: "plist.principal_class",
        severity: "warning",
        message: `Info.plist sets NSPrincipalClass to "${principal}". It must be BDKApplication or a subclass, which cannot be checked from the plist.`,
        file: "Info.plist",
      });
    }
  }
  if (infoPlist) checkPasskeyFacts(infoPlist, entitlements, reporter);
  if (entitlements?.["com.apple.security.app-sandbox"] === true) {
    reporter.errorAt(
      "plist.app_sandbox",
      "The App Sandbox entitlement (com.apple.security.app-sandbox) is set on the application.",
      "entitlements",
      "Remove it. The browser process is not expected to run under the App Sandbox.",
    );
  }
}

/**
 * Passkeys on a phone (hybrid, ui-requests.md 5.18): Chromium offers them only to an application
 * that may use Bluetooth. A note, not a warning: an application may not want them, and security
 * keys work either way.
 */
function checkPasskeyFacts(
  infoPlist: Record<string, unknown>,
  entitlements: Record<string, unknown> | undefined,
  reporter: Reporter,
): void {
  const usage = infoPlist.NSBluetoothAlwaysUsageDescription;
  if (typeof usage !== "string" || usage === "") {
    reporter.diagnostics.push({
      code: "plist.passkeys_phone",
      severity: "note",
      message:
        "Info.plist has no NSBluetoothAlwaysUsageDescription, so passkeys on a phone are not offered. Security keys still work.",
      file: "Info.plist",
      fix: "To offer them, add NSBluetoothAlwaysUsageDescription and, for a hardened-runtime build, the com.apple.security.device.bluetooth entitlement.",
    });
  } else if (entitlements && entitlements["com.apple.security.device.bluetooth"] !== true) {
    reporter.diagnostics.push({
      code: "plist.passkeys_phone",
      severity: "note",
      message:
        "The entitlements have no com.apple.security.device.bluetooth, so a hardened-runtime build cannot use Bluetooth for Web Bluetooth or passkeys on a phone.",
      file: "entitlements",
      fix: "Add com.apple.security.device.bluetooth to the entitlements.",
    });
  }
}

export interface AssembleInput {
  doc: Doc;
  projectDir: string;
  schema: unknown;
  bundleFacts: BundleFacts | undefined;
  bdkVersion: string;
  reporter: Reporter;
  schemeKey: string[];
}

/** Returns the resolved object without `config_digest` filled in, or `undefined` after errors. */
export function assemble(input: AssembleInput): Record<string, unknown> | undefined {
  const { doc, projectDir, schema, bundleFacts, reporter } = input;
  const browser = table(doc, "browser");
  const pages = table(doc, "pages");
  const ui = table(doc, "ui");
  const extensions = table(doc, "extensions");
  const install = table(extensions, "install");
  const services = table(doc, "services");
  const automation = table(doc, "automation");
  const devtools = table(doc, "devtools");

  const name = String(browser.name);
  const scheme = pick(browser.internal_scheme as string | undefined, deriveScheme(name));
  checkBundleFacts(bundleFacts, scheme, input.schemeKey, reporter);

  const routeTable = buildRouteTable(
    { projectDir, pages, extensionsEnabled: pick(extensions.enabled, true) },
    reporter,
  );

  const automationEnabled = pick(automation.enabled, false);
  const automationDirectory = normaliseProjectPath(
    String(pick(automation.directory, "Automation")),
  );
  const scripts = automationEnabled
    ? discoverScripts(projectDir, automationDirectory, reporter)
    : {};

  if (reporter.hasErrors) return undefined;

  const surfaceDefault = pick(ui.default_browser_surfaces, true) ? "default" : "custom";
  const brandVersion = bundleFacts?.infoPlist?.CFBundleShortVersionString;
  const translation = table(services, "translation");
  const reputation = table(services, "reputation");
  const geolocation = table(services, "geolocation");
  const frontend = pick(devtools.frontend, "default");

  return {
    schema: RESOLVED_SCHEMA_VERSION,
    bdk_version: input.bdkVersion,
    browser: {
      name,
      internal_scheme: scheme,
      logo: "app-icon",
      user_agent: typeof brandVersion === "string" ? { brand_version: brandVersion } : {},
    },
    storage: { directory: name, keychain_item: `${name} Safe Storage` },
    runtime: {
      mode: pick(table(doc, "runtime").mode, "prebuilt"),
      downgrade: pick(table(doc, "runtime").downgrade, "refuse"),
      snapshot_on_upgrade: pick(table(doc, "runtime").snapshot_on_upgrade, true),
    },
    routes: routeTable.routes,
    aliases: routeTable.aliases,
    surfaces: surfaceModes(
      schema,
      ["$defs", "ui", "properties", "surfaces", "properties"],
      table(ui, "surfaces"),
      surfaceDefault,
    ),
    extensions: {
      enabled: pick(extensions.enabled, true),
      install_policy: pick(extensions.install_policy, "reject_known_incompatible"),
      allow_in_private: pick(extensions.allow_in_private, "user"),
      install: {
        unpacked: pick(install.unpacked, true),
        crx: pick(install.crx, true),
        update_urls: pick(install.update_urls, "any"),
        stores: pick(install.stores, []),
      },
      ui: surfaceModes(
        schema,
        ["$defs", "extensions", "properties", "ui", "properties"],
        table(extensions, "ui"),
        surfaceDefault,
      ),
      bundled: {},
    },
    services: {
      translation: {
        provider: pick(translation.provider, "default"),
        ...optional(translation, "endpoint"),
        models: pick(translation.models, "on-demand"),
        bundled_languages: pick(translation.bundled_languages, []),
      },
      reputation: {
        provider: pick(reputation.provider, "default"),
        ...optional(reputation, "endpoint"),
        update_interval: pick(reputation.update_interval, "1h"),
      },
      geolocation: {
        provider: pick(geolocation.provider, "system"),
        ...optional(geolocation, "endpoint"),
      },
      credentials: { provider: "chromium" },
      leak_check: { provider: "none" },
    },
    settings_defaults: {},
    automation: {
      enabled: automationEnabled,
      scripts: sortedObject(scripts),
      external: { enabled: pick(table(automation, "external").enabled, false) },
    },
    devtools: {
      frontend:
        frontend === "default" || frontend === "disabled"
          ? frontend
          : normaliseProjectPath(String(frontend)),
      ...(devtools.overlay === undefined
        ? {}
        : { overlay: normaliseProjectPath(String(devtools.overlay)) }),
      protocol: pick(devtools.protocol, true),
    },
    diagnostics: { crash_reports: "local", log_level: "warning" },
    chromium_features: { enable: [], disable: [] },
  };
}

/** Digest of the configuration: the resolved object serialised without `config_digest`. */
export function configDigest(resolvedWithoutDigest: Record<string, unknown>): string {
  return sha256(serialiseResolved(resolvedWithoutDigest));
}

/** The exact bytes of `bdk.resolved.json`: two-space JSON, keys in the documented order, final newline. */
export function serialiseResolved(resolved: Record<string, unknown>): string {
  return `${JSON.stringify(resolved, null, 2)}\n`;
}
