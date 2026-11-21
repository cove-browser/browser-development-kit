import { existsSync, mkdirSync, readdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { renderHeader } from "./header.js";
import { InterfaceError, type Issue } from "./issue.js";
import {
  EMPTY_REGISTRY,
  parseRegistry,
  REGISTRY_FILE,
  reconcile,
  serializeRegistry,
} from "./registry.js";
import { renderSwiftServices, SWIFT_PATH, SWIFT_REPORT_PATH } from "./swift.js";
import { renderCommandTable, TABLE_PATH } from "./table.js";
import { renderTypescriptModules, TYPESCRIPT_DIR } from "./typescript.js";
import { parsePolicy, type SourceFile, validate } from "./validate.js";

/** Files in the interface directory that are not namespace definitions. */
export const POLICY_FILE = "policy.json";
const RESERVED_FILES: readonly string[] = [REGISTRY_FILE, POLICY_FILE];

export const HEADER_PATH = "include/bdk/bdk_ids.h";
const BASE_HEADER_PATH = "include/bdk/bdk_base.h";

/** Everything the generator reads, as text. `registry` is undefined before the first generation. */
export interface Inputs {
  readonly abiVersion: number;
  readonly definitions: readonly { file: string; text: string }[];
  readonly policy: string;
  readonly registry: string | undefined;
}

export interface Outputs {
  readonly registry: string;
  readonly header: string;
  readonly table: string;
  readonly swift: string;
  /** File name (relative to `TYPESCRIPT_DIR`) to the text of each `@bdk/internal` module. */
  readonly typescript: Readonly<Record<string, string>>;
}

function parseJson(file: string, text: string): { json: unknown; issues: Issue[] } {
  try {
    return { json: JSON.parse(text), issues: [] };
  } catch (error) {
    const detail = error instanceof Error ? error.message : String(error);
    return { json: undefined, issues: [{ file, path: "$", message: `invalid JSON: ${detail}` }] };
  }
}

/** Validates the definitions and works out the registry and header they produce. Pure. */
export function build(inputs: Inputs): Outputs {
  const issues: Issue[] = [];
  const sources: SourceFile[] = [];
  for (const { file, text } of [...inputs.definitions].sort((a, b) => (a.file < b.file ? -1 : 1))) {
    const parsed = parseJson(file, text);
    issues.push(...parsed.issues);
    if (parsed.issues.length === 0) sources.push({ file, json: parsed.json });
  }

  const policyJson = parseJson(POLICY_FILE, inputs.policy);
  issues.push(...policyJson.issues);
  const { policy, issues: policyIssues } =
    policyJson.issues.length === 0
      ? parsePolicy(POLICY_FILE, policyJson.json)
      : { policy: { neverForPages: [], neverForSessions: [] }, issues: [] };
  issues.push(...policyIssues);

  let registry = EMPTY_REGISTRY;
  if (inputs.registry !== undefined) {
    const registryJson = parseJson(REGISTRY_FILE, inputs.registry);
    issues.push(...registryJson.issues);
    if (registryJson.issues.length === 0) {
      const parsed = parseRegistry(registryJson.json);
      issues.push(...parsed.issues);
      registry = parsed.registry;
    }
  }

  const validated = validate(sources, { abiVersion: inputs.abiVersion, policy });
  issues.push(...validated.issues);
  if (issues.length > 0) throw new InterfaceError(issues);

  const reconciled = reconcile(registry, validated.definitions);
  if (reconciled.issues.length > 0) throw new InterfaceError(reconciled.issues);
  return {
    registry: serializeRegistry(reconciled.registry),
    header: renderHeader(validated.definitions, reconciled.registry),
    table: renderCommandTable(validated.definitions, reconciled.registry, policy),
    swift: renderSwiftServices(validated.definitions),
    typescript: renderTypescriptModules(validated.definitions),
  };
}

function readOptional(path: string): string | undefined {
  return existsSync(path) ? readFileSync(path, "utf8") : undefined;
}

/** Reads the interface directory and the ABI version under `coreDir` (`packages/core`). */
export function readInputs(coreDir: string): Inputs {
  const interfaceDir = join(coreDir, "interface");
  const files = readdirSync(interfaceDir)
    .filter((name) => name.endsWith(".json") && !RESERVED_FILES.includes(name))
    .sort();
  const base = readFileSync(join(coreDir, BASE_HEADER_PATH), "utf8");
  const version = /^#define BDK_ABI_VERSION (\d+)\s*$/m.exec(base);
  if (version === null) throw new Error(`${BASE_HEADER_PATH}: BDK_ABI_VERSION is not defined`);
  return {
    abiVersion: Number(version[1]),
    definitions: files.map((file) => ({
      file,
      text: readFileSync(join(interfaceDir, file), "utf8"),
    })),
    policy: readFileSync(join(interfaceDir, POLICY_FILE), "utf8"),
    registry: readOptional(join(interfaceDir, REGISTRY_FILE)),
  };
}

/**
 * The committed outputs as absolute path, text and the path `check` reports it by. They are committed because
 * builds that cannot run this generator read them: the registry is the record of every identifier ever
 * assigned, the Chromium GN build compiles the header and the tables, and SwiftPM compiles the Swift wrappers.
 */
function committedTargets(coreDir: string, outputs: Outputs): [string, string, string][] {
  return [
    [
      join(coreDir, "interface", REGISTRY_FILE),
      outputs.registry,
      `packages/core/interface/${REGISTRY_FILE}`,
    ],
    [join(coreDir, HEADER_PATH), outputs.header, `packages/core/${HEADER_PATH}`],
    [join(coreDir, TABLE_PATH), outputs.table, `packages/core/${TABLE_PATH}`],
    [join(coreDir, SWIFT_PATH), outputs.swift, SWIFT_REPORT_PATH],
  ];
}

/**
 * The `@bdk/internal` modules as absolute path and text. They are git-ignored build outputs: only Nx targets
 * read them, and those depend on `js-codegen:codegen`.
 */
function typescriptTargets(coreDir: string, outputs: Outputs): [string, string][] {
  return Object.entries(outputs.typescript).map(([file, text]): [string, string] => [
    join(coreDir, TYPESCRIPT_DIR, file),
    text,
  ]);
}

/** Writes every generated file, leaving one untouched when its content would not change. */
export function generate(coreDir: string): string[] {
  const written: string[] = [];
  const outputs = build(readInputs(coreDir));
  for (const [path, text] of [
    ...committedTargets(coreDir, outputs),
    ...typescriptTargets(coreDir, outputs),
  ]) {
    if (readOptional(path) !== text) {
      mkdirSync(dirname(path), { recursive: true });
      writeFileSync(path, text);
      written.push(path);
    }
  }
  return written;
}

/**
 * Names the committed outputs that differ from what the definitions generate. Empty when current. It also
 * generates the `@bdk/internal` modules in memory, so a definition they cannot be written from still fails.
 */
export function check(coreDir: string): string[] {
  return committedTargets(coreDir, build(readInputs(coreDir)))
    .filter(([path, text]) => readOptional(path) !== text)
    .map(([, , report]) => report);
}
