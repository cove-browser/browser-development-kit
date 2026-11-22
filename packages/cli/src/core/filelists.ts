import { stat } from "node:fs/promises";
import { join, resolve } from "node:path";
import { APP_ENVIRONMENT_PATH } from "@bdk/codegen";
import type { FileList } from "../platform/platform.js";
import { SDK_DIGEST_FILE } from "./codegen/packages.js";
import type { Context } from "./context.js";
import { collectFiles } from "./pages/files.js";
import {
  automationDirectory,
  bundleFolder,
  pagesDirectory,
  readBundledRoutes,
} from "./pages/routes.js";
import type { Project } from "./project.js";
import { SDK_DIRECTORY } from "./steps/pages.js";

async function exists(path: string): Promise<boolean> {
  try {
    await stat(path);
    return true;
  } catch {
    return false;
  }
}

/** A project-relative setting such as `INFOPLIST_FILE`, made absolute against `SRCROOT`. */
function setting(env: NodeJS.ProcessEnv, name: string, project: Project): string | undefined {
  const value = env[name];
  return value ? resolve(env.SRCROOT ?? project.dir, value) : undefined;
}

/** Every file a page build can read: the pages directory, and the directory of a route an override moved out of it. */
async function pageInputs(project: Project, resolvedFile: string): Promise<string[]> {
  const root = join(project.dir, await pagesDirectory(project.configFile));
  const directories = [
    root,
    ...(await readBundledRoutes(resolvedFile)).map((r) => join(project.dir, r.path)),
  ];
  const files = new Set<string>();
  for (const directory of directories) {
    if (!(await exists(directory))) continue;
    for (const file of await collectFiles(directory, "", new Set(["node_modules"]))) {
      files.add(file.absolute);
    }
  }
  return [...files].sort();
}

/**
 * The input and output lists of the two build phases of an application (sdk/cli.md section 8), so the host can
 * skip a phase when nothing changed. `runtime` is `config runtime codegen`; `bundle` is `pages embed`. The
 * bundle list is left out when the host names no bundle.
 */
export async function buildFileLists(input: {
  project: Project;
  ctx: Context;
  resolvedFile: string;
  jsOut: string;
  commandsFile: string;
}): Promise<FileList[]> {
  const { project, ctx, resolvedFile } = input;
  const javascript =
    (await exists(join(project.dir, await pagesDirectory(project.configFile)))) ||
    (await exists(join(project.dir, await automationDirectory(project.configFile))));
  const optional = async (path: string | undefined) =>
    path !== undefined && (await exists(path)) ? [path] : [];

  const runtime: FileList = {
    phase: "runtime",
    inputs: [
      project.configFile,
      ...(await optional(setting(ctx.env, "INFOPLIST_FILE", project))),
      ...(await optional(setting(ctx.env, "CODE_SIGN_ENTITLEMENTS", project))),
      ...(await optional(input.commandsFile)),
    ],
    outputs: [
      resolvedFile,
      ...(javascript
        ? [join(input.jsOut, SDK_DIGEST_FILE), join(input.jsOut, APP_ENVIRONMENT_PATH)]
        : []),
    ],
  };
  const lists = [runtime];

  const bundle = bundleFolder(ctx.env);
  if (bundle !== undefined) {
    const resources = join(resolve(ctx.cwd, bundle), "Contents", "Resources");
    const routes = await readBundledRoutes(resolvedFile);
    lists.push({
      phase: "bundle",
      inputs: [project.configFile, resolvedFile, ...(await pageInputs(project, resolvedFile))],
      outputs:
        routes.length === 0
          ? []
          : [...routes.map((r) => join(resources, r.path)), join(resources, SDK_DIRECTORY)],
    });
  }
  return lists;
}
