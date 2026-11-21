import { existsSync } from "node:fs";
import { createRequire } from "node:module";
import { join } from "node:path";
import { PAGE_ENV } from "@bdk/vite/tooling";
import { CliError, ExitCode } from "../exit-codes.js";

/** The configuration file names Vite looks for, in the order it looks. */
const CONFIG_NAMES = ["vite.config.ts", "vite.config.mts", "vite.config.js", "vite.config.mjs"];

export function findViteConfig(pagesDir: string): string | undefined {
  return CONFIG_NAMES.map((name) => join(pagesDir, name)).find((file) => existsSync(file));
}

/** Where Node finds `vite` for code in the pages directory: its own `node_modules`, then each parent's. */
function resolveFromProject(pagesDir: string): string {
  return createRequire(join(pagesDir, "package.json")).resolve("vite");
}

/**
 * Loads the module at `entry`. `require`, not `import()`: Vite is an ES module, which Node's `require` loads,
 * and inside the single executable `import()` of a file is not available (only built-in modules are).
 */
function loadModule(entry: string): unknown {
  return createRequire(entry)(entry);
}

type BuildFunction = (config: {
  root: string;
  configFile: string;
  logLevel: "warn";
  build: { outDir: string; emptyOutDir: true };
}) => Promise<unknown>;

/**
 * The project's own Vite, loaded from its `node_modules` and run in this process with the Node that runs
 * the CLI, so a second Node is not needed (sdk/cli.md section 2.3).
 */
export function loadVite(
  pagesDir: string,
  configFile: string,
  resolveVite: (pagesDir: string) => string = resolveFromProject,
): BuildFunction {
  let entry: string;
  try {
    entry = resolveVite(pagesDir);
  } catch {
    throw new CliError(
      {
        code: "pages.vite_missing",
        severity: "error",
        message: `${configFile} configures Vite, but Vite is not installed for ${pagesDir}.`,
        file: configFile,
        fix: `Run your package manager's install (for example \`npm install\`) in ${pagesDir}.`,
      },
      ExitCode.ProjectFailure,
    );
  }
  const loaded = loadModule(entry);
  if (typeof loaded === "object" && loaded !== null && "build" in loaded) {
    if (typeof loaded.build === "function") return loaded.build as BuildFunction;
  }
  throw new CliError(
    {
      code: "pages.vite_missing",
      severity: "error",
      message: `${entry} does not export a build function, so it is not a Vite that bdk can run.`,
      file: configFile,
      fix: "Install a current Vite in the pages directory.",
    },
    ExitCode.ProjectFailure,
  );
}

/**
 * Builds the page directory `name` alone into `outDir`, with `@bdk/vite` told which page it is. The output
 * is that page's whole origin: `index.html` and `assets/` at the top. One page at a time, because pages
 * are separate origins and cannot share chunks.
 */
export async function buildPage(
  build: BuildFunction,
  options: { pagesDir: string; configFile: string; name: string; outDir: string },
): Promise<void> {
  const before = process.env[PAGE_ENV];
  process.env[PAGE_ENV] = options.name;
  try {
    await build({
      root: options.pagesDir,
      configFile: options.configFile,
      logLevel: "warn",
      build: { outDir: options.outDir, emptyOutDir: true },
    });
  } catch (error) {
    throw new CliError(
      {
        code: "pages.vite_failed",
        severity: "error",
        message: `Vite failed to build the page "${options.name}": ${error instanceof Error ? error.message : String(error)}`,
        file: options.configFile,
        fix: "Fix what Vite reports above, then run `bdk build pages` again.",
      },
      ExitCode.ProjectFailure,
    );
  } finally {
    if (before === undefined) delete process.env[PAGE_ENV];
    else process.env[PAGE_ENV] = before;
  }
}
