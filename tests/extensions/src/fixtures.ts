import { existsSync, readdirSync, readFileSync, statSync } from "node:fs";
import { dirname, join } from "node:path";

/** Directory prefixes owned by the engine tests, which keep their own fixtures next to ours. */
export const ENGINE_FIXTURE_PREFIXES = ["install-", "ui-", "panel-"] as const;

type Json = Record<string, unknown>;

const asList = (value: unknown): unknown[] => (Array.isArray(value) ? value : []);
const asObject = (value: unknown): Json =>
  typeof value === "object" && value !== null && !Array.isArray(value) ? (value as Json) : {};

/** Every file a manifest names, relative to the extension directory. */
export function referencedFiles(manifest: Json): string[] {
  const files: unknown[] = [];
  files.push(asObject(manifest.background).service_worker);
  files.push(manifest.options_page, manifest.devtools_page);
  files.push(asObject(manifest.options_ui).page);
  files.push(asObject(manifest.action).default_popup, asObject(manifest.side_panel).default_path);
  for (const script of asList(manifest.content_scripts)) {
    files.push(...asList(asObject(script).js), ...asList(asObject(script).css));
  }
  for (const ruleset of asList(asObject(manifest.declarative_net_request).rule_resources)) {
    files.push(asObject(ruleset).path);
  }
  const icons = [manifest.icons, asObject(manifest.action).default_icon];
  for (const set of icons) files.push(...Object.values(asObject(set)));
  files.push(...Object.values(asObject(manifest.chrome_url_overrides)));
  return files.filter((file): file is string => typeof file === "string" && file !== "");
}

/** Local scripts an HTML page loads with `<script src>`. */
export function scriptsOf(html: string): string[] {
  return [...html.matchAll(/<script[^>]*\ssrc="([^"]+)"/g)]
    .map((match) => match[1] ?? "")
    .filter((src) => !/^[a-z]+:|^\//.test(src));
}

/** Problems with one extension directory, one message each. Empty when it is a loadable Manifest V3 fixture. */
export function validateFixture(dir: string): string[] {
  const manifestPath = join(dir, "manifest.json");
  if (!existsSync(manifestPath)) return [`${dir}: no manifest.json`];
  let manifest: Json;
  try {
    manifest = JSON.parse(readFileSync(manifestPath, "utf8")) as Json;
  } catch (error) {
    return [`${manifestPath}: not valid JSON (${String(error)})`];
  }
  const issues: string[] = [];
  if (manifest.manifest_version !== 3) issues.push(`${manifestPath}: manifest_version must be 3`);
  for (const key of ["name", "version"]) {
    if (typeof manifest[key] !== "string" || manifest[key] === "")
      issues.push(`${manifestPath}: no ${key}`);
  }
  for (const file of referencedFiles(manifest)) {
    const path = join(dir, file);
    if (!existsSync(path)) {
      issues.push(`${manifestPath}: refers to ${file}, which does not exist`);
    } else if (file.endsWith(".html")) {
      for (const script of scriptsOf(readFileSync(path, "utf8"))) {
        if (!existsSync(join(dirname(path), script)))
          issues.push(`${path}: loads ${script}, which does not exist`);
      }
    }
  }
  return issues;
}

/** Our fixtures: the probe and every directory of `fixtures/` the engine tests do not own. */
export function suiteFixtures(suiteRoot: string): string[] {
  const dirs = [join(suiteRoot, "probe")];
  const fixtures = join(suiteRoot, "fixtures");
  for (const name of readdirSync(fixtures).sort()) {
    const dir = join(fixtures, name);
    if (!statSync(dir).isDirectory()) continue;
    if (ENGINE_FIXTURE_PREFIXES.some((prefix) => name.startsWith(prefix))) continue;
    dirs.push(dir);
  }
  return dirs;
}
