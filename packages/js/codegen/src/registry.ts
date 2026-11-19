import type { Issue } from "./issue.js";
import type { Definitions } from "./model.js";

/** File name of the registry inside the interface directory. */
export const REGISTRY_FILE = "ids.json";

/**
 * Every identifier ever assigned. An entry is never removed and its id never changes; removing a
 * definition marks the entry retired, and its id is never given to another name.
 */
export interface RegistryEntry {
  readonly name: string;
  readonly id: number | string;
  readonly retired?: true;
}

export interface Registry {
  readonly commands: readonly RegistryEntry[];
  readonly events: readonly RegistryEntry[];
  /** Per constant group, in the order the entries were first assigned. */
  readonly constants: Readonly<Record<string, readonly RegistryEntry[]>>;
}

export const EMPTY_REGISTRY: Registry = { commands: [], events: [], constants: {} };

type Json = Record<string, unknown>;

function isObject(value: unknown): value is Json {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function quote(value: unknown): string {
  return JSON.stringify(value);
}

/** Checks the shape and the internal consistency of a registry read from disk. */
export function parseRegistry(json: unknown): { registry: Registry; issues: Issue[] } {
  const issues: Issue[] = [];
  const add = (path: string, message: string): void => {
    issues.push({ file: REGISTRY_FILE, path, message });
  };

  const parseList = (path: string, list: unknown, numeric: boolean): RegistryEntry[] => {
    if (!Array.isArray(list)) {
      add(path, "must be an array of identifier entries");
      return [];
    }
    const entries: RegistryEntry[] = [];
    const names = new Set<string>();
    const ids = new Set<number | string>();
    for (const [index, raw] of list.entries()) {
      const at = `${path}[${index}]`;
      if (!isObject(raw)) {
        add(at, "must be an object");
        continue;
      }
      for (const key of Object.keys(raw)) {
        if (!["name", "id", "retired"].includes(key)) add(`${at}.${key}`, "unknown field");
      }
      const { name, id, retired } = raw;
      if (typeof name !== "string" || name === "") {
        add(`${at}.name`, "must be a non-empty string");
        continue;
      }
      const idOk = numeric
        ? typeof id === "number" && Number.isInteger(id) && id >= 1
        : typeof id === "number" || typeof id === "string";
      if (!idOk) {
        add(`${at}.id`, numeric ? "must be a positive integer" : "must be a number or a string");
        continue;
      }
      if (retired !== undefined && retired !== true) {
        add(`${at}.retired`, "must be true when present");
        continue;
      }
      if (names.has(name)) {
        add(`${at}.name`, `duplicate name ${quote(name)}`);
        continue;
      }
      if (ids.has(id as number | string)) {
        add(`${at}.id`, `id ${quote(id)} is assigned twice; identifiers are never reused`);
        continue;
      }
      names.add(name);
      ids.add(id as number | string);
      entries.push(
        retired === true
          ? { name, id: id as number | string, retired }
          : { name, id: id as number | string },
      );
    }
    return entries;
  };

  if (!isObject(json)) {
    add("$", "must be an object");
    return { registry: EMPTY_REGISTRY, issues };
  }
  for (const key of Object.keys(json)) {
    if (!["commands", "events", "constants"].includes(key)) add(`$.${key}`, "unknown field");
  }
  const commands = parseList("$.commands", json.commands, true);
  const events = parseList("$.events", json.events, true);
  const constants: Record<string, RegistryEntry[]> = {};
  if (!isObject(json.constants)) add("$.constants", "must be an object of groups");
  else {
    for (const [group, list] of Object.entries(json.constants)) {
      constants[group] = parseList(`$.constants.${group}`, list, false);
    }
  }
  return { registry: { commands, events, constants }, issues };
}

function reconcileList(
  path: string,
  existing: readonly RegistryEntry[],
  defined: ReadonlyArray<readonly [string, number | string | undefined]>,
  issues: Issue[],
): RegistryEntry[] {
  const add = (at: string, message: string) =>
    issues.push({ file: REGISTRY_FILE, path: at, message });
  const definedNames = new Map(defined);
  const known = new Map(existing.map((entry) => [entry.name, entry]));
  const result: RegistryEntry[] = [];

  for (const entry of existing) {
    if (entry.retired === true) {
      if (definedNames.has(entry.name)) {
        add(
          `${path}[${quote(entry.name)}]`,
          `${quote(entry.name)} was retired with id ${quote(entry.id)}; a retired name cannot be defined again`,
        );
      }
      result.push(entry);
      continue;
    }
    const value = definedNames.get(entry.name);
    if (!definedNames.has(entry.name)) {
      result.push({ name: entry.name, id: entry.id, retired: true });
    } else {
      if (value !== undefined && value !== entry.id) {
        add(
          `${path}[${quote(entry.name)}]`,
          `${quote(entry.name)} is defined as ${quote(value)} but was assigned ${quote(entry.id)}; identifiers are never renumbered`,
        );
      }
      result.push(entry);
    }
  }

  let next =
    existing.reduce(
      (max, entry) => (typeof entry.id === "number" ? Math.max(max, entry.id) : max),
      0,
    ) + 1;
  for (const [name, value] of defined) {
    if (known.has(name)) continue;
    if (value === undefined) {
      result.push({ name, id: next });
      next += 1;
      continue;
    }
    const owner = result.find((entry) => entry.id === value);
    if (owner !== undefined) {
      const state = owner.retired === true ? "retired " : "";
      add(
        `${path}[${quote(name)}]`,
        `value ${quote(value)} belongs to ${state}${quote(owner.name)}; identifiers are never reused`,
      );
      continue;
    }
    result.push({ name, id: value });
  }
  return result;
}

/**
 * Applies the definitions to the registry: new names get the next free identifier (commands and events)
 * or are recorded with their stated value (constants), removed names are retired, and any attempt to
 * renumber or reuse an identifier is an issue.
 */
export function reconcile(
  registry: Registry,
  definitions: Definitions,
): { registry: Registry; issues: Issue[] } {
  const issues: Issue[] = [];
  const commands = reconcileList(
    "$.commands",
    registry.commands,
    definitions.commands.map((command) => [command.name, undefined] as const),
    issues,
  );
  const events = reconcileList(
    "$.events",
    registry.events,
    definitions.events.map((event) => [event.name, undefined] as const),
    issues,
  );
  const constants: Record<string, readonly RegistryEntry[]> = {};
  const groups = new Map(definitions.constants.map((group) => [group.group, group]));
  for (const name of [...Object.keys(registry.constants), ...groups.keys()]) {
    if (name in constants) continue;
    const group = groups.get(name);
    constants[name] = reconcileList(
      `$.constants.${name}`,
      registry.constants[name] ?? [],
      group === undefined ? [] : group.values,
      issues,
    );
  }
  return { registry: { commands, events, constants }, issues };
}

/** Biome's line width: a list that fits on one line is printed on one line. */
const LINE_WIDTH = 100;

/**
 * The registry as committed, in the layout `biome format` produces so the file passes the repository
 * lint: a list that fits on one line stays on one line, otherwise one entry per line.
 */
export function serializeRegistry(registry: Registry): string {
  const line = (entry: RegistryEntry) =>
    `{ "name": ${quote(entry.name)}, "id": ${quote(entry.id)}${entry.retired === true ? ', "retired": true' : ""} }`;
  const list = (key: string, entries: readonly RegistryEntry[], indent: string, comma: boolean) => {
    const head = `${indent}${quote(key)}: `;
    const flat = `[${entries.map(line).join(", ")}]`;
    if (head.length + flat.length + (comma ? 1 : 0) <= LINE_WIDTH) return head + flat;
    return `${head}[\n${entries.map((entry) => `${indent}  ${line(entry)}`).join(",\n")}\n${indent}]`;
  };
  const groups = Object.entries(registry.constants).map(([group, entries], index, all) =>
    list(group, entries, "    ", index < all.length - 1),
  );
  const constants = groups.length === 0 ? "{}" : `{\n${groups.join(",\n")}\n  }`;
  return `{\n${list("commands", registry.commands, "  ", true)},\n${list("events", registry.events, "  ", true)},\n  "constants": ${constants}\n}\n`;
}
