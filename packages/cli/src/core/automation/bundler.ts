import { existsSync } from "node:fs";
import { isBuiltin } from "node:module";
import type { Diagnostic } from "../diagnostics.js";

/** The slice of Rolldown's plugin context that the bundle step uses. */
export interface ResolveContext {
  resolve(
    source: string,
    importer: string | undefined,
    options: { skipSelf: boolean },
  ): Promise<{ id: string } | null>;
}

/** The slice of Rolldown's API that the bundle step uses, so tests can supply a fake. */
export interface Bundler {
  rolldown(options: {
    input: string;
    platform: "browser";
    logLevel: "warn";
    plugins: [
      {
        name: string;
        resolveId(
          this: ResolveContext,
          source: string,
          importer: string | undefined,
        ): Promise<{ id: string; external?: boolean } | null>;
        transform(code: string, id: string): null;
      },
    ];
  }): Promise<{
    generate(options: { format: "esm"; minify: false; codeSplitting: false }): Promise<{
      output: readonly { type: string; code?: string }[];
    }>;
    close(): Promise<void>;
  }>;
}

export interface BundleRequest {
  /** The script's name, for messages. */
  script: string;
  /** Absolute path of the script. */
  entry: string;
  /** Specifier to absolute file, for the packages the host provides no module for (`@bdk/*`). */
  aliases: ReadonlyMap<string, string>;
}

export interface BundleOutcome {
  diagnostics: Diagnostic[];
  /** The ES module text, when there is no error. */
  code?: string;
  /** Every source file that went into the bundle: the incremental build's input list. */
  inputs: string[];
}

/**
 * Bundles one script into a single ES module with no imports. The automation host provides no
 * modules, so an import is either inlined or an error: `@bdk/*` comes from the aliases, a Node
 * built-in is refused (the host has no Node APIs), and anything that does not resolve is reported
 * with the script and the specifier instead of being left for the host to fail on.
 */
export async function bundleScript(
  bundler: Bundler,
  request: BundleRequest,
): Promise<BundleOutcome> {
  const diagnostics: Diagnostic[] = [];
  const inputs = new Set<string>();
  const reported = new Set<string>();
  const report = (diagnostic: Diagnostic): void => {
    const key = `${diagnostic.code}\0${diagnostic.message}`;
    if (reported.has(key)) return;
    reported.add(key);
    diagnostics.push(diagnostic);
  };
  const { script, entry, aliases } = request;
  try {
    const build = await bundler.rolldown({
      input: entry,
      platform: "browser",
      logLevel: "warn",
      plugins: [
        {
          name: "bdk-automation",
          async resolveId(source, importer) {
            if (importer === undefined) return null;
            const alias = aliases.get(source);
            if (alias !== undefined) return { id: alias };
            if (isBuiltin(source)) {
              report({
                code: "automation.import_not_allowed",
                severity: "error",
                message: `The script "${script}" imports "${source}", a Node built-in, and the automation host has no Node APIs.`,
                file: importer,
                fix: "Use a @bdk/automation command instead, or move the work into the application.",
              });
              return { id: source, external: true };
            }
            const resolved = await this.resolve(source, importer, { skipSelf: true });
            if (resolved === null) {
              report({
                code: "automation.import_unresolved",
                severity: "error",
                message: `The script "${script}" imports "${source}", which cannot be resolved from the project.`,
                file: importer,
                fix: "Install the package in the project, fix the path, or import from @bdk/automation.",
              });
              return { id: source, external: true };
            }
            return resolved;
          },
          transform(_code, id) {
            const file = id.split("?")[0] as string;
            if (existsSync(file)) inputs.add(file);
            return null;
          },
        },
      ],
    });
    try {
      const { output } = await build.generate({
        format: "esm",
        minify: false,
        codeSplitting: false,
      });
      const chunks = output.filter((o) => o.type === "chunk");
      if (diagnostics.length === 0 && (chunks.length !== 1 || chunks[0]?.code === undefined)) {
        report(bundleFailed(script, `Rolldown produced ${chunks.length} files, not one.`));
      }
      return {
        diagnostics,
        inputs: [...inputs].sort(),
        ...(diagnostics.length === 0 ? { code: chunks[0]?.code as string } : {}),
      };
    } finally {
      await build.close();
    }
  } catch (error) {
    // An import problem already reported is the cause; Rolldown's own failure comes after it.
    if (diagnostics.length === 0) {
      report(bundleFailed(script, error instanceof Error ? error.message : String(error)));
    }
    return { diagnostics, inputs: [...inputs].sort() };
  }
}

function bundleFailed(script: string, message: string): Diagnostic {
  return {
    code: "automation.bundle_failed",
    severity: "error",
    message: `Rolldown could not bundle the script "${script}": ${message}`,
    fix: "Fix the error above in the script or a module it imports.",
  };
}
