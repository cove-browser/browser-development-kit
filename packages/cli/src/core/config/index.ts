/**
 * The configuration core of the CLI: validates `bdk.toml` and resolves it, with the project's
 * `Pages/` and `Automation/` directories, into `bdk.resolved.json` (configuration.md sections 7 and 9).
 * Platform neutral: it reads files with `node:fs` and nothing else. The caller supplies facts
 * about `Info.plist` and the entitlements, already parsed.
 */
import { existsSync, readFileSync } from "node:fs";
import { join } from "node:path";
import type { Diagnostic } from "../diagnostics.js";
import { Reporter, sortDiagnostics } from "./reporter.js";
import {
  assemble,
  configDigest,
  DEFAULT_BDK_VERSION,
  serialiseResolved as serialise,
} from "./resolve.js";
import { CONFIG_FILE, parseConfig } from "./validate.js";

export interface ResolveOptions {
  projectDir: string;
  schema: unknown;
  bundleFacts?: BundleFacts;
  /** The version of BDK the application uses (the Swift package version). Defaults to "0.0.0". */
  bdkVersion?: string;
}

export interface BundleFacts {
  infoPlist?: Record<string, unknown>;
  entitlements?: Record<string, unknown>;
}

export interface ResolveResult {
  diagnostics: Diagnostic[];
  resolved?: Record<string, unknown>;
}

/** Validates and resolves the configuration of the project in `options.projectDir`. */
export async function resolveConfig(options: ResolveOptions): Promise<ResolveResult> {
  const { projectDir, schema } = options;
  const file = join(projectDir, CONFIG_FILE);
  if (!existsSync(file)) {
    return {
      diagnostics: [
        {
          code: "config.missing",
          severity: "error",
          message: `There is no ${CONFIG_FILE} in the project directory.`,
          file: CONFIG_FILE,
          fix: "Run `bdk init`, or create bdk.toml with a [browser] table that sets name.",
        },
      ],
    };
  }
  const outcome = parseConfig(readFileSync(file, "utf8"), schema as object);
  if (!outcome.config) return { diagnostics: outcome.diagnostics };

  const { doc, locator } = outcome.config;
  const reporter = new Reporter(CONFIG_FILE, locator);
  reporter.diagnostics.push(...outcome.diagnostics);
  const body = assemble({
    doc,
    projectDir,
    schema,
    bundleFacts: options.bundleFacts,
    bdkVersion: options.bdkVersion ?? DEFAULT_BDK_VERSION,
    reporter,
    schemeKey:
      doc.browser && "internal_scheme" in (doc.browser as object)
        ? ["browser", "internal_scheme"]
        : ["browser", "name"],
  });
  const diagnostics = sortDiagnostics(reporter.diagnostics);
  if (!body || reporter.hasErrors) return { diagnostics };

  const { schema: version, bdk_version, ...rest } = body;
  const resolved = { schema: version, bdk_version, config_digest: configDigest(body), ...rest };
  return { diagnostics, resolved };
}

/** The exact bytes to write to `bdk.resolved.json`. */
export function serialiseResolved(resolved: Record<string, unknown>): string {
  return serialise(resolved);
}

/** The web app and push diagnostics alone, for `bdk doctor` (T-CLI-01). */
export { checkWebAppsAndPush } from "./resolve.js";
export { validateConfigText } from "./validate.js";
