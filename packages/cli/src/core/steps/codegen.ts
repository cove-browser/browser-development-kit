import { mkdir, readFile, rm, stat, writeFile } from "node:fs/promises";
import { dirname, join, resolve } from "node:path";
import {
  APP_DECLARATIONS_PATH,
  APP_ENVIRONMENT_PATH,
  COMMANDS_FILE,
  generateApplication,
  InterfaceError,
  isStale,
} from "@bdk/codegen";
import { SDK_PREFIX } from "../../asset-sources.js";
import { linkScope } from "../codegen/links.js";
import {
  aliasFiles,
  outputPath,
  packageJson,
  SCOPE_DIRECTORY,
  SDK_DIGEST_FILE,
  sdkDigest,
  sdkPackages,
} from "../codegen/packages.js";
import type { Diagnostic } from "../diagnostics.js";
import { hasErrors } from "../diagnostics-format.js";
import { CliError, ExitCode } from "../exit-codes.js";
import { automationDirectory, pagesDirectory } from "../pages/routes.js";
import { RESOLVED_FILE } from "./config.js";
import type { Step, StepResult } from "./step.js";

/** What stands in for the declarations when the application has none (`bdk.commands.json`, format version 1). */
const NO_DECLARATIONS = '{"version":1}';

/** Where a build outside a host keeps generated files, relative to the project. */
const DEFAULT_OUT = join(".bdk", "js");
const DEFAULT_COMMANDS = join(".bdk", COMMANDS_FILE);

async function readOptional(file: string): Promise<string | undefined> {
  try {
    return await readFile(file, "utf8");
  } catch (error) {
    if (typeof error === "object" && error !== null && "code" in error && error.code === "ENOENT") {
      return undefined;
    }
    throw error;
  }
}

async function isDirectory(path: string): Promise<boolean> {
  try {
    return (await stat(path)).isDirectory();
  } catch {
    return false;
  }
}

/** Turns what the generator found wrong in its two inputs into diagnostics at the files that hold them. */
function issueDiagnostics(error: InterfaceError, files: { commands: string; resolved: string }) {
  return error.issues.map((issue): Diagnostic => {
    const declarations = issue.file === COMMANDS_FILE;
    return declarations
      ? {
          code: "codegen.declarations_invalid",
          severity: "error",
          message: `${issue.path}: ${issue.message}`,
          file: files.commands,
          fix: "Fix the declaration in Swift and write bdk.commands.json again (BDKDeclarations.write(to:)).",
        }
      : {
          code: "codegen.resolved_invalid",
          severity: "error",
          message: `${issue.path}: ${issue.message}`,
          file: files.resolved,
          fix: "Set `internal_scheme` under `[browser]` in bdk.toml where it is missing, then run `bdk build config`.",
        };
  });
}

/**
 * `bdk build codegen`: write the `@bdk` packages (the SDK, its types, the Vite preset) and the application's
 * own declarations where editors and `tsc` find them, and link them into the pages directory
 * (sdk/cli.md section 5, javascript-api.md section 7). Does nothing when the output already records the digest
 * of every input.
 */
