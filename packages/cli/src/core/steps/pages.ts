import { mkdir, mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { basename, dirname, join, resolve } from "node:path";
import { INTERNAL_MODULES_PREFIX } from "../../asset-sources.js";
import type { Diagnostic } from "../diagnostics.js";
import { hasErrors } from "../diagnostics-format.js";
import { CliError, ExitCode, usageError } from "../exit-codes.js";
import { checkPageFiles, collectFiles, type PageFile, writeFiles } from "../pages/files.js";
import {
  type BundledRoute,
  bundleFolder,
  pagesDirectory,
  readBundledRoutes,
} from "../pages/routes.js";
import { buildPage, findViteConfig, loadVite } from "../pages/vite.js";
import { RESOLVED_FILE } from "./config.js";
import type { Step, StepResult } from "./step.js";

/** Where the runtime serves the SDK from, inside the application's resources (design: page bundle layout). */
export const SDK_DIRECTORY = "BDK/js/internal";

/** `bdk build pages`: package the bundled pages and the SDK into the application's resources (sdk/cli.md section 5). */
export const pagesStep: Step = {
  name: "pages",
  flags: ["bundle", "resolved"],
  applies: ({ ctx, args }) => (args.bundle ?? bundleFolder(ctx.env)) !== undefined,
  async run({ project, ctx, args }): Promise<StepResult> {
    const bundle = args.bundle ?? bundleFolder(ctx.env);
    if (bundle === undefined || bundle === "") {
      throw usageError(
        "`bdk build pages` needs the application bundle.",
        "Pass --bundle <App.app>. Xcode build phases set CODESIGNING_FOLDER_PATH.",
      );
    }
    const resources = join(resolve(ctx.cwd, bundle), "Contents", "Resources");
    const resolvedFile =
      args.resolved === undefined
        ? join(project.dir, RESOLVED_FILE)
        : resolve(ctx.cwd, args.resolved);
    const routes = await readBundledRoutes(resolvedFile);
    if (routes.length === 0) {
      return { diagnostics: [], exitCode: ExitCode.Success, data: { pages: [], sdkModules: 0 } };
    }

    const sdk = ctx.assets.list(INTERNAL_MODULES_PREFIX);
    if (sdk.length === 0) {
      throw new CliError(
        {
          code: "cli.asset_missing",
          severity: "error",
          message: "This bdk has no embedded @bdk/internal modules to copy into the application.",
          fix: "Reinstall bdk from a release, or build the modules with `pnpm exec nx run js-internal:build` and rebuild bdk with `pnpm cli:package`.",
        },
        ExitCode.Environment,
      );
    }

    const pagesRoot = join(project.dir, await pagesDirectory(project.configFile));
    const viteConfig = findViteConfig(pagesRoot);
    const work = await mkdtemp(join(tmpdir(), "bdk-pages-"));
    try {
      const build = viteConfig === undefined ? undefined : loadVite(pagesRoot, viteConfig);
      const pages: { route: BundledRoute; source: string; files: PageFile[] }[] = [];
      for (const route of routes) {
        const source = join(project.dir, route.path);
        // Vite builds a page that is a direct child of the pages directory. An override elsewhere is copied.
        if (build !== undefined && viteConfig !== undefined && dirname(source) === pagesRoot) {
          const outDir = join(work, route.route);
          await buildPage(build, {
            pagesDir: pagesRoot,
            configFile: viteConfig,
            name: basename(source),
            outDir,
          });
          pages.push({ route, source, files: await collectFiles(outDir) });
        } else {
          pages.push({ route, source, files: await collectFiles(source) });
        }
      }

      const diagnostics: Diagnostic[] = [];
      for (const page of pages) {
        diagnostics.push(
          // A built page is named by where its file comes from in the source: the page directory.
          ...(await checkPageFiles(page.files, (file) => join(page.source, file.relative))),
        );
      }
      if (hasErrors(diagnostics)) {
        return { diagnostics, exitCode: ExitCode.ProjectFailure, data: { wrote: false } };
      }

      for (const page of pages) await writeFiles(join(resources, page.route.path), page.files);
      const sdkDir = join(resources, SDK_DIRECTORY);
      await rm(sdkDir, { recursive: true, force: true });
      await mkdir(sdkDir, { recursive: true });
      for (const name of sdk) {
        await writeFile(
          join(sdkDir, name.slice(INTERNAL_MODULES_PREFIX.length)),
          ctx.assets.readText(name),
        );
      }

      return {
        diagnostics,
        exitCode: ExitCode.Success,
        data: {
          wrote: true,
          bundle: resolve(ctx.cwd, bundle),
          pages: pages.map((p) => ({
            route: p.route.route,
            path: p.route.path,
            files: p.files.length,
          })),
          sdkModules: sdk.length,
        },
      };
    } finally {
      await rm(work, { recursive: true, force: true });
    }
  },
};
