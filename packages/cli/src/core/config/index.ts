import type { Diagnostic } from "../diagnostics.js";

// PLACEHOLDER from T-CLI-05: the configuration module (T-CFG-01/02) replaces this whole file at merge.
// It exists only so the CLI foundation typechecks, tests and packages on its own.

export interface BundleFacts {
  infoPlist?: Record<string, unknown>;
  entitlements?: Record<string, unknown>;
}

export interface ResolveOptions {
  projectDir: string;
  schema: unknown;
  bundleFacts?: BundleFacts;
}

export interface ResolveResult {
  diagnostics: Diagnostic[];
  resolved?: Record<string, unknown>;
}

export function resolveConfig(_options: ResolveOptions): Promise<ResolveResult> {
  return Promise.resolve({
    diagnostics: [
      {
        code: "config.unavailable",
        severity: "error",
        message: "The configuration resolver is not part of this build of bdk.",
        fix: "Use a bdk release that includes the configuration resolver.",
      },
    ],
  });
}