export const codegenStep: Step = {
  name: "codegen",
  flags: ["out", "commands", "resolved"],
  async run({ project, ctx, args }): Promise<StepResult> {
    const directories = [
      await pagesDirectory(project.configFile),
      await automationDirectory(project.configFile),
    ].map((directory) => join(project.dir, directory));
    const present: string[] = [];
    for (const directory of directories) if (await isDirectory(directory)) present.push(directory);
    if (present.length === 0) {
      return {
        diagnostics: [],
        exitCode: ExitCode.Success,
        data: { generated: false, reason: "no Pages or Automation directory" },
      };
    }

    const locations = ctx.platform.buildLocations(ctx.env);
    const resolvedFile =
      args.resolved === undefined
        ? join(project.dir, RESOLVED_FILE)
        : resolve(ctx.cwd, args.resolved);
    const resolvedText = await readOptional(resolvedFile);
    if (resolvedText === undefined) {
      throw new CliError(
        {
          code: "codegen.resolved_missing",
          severity: "error",
          message: `${resolvedFile} does not exist, so the routes and the internal scheme are not known.`,
          file: resolvedFile,
          fix: "Run `bdk build config` first, or pass --resolved <file>.",
        },
        ExitCode.ProjectFailure,
      );
    }
    const commandsFile = resolve(
      ctx.cwd,
      args.commands ?? locations?.commandsFile ?? join(project.dir, DEFAULT_COMMANDS),
    );
    const commandsText = await readOptional(commandsFile);
    const diagnostics: Diagnostic[] = [];
    if (commandsText === undefined) {
      diagnostics.push({
        code: "codegen.no_declarations",
        severity: "note",
        message: `${commandsFile} does not exist, so no app. commands, events or settings are declared to pages.`,
        file: commandsFile,
        fix: "An application with no commands needs nothing. Otherwise write the file with BDKDeclarations.write(to:) and pass it with --commands.",
      });
    }
    const declarations = commandsText ?? NO_DECLARATIONS;
    const out = resolve(ctx.cwd, args.out ?? locations?.jsOut ?? join(project.dir, DEFAULT_OUT));

    const names = ctx.assets.list(SDK_PREFIX);
    const packages = sdkPackages(names);
    if (packages.length === 0) {
      throw new CliError(
        {
          code: "cli.asset_missing",
          severity: "error",
          message: "This bdk has no embedded @bdk packages to write for pages.",
          fix: "Reinstall bdk from a release, or build them with `pnpm exec nx run-many -t build -p js-protocol,js-internal,js-vite` and rebuild bdk with `pnpm cli:package`.",
        },
        ExitCode.Environment,
      );
    }
    const aliases = aliasFiles(packages, ctx.version);
    const files = new Map(names.map((name) => [name, ctx.assets.readText(name)]));
    const digest = sdkDigest(files);

    const [appFile, envFile, recordedDigest] = await Promise.all([
      readOptional(join(out, APP_DECLARATIONS_PATH)),
      readOptional(join(out, APP_ENVIRONMENT_PATH)),
      readOptional(join(out, SDK_DIGEST_FILE)),
    ]);
    const upToDate =
      appFile !== undefined &&
      envFile !== undefined &&
      recordedDigest === digest &&
      !isStale(appFile, declarations) &&
      !isStale(envFile, resolvedText);

    if (!upToDate) {
      let generated: Record<string, string>;
      try {
        generated = generateApplication({ commands: declarations, resolved: resolvedText });
      } catch (error) {
        if (!(error instanceof InterfaceError)) throw error;
        return {
          diagnostics: [
            ...diagnostics,
            ...issueDiagnostics(error, { commands: commandsFile, resolved: resolvedFile }),
          ],
          exitCode: ExitCode.ProjectFailure,
          data: { generated: false },
        };
      }
      await rm(join(out, SCOPE_DIRECTORY), { recursive: true, force: true });
      const write = async (path: string, text: string) => {
        await mkdir(dirname(join(out, path)), { recursive: true });
        await writeFile(join(out, path), text);
      };
      for (const [name, text] of files) await write(outputPath(name), text);
      for (const pkg of packages) {
        await write(
          join(SCOPE_DIRECTORY, pkg.name.slice(SCOPE_DIRECTORY.length + 1), "package.json"),
          packageJson(pkg, ctx.version),
        );
      }
      for (const [path, text] of Object.entries(generated)) await write(path, text);
      for (const [path, text] of aliases) await write(path, text);
      await write(SDK_DIGEST_FILE, digest);
    }

    // TypeScript and Node resolve a link to its real path, so `@bdk/internal` finds `@bdk/protocol` only if
    // the output is itself a project with `node_modules/@bdk` beside the packages.
    const self = await linkScope(out, join(out, SCOPE_DIRECTORY));
    if (self.diagnostic !== undefined) diagnostics.push(self.diagnostic);
    const links: string[] = [];
    for (const directory of present) {
      const result = await linkScope(directory, join(out, SCOPE_DIRECTORY));
      if (result.diagnostic !== undefined) diagnostics.push(result.diagnostic);
      else links.push(result.link);
    }
    return {
      diagnostics,
      exitCode: hasErrors(diagnostics) ? ExitCode.ProjectFailure : ExitCode.Success,
      data: {
        generated: !upToDate,
        out,
        declarations: commandsText === undefined ? null : commandsFile,
        packages: [
          ...packages.map((p) => p.name),
          ...[...aliases.keys()]
            .filter((path) => path.endsWith("package.json"))
            .map((path) => dirname(path)),
        ],
        links,
      },
    };
  },
};
