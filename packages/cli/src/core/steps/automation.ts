import { mkdir, readFile, writeFile } from "node:fs/promises";
import { join, resolve } from "node:path";
import { type Bundler, bundleScript } from "../automation/bundler.js";
import {
  type Cache,
  digestInputs,
  isUpToDate,
  readCache,
  writeCache,
} from "../automation/cache.js";
import { automationAliases } from "../automation/locate.js";
import { sha256 } from "../config/digest.js";
import { serialiseResolved } from "../config/index.js";
import { configDigest } from "../config/resolve.js";
import type { Diagnostic } from "../diagnostics.js";
import { hasErrors } from "../diagnostics-format.js";
import { CliError, ExitCode, usageError } from "../exit-codes.js";
import { automationDirectory } from "../pages/routes.js";
import { RESOLVED_FILE } from "./config.js";
import type { Step, StepInput, StepResult } from "./step.js";

/** Inside the application bundle's `Contents/Resources`. */
const BUNDLE_DIRECTORY = "BDK/Automation";

type Entry = Record<string, unknown> & { path: string; digest: string };

/**
 * `bdk build automation`: bundle each script the resolved configuration lists into one ES module with
 * no imports inside the application bundle, and record its path and digest in `bdk.resolved.json`
 * (sdk/cli.md section 5, javascript-api.md section 5.1). Run `bdk build config` first.
 */
export const automationStep: Step = {
  name: "automation",
  flags: ["bundle", "out"],
  watchDirectory: (project) => automationDirectory(project.configFile),
  // Without a bundle there is nothing to write into: a plain `bdk build` leaves the step out.
  applies: ({ ctx, args }) => (args.bundle ?? ctx.env.CODESIGNING_FOLDER_PATH) !== undefined,
  async run({ project, ctx, args }): Promise<StepResult> {
    const resolvedFile =
      args.out === undefined ? resolve(project.dir, RESOLVED_FILE) : resolve(ctx.cwd, args.out);
    const resolved = await readResolved(resolvedFile);
    const automation = asRecord(resolved.automation);
    const scripts = asRecord(automation.scripts) as Record<string, Entry>;
    const names = Object.keys(scripts);
    if (automation.enabled !== true || names.length === 0) {
      const reason =
        automation.enabled === true ? "there are no scripts" : "[automation] enabled is false";
      if (!args.json) ctx.stdout(`bdk build automation: nothing to do, ${reason}.\n`);
      return { diagnostics: [], exitCode: ExitCode.Success, data: { built: [], skipped: reason } };
    }

    const bundle = args.bundle ?? ctx.env.CODESIGNING_FOLDER_PATH;
    if (bundle === undefined || bundle === "") {
      throw usageError(
        "`bdk build automation` needs the application bundle.",
        "Pass --bundle <App.app>. Xcode build phases set CODESIGNING_FOLDER_PATH.",
      );
    }
    const outDir = join(resolve(ctx.cwd, bundle), "Contents", "Resources", BUNDLE_DIRECTORY);

    const cache = readCache(project.dir);
    const diagnostics: Diagnostic[] = [];
    const built: string[] = [];
    const unchanged: string[] = [];
    const digests = new Map<string, string>();
    let bundler: Bundler | undefined;
    let aliases: Map<string, string> | undefined;

    for (const name of names) {
      const entry = scripts[name] as Entry;
      const output = join(outDir, `${name}.js`);
      const entryFile = resolve(project.dir, entry.path);
      const cached = cache.get(name);
      if (isUpToDate(cached, entryFile, output)) {
        unchanged.push(name);
        digests.set(name, (cached as { bundle_digest: string }).bundle_digest);
        continue;
      }
      bundler ??= await requireBundler(ctx, project.dir);
      aliases ??= automationAliases(project.dir);
      const outcome = await bundleScript(bundler, { script: name, entry: entryFile, aliases });
      diagnostics.push(...outcome.diagnostics);
      if (outcome.code === undefined) continue;
      const bytes = Buffer.from(outcome.code, "utf8");
      await mkdir(outDir, { recursive: true });
      await writeFile(output, bytes);
      const bundleDigest = sha256(bytes);
      cache.set(name, {
        entry: entryFile,
        output,
        bundle_digest: bundleDigest,
        inputs: digestInputs(outcome.inputs),
      });
      built.push(name);
      digests.set(name, bundleDigest);
    }
    await writeCache(project.dir, pruned(cache, names));

    if (hasErrors(diagnostics)) {
      return { diagnostics, exitCode: ExitCode.ProjectFailure, data: { built, unchanged } };
    }
    const updated = record(resolved, scripts, digests);
    if (updated !== undefined) await writeFile(resolvedFile, serialiseResolved(updated));
    if (!args.json) {
      ctx.stdout(
        `bdk build automation: bundled ${built.length} script(s), ${unchanged.length} unchanged.\n`,
      );
    }
    return { diagnostics, exitCode: ExitCode.Success, data: { built, unchanged } };
  },
};

async function requireBundler(ctx: StepInput["ctx"], projectDir: string): Promise<Bundler> {
  const bundler = await ctx.loadBundler(projectDir);
  if (bundler !== undefined) return bundler;
  throw new CliError(
    {
      code: "automation.bundler_missing",
      severity: "error",
      message:
        "Rolldown is not installed in the project, so the automation scripts cannot be bundled.",
      fix: "Add rolldown@1.2.18 to the project's devDependencies and install it.",
    },
    ExitCode.Environment,
  );
}

async function readResolved(file: string): Promise<Record<string, unknown>> {
  const fail = (message: string): CliError =>
    new CliError(
      {
        code: "automation.resolved_missing",
        severity: "error",
        message,
        file,
        fix: "Run `bdk build config` first; it writes bdk.resolved.json.",
      },
      ExitCode.ProjectFailure,
    );
  let text: string;
  try {
    text = await readFile(file, "utf8");
  } catch {
    throw fail(`There is no ${RESOLVED_FILE} to read the automation scripts from.`);
  }
  try {
    return asRecord(JSON.parse(text));
  } catch {
    throw fail(`${RESOLVED_FILE} is not valid JSON.`);
  }
}

function asRecord(value: unknown): Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value)
    ? (value as Record<string, unknown>)
    : {};
}

function pruned(cache: Cache, names: readonly string[]): Cache {
  return new Map([...cache].filter(([name]) => names.includes(name)));
}

/**
 * The resolved configuration with `bundle` and `bundle_digest` on each script and `config_digest`
 * recomputed over everything else, or `undefined` when nothing changes.
 */
function record(
  resolved: Record<string, unknown>,
  scripts: Record<string, Entry>,
  digests: ReadonlyMap<string, string>,
): Record<string, unknown> | undefined {
  // fromEntries defines own properties, so a script called `__proto__` stays a key.
  const next: Record<string, Entry> = Object.fromEntries(
    Object.entries(scripts).map(([name, entry]) => {
      const { bundle: _bundle, bundle_digest: _digest, ...source } = entry;
      return [
        name,
        {
          ...source,
          bundle: `${BUNDLE_DIRECTORY}/${name}.js`,
          bundle_digest: digests.get(name) as string,
        },
      ];
    }),
  );
  const { config_digest: previous, ...body } = resolved;
  const withBundles = {
    ...body,
    automation: { ...asRecord(body.automation), scripts: next },
  };
  const digest = configDigest(withBundles);
  if (digest === previous) return undefined;
  // `config_digest` stays where the resolver put it.
  return Object.fromEntries(
    Object.keys(resolved).map((key) => [
      key,
      key === "config_digest" ? digest : (withBundles as Record<string, unknown>)[key],
    ]),
  );
}
