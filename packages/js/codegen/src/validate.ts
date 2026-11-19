import type { Issue } from "./issue.js";
import {
  CALLERS,
  type Caller,
  CONSTANT_KINDS,
  type Command,
  type ConstantGroup,
  type ConstantKind,
  type Definitions,
  type EventDefinition,
  type FieldDefinition,
  type Policy,
  PRIMITIVES,
  PROFILE_MODES,
  type ProfileMode,
  REQUIREMENTS,
  type Requirement,
  type TypeDefinition,
} from "./model.js";

/** One interface file as read from disk: its base name and its parsed JSON. */
export interface SourceFile {
  readonly file: string;
  readonly json: unknown;
}

export interface ValidationContext {
  /** The current `BDK_ABI_VERSION`. A `since` above it is an error. */
  readonly abiVersion: number;
  readonly policy: Policy;
}

export interface Validated {
  readonly issues: readonly Issue[];
  readonly definitions: Definitions;
}

const NAME = /^[a-z][a-zA-Z0-9]*$/;
const CAPABILITY = /^[a-z][a-zA-Z0-9-]*\.[a-z][a-zA-Z0-9-]*$/;
const TYPE_NAME = /^[A-Z][a-zA-Z0-9]*$/;
const TYPE_EXPRESSION = /^([A-Za-z][A-Za-z0-9]*)(\[\])?(\?)?$/;
const MACRO_PREFIX = /^BDK_[A-Z0-9]+(_[A-Z0-9]+)*_$/;
const MACRO_SUFFIX = /^[A-Z][A-Z0-9]*(_[A-Z0-9]+)*$/;

const FILE_KEYS = ["namespace", "since", "types", "commands", "events", "constants"];
const COMMAND_KEYS = [
  "name",
  "capability",
  "callers",
  "profile",
  "requires",
  "raises",
  "args",
  "result",
  "paginated",
  "progress",
  "stream",
  "binary",
  "authenticateWhen",
  "since",
];
const EVENT_KEYS = ["name", "capability", "payload", "since"];
const FLAG_KEYS = ["paginated", "progress", "stream", "binary"] as const;
const CONSTANT_KEYS = ["group", "kind", "prefix", "comment", "values"];
const POLICY_KEYS = ["neverForPages", "neverForSessions"];

type Json = Record<string, unknown>;

function isObject(value: unknown): value is Json {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function quote(value: unknown): string {
  return JSON.stringify(value);
}

function fieldsOf(body: Json): FieldDefinition[] {
  return Object.entries(body).map(([name, type]) => ({ name, type: String(type) }));
}

/** The macro fragment for a dotted name: `history.deleteAll` becomes `HISTORY_DELETE_ALL`. */
export function macroFragment(name: string): string {
  return name
    .split(".")
    .map((part) => part.replace(/([a-z0-9])([A-Z])/g, "$1_$2").toUpperCase())
    .join("_");
}

export function matchesCapabilityPattern(capability: string, pattern: string): boolean {
  return pattern.endsWith(".*")
    ? capability.startsWith(pattern.slice(0, -1))
    : capability === pattern;
}

/** Checks the policy file. Returns its content, or empty lists with the issues found. */
export function parsePolicy(file: string, json: unknown): { policy: Policy; issues: Issue[] } {
  const issues: Issue[] = [];
  const add = (path: string, message: string) => issues.push({ file, path, message });
  const lists: Record<string, string[]> = { neverForPages: [], neverForSessions: [] };
  if (!isObject(json)) {
    add("$", "must be an object");
  } else {
    for (const key of Object.keys(json)) {
      if (!POLICY_KEYS.includes(key)) add(`$.${key}`, "unknown field");
    }
    for (const key of POLICY_KEYS) {
      const list = json[key];
      const target = lists[key] as string[];
      if (!Array.isArray(list)) {
        add(`$.${key}`, "must be an array of capability names or namespace.* patterns");
        continue;
      }
      list.forEach((entry, index) => {
        const ok =
          typeof entry === "string" &&
          (CAPABILITY.test(entry) || /^[a-z][a-zA-Z0-9-]*\.\*$/.test(entry));
        if (ok) target.push(entry);
        else
          add(
            `$.${key}[${index}]`,
            `${quote(entry)} is not a capability name or namespace.* pattern`,
          );
      });
    }
  }
  return {
    policy: {
      neverForPages: lists.neverForPages as string[],
      neverForSessions: lists.neverForSessions as string[],
    },
    issues,
  };
}

interface TypeInfo {
  readonly file: string;
}

/** Validates every interface file together; names are global across files. */
export function validate(files: readonly SourceFile[], context: ValidationContext): Validated {
  const issues: Issue[] = [];
  const constants: ConstantGroup[] = [];
  const commands: Command[] = [];
  const events: EventDefinition[] = [];
  const typeDefinitions: TypeDefinition[] = [];

  const types = new Map<string, TypeInfo>();
  const commandNames = new Map<string, string>();
  const eventNames = new Map<string, string>();
  const groupNames = new Map<string, string>();
  const prefixes = new Map<string, string>();
  const macros = new Map<string, string>();
  const surfaces = new Set<string>();

  const objects = files.filter(({ json }) => isObject(json));

  const claimMacro = (file: string, path: string, macro: string) => {
    const first = macros.get(macro);
    if (first === undefined) macros.set(macro, `${file} ${path}`);
    else issues.push({ file, path, message: `macro ${macro} is already defined at ${first}` });
  };

  const checkSince = (file: string, path: string, value: unknown) => {
    if (value === undefined) return;
    if (typeof value !== "number" || !Number.isInteger(value) || value < 1) {
      issues.push({ file, path, message: "must be an integer of at least 1" });
    } else if (value > context.abiVersion) {
      issues.push({
        file,
        path,
        message: `${value} is newer than the current ABI version ${context.abiVersion}`,
      });
    }
  };

  const checkUnknownKeys = (file: string, path: string, json: Json, allowed: readonly string[]) => {
    for (const key of Object.keys(json)) {
      if (!allowed.includes(key))
        issues.push({ file, path: `${path}.${key}`, message: "unknown field" });
    }
  };

  // Pass 1: shapes of each file, the type table and the constants.
  for (const { file, json } of files) {
    const add = (path: string, message: string) => issues.push({ file, path, message });
    if (!isObject(json)) {
      add("$", "must be an object");
      continue;
    }
    checkUnknownKeys(file, "$", json, FILE_KEYS);

    const namespace = json.namespace;
    if (typeof namespace !== "string" || !NAME.test(namespace)) {
      add("$.namespace", "must match [a-z][a-zA-Z0-9]*");
    } else if (`${namespace}.json` !== file) {
      add("$.namespace", `${quote(namespace)} does not match the file name ${quote(file)}`);
    } else if (namespace === "app") {
      add("$.namespace", `${quote(namespace)} is reserved for application commands`);
    }
    checkSince(file, "$.since", json.since);
    if (json.since === undefined) add("$.since", "is required");

    if (json.types !== undefined) {
      if (!isObject(json.types)) add("$.types", "must be an object");
      else {
        for (const name of Object.keys(json.types)) {
          const path = `$.types.${name}`;
          if (!TYPE_NAME.test(name)) add(path, "type names must match [A-Z][a-zA-Z0-9]*");
          else if (PRIMITIVES.includes(name.toLowerCase()))
            add(path, `${quote(name)} is a primitive type name`);
          else if (types.has(name))
            add(path, `duplicate type ${quote(name)} (first defined in ${types.get(name)?.file})`);
          else types.set(name, { file });
        }
      }
    }

    if (json.constants !== undefined) {
      if (!Array.isArray(json.constants)) add("$.constants", "must be an array");
      else {
        json.constants.forEach((entry, index) => {
          const group = checkConstantGroup(file, `$.constants[${index}]`, entry);
          if (group === undefined) return;
          const path = `$.constants[${index}]`;
          const first = groupNames.get(group.group);
          if (first !== undefined)
            add(
              `${path}.group`,
              `duplicate group ${quote(group.group)} (first defined in ${first})`,
            );
          groupNames.set(group.group, file);
          const prefixOwner = prefixes.get(group.prefix);
          if (prefixOwner !== undefined) {
            add(
              `${path}.prefix`,
              `prefix ${group.prefix} is already used by group ${quote(prefixOwner)}`,
            );
          }
          prefixes.set(group.prefix, group.group);
          for (const [suffix] of group.values) {
            claimMacro(file, `${path}.values.${suffix}`, group.prefix + suffix);
          }
          if (group.group === "surfaces") {
            for (const [suffix] of group.values) surfaces.add(suffix.toLowerCase());
          }
          constants.push(group);
        });
      }
    }
  }

  const checkTypeExpression = (
    file: string,
    path: string,
    expression: unknown,
    allowVoid: boolean,
  ) => {
    const add = (message: string) => issues.push({ file, path, message });
    if (typeof expression !== "string") return add("must be a type expression string");
    if (allowVoid && expression === "void") return;
    const match = TYPE_EXPRESSION.exec(expression);
    if (match === null) return add(`invalid type expression ${quote(expression)}`);
    const base = match[1] as string;
    if (!PRIMITIVES.includes(base) && !types.has(base)) add(`unknown type ${quote(base)}`);
  };

  const checkFields = (file: string, path: string, fields: Json) => {
    for (const [name, expression] of Object.entries(fields)) {
      if (!NAME.test(name))
        issues.push({
          file,
          path: `${path}.${name}`,
          message: "field names must match [a-z][a-zA-Z0-9]*",
        });
      checkTypeExpression(file, `${path}.${name}`, expression, false);
    }
  };

  const checkCapability = (file: string, path: string, value: unknown, required: boolean) => {
    if (value === undefined) {
      if (required) issues.push({ file, path, message: "is required" });
      return undefined;
    }
    if (typeof value !== "string" || !CAPABILITY.test(value)) {
      issues.push({ file, path, message: `${quote(value)} does not match namespace.level` });
      return undefined;
    }
    if (value.startsWith("app.")) {
      issues.push({
        file,
        path,
        message: `${quote(value)} starts with "app."; application names are declared at run time`,
      });
      return undefined;
    }
    return value;
  };

  const checkName = (
    file: string,
    path: string,
    value: unknown,
    namespace: unknown,
    what: string,
  ) => {
    const issue = (message: string) => issues.push({ file, path, message });
    if (typeof value !== "string") {
      issue("is required and must be a string");
      return undefined;
    }
    if (value.startsWith("app.")) {
      issue(`${quote(value)} starts with "app."; application names are declared at run time`);
      return undefined;
    }
    const segments = value.split(".");
    if (segments.length < 2 || !segments.every((segment) => NAME.test(segment))) {
      issue(`${quote(value)} does not match namespace.verb`);
      return undefined;
    }
    if (segments[0] !== namespace) {
      issue(`${what} ${quote(value)} is not in the file's namespace ${quote(namespace)}`);
      return undefined;
    }
    return value;
  };

  // Pass 2: commands and events, which refer to the types, surfaces and policy collected above.
  for (const { file, json } of objects) {
    const add = (path: string, message: string) => issues.push({ file, path, message });
    const namespace = (json as Json).namespace;

    const commandList = (json as Json).commands;
    if (commandList !== undefined && !Array.isArray(commandList))
      add("$.commands", "must be an array");
    (Array.isArray(commandList) ? commandList : []).forEach((entry: unknown, index: number) => {
      const path = `$.commands[${index}]`;
      if (!isObject(entry)) {
        add(path, "must be an object");
        return;
      }
      checkUnknownKeys(file, path, entry, COMMAND_KEYS);

      const name = checkName(file, `${path}.name`, entry.name, namespace, "command");
      if (name !== undefined) {
        const first = commandNames.get(name);
        if (first !== undefined)
          add(`${path}.name`, `duplicate command ${quote(name)} (first defined in ${first})`);
        else {
          commandNames.set(name, file);
          claimMacro(file, `${path}.name`, `BDK_COMMAND_${macroFragment(name)}`);
        }
      }
      const capability = checkCapability(file, `${path}.capability`, entry.capability, false);

      const callers: Caller[] = [];
      if (!Array.isArray(entry.callers) || entry.callers.length === 0) {
        add(`${path}.callers`, "must be a non-empty array of swift, page or session");
      } else {
        entry.callers.forEach((caller: unknown, at: number) => {
          if (!CALLERS.includes(caller as Caller))
            add(
              `${path}.callers[${at}]`,
              `unknown caller ${quote(caller)}; expected swift, page or session`,
            );
          else if (callers.includes(caller as Caller))
            add(`${path}.callers[${at}]`, `duplicate caller ${quote(caller)}`);
          else callers.push(caller as Caller);
        });
      }

      if (!PROFILE_MODES.includes(entry.profile as never)) {
        add(
          `${path}.profile`,
          `must be one of required, optional, none; got ${quote(entry.profile)}`,
        );
      }
      if (entry.requires !== undefined) {
        if (!Array.isArray(entry.requires))
          add(`${path}.requires`, "must be an array of gesture or authentication");
        else {
          entry.requires.forEach((value: unknown, at: number) => {
            if (!REQUIREMENTS.includes(value as never))
              add(
                `${path}.requires[${at}]`,
                `unknown requirement ${quote(value)}; expected gesture or authentication`,
              );
          });
        }
      }
      if (
        entry.raises !== undefined &&
        (typeof entry.raises !== "string" || !surfaces.has(entry.raises))
      ) {
        add(`${path}.raises`, `unknown surface ${quote(entry.raises)}`);
      }
      for (const flag of FLAG_KEYS) {
        if (entry[flag] !== undefined && typeof entry[flag] !== "boolean")
          add(`${path}.${flag}`, "must be true or false");
      }
      checkSince(file, `${path}.since`, entry.since);

      if (!isObject(entry.args))
        add(`${path}.args`, "is required and must be an object of field types");
      else checkFields(file, `${path}.args`, entry.args);
      if (entry.result === undefined) add(`${path}.result`, "is required");
      else if (isObject(entry.result)) checkFields(file, `${path}.result`, entry.result);
      else checkTypeExpression(file, `${path}.result`, entry.result, true);

      if (entry.paginated === true) {
        const args = isObject(entry.args) ? entry.args : {};
        const result = isObject(entry.result) ? entry.result : {};
        for (const field of ["limit", "cursor"]) {
          if (!(field in args)) add(`${path}.args.${field}`, "is required for a paginated command");
        }
        for (const field of ["items", "nextCursor"]) {
          if (!(field in result))
            add(`${path}.result.${field}`, "is required for a paginated command");
        }
      }

      if (capability !== undefined) {
        const never = context.policy.neverForPages.find((pattern) =>
          matchesCapabilityPattern(capability, pattern),
        );
        if (never !== undefined && callers.includes("page")) {
          add(
            `${path}.capability`,
            `${quote(capability)} is on the never-for-pages list (${never}) but callers includes "page"`,
          );
        }
        const neverSession = context.policy.neverForSessions.find((pattern) =>
          matchesCapabilityPattern(capability, pattern),
        );
        if (neverSession !== undefined && callers.includes("session")) {
          add(
            `${path}.capability`,
            `${quote(capability)} is on the never-for-sessions list (${neverSession}) but callers includes "session"`,
          );
        }
      }

      if (entry.authenticateWhen !== undefined) {
        const field = entry.authenticateWhen;
        if (typeof field !== "string" || !isObject(entry.args) || !(field in entry.args))
          add(`${path}.authenticateWhen`, `must name a field of args; got ${quote(field)}`);
        else if (Array.isArray(entry.requires) && entry.requires.includes("authentication"))
          add(`${path}.authenticateWhen`, 'is redundant: requires already has "authentication"');
      }

      if (name !== undefined) {
        commands.push({
          name,
          capability,
          callers,
          profile: entry.profile as ProfileMode,
          requires: (Array.isArray(entry.requires) ? entry.requires : []) as Requirement[],
          raises: typeof entry.raises === "string" ? entry.raises : undefined,
          paginated: entry.paginated === true,
          progress: entry.progress === true,
          stream: entry.stream === true,
          binary: entry.binary === true,
          authenticateWhen:
            typeof entry.authenticateWhen === "string" ? entry.authenticateWhen : undefined,
          args: isObject(entry.args) ? fieldsOf(entry.args) : [],
          result: isObject(entry.result) ? fieldsOf(entry.result) : String(entry.result),
        });
      }
    });

    const eventList = (json as Json).events;
    if (eventList !== undefined && !Array.isArray(eventList)) add("$.events", "must be an array");
    (Array.isArray(eventList) ? eventList : []).forEach((entry: unknown, index: number) => {
      const path = `$.events[${index}]`;
      if (!isObject(entry)) {
        add(path, "must be an object");
        return;
      }
      checkUnknownKeys(file, path, entry, EVENT_KEYS);
      const name = checkName(file, `${path}.name`, entry.name, namespace, "event");
      if (name !== undefined) {
        const first = eventNames.get(name);
        if (first !== undefined)
          add(`${path}.name`, `duplicate event ${quote(name)} (first defined in ${first})`);
        else {
          eventNames.set(name, file);
          claimMacro(file, `${path}.name`, `BDK_EVENT_${macroFragment(name)}`);
        }
      }
      const eventCapability = checkCapability(file, `${path}.capability`, entry.capability, true);
      checkSince(file, `${path}.since`, entry.since);
      if (entry.payload === undefined) add(`${path}.payload`, "is required");
      else if (isObject(entry.payload)) checkFields(file, `${path}.payload`, entry.payload);
      else checkTypeExpression(file, `${path}.payload`, entry.payload, false);
      if (name !== undefined && eventCapability !== undefined)
        events.push({
          name,
          capability: eventCapability,
          payload: isObject(entry.payload) ? fieldsOf(entry.payload) : String(entry.payload),
        });
    });
  }

  // Struct and enum bodies are checked last, once every type name is known.
  for (const { file, json } of objects) {
    const table = (json as Json).types;
    if (!isObject(table)) continue;
    for (const [name, body] of Object.entries(table)) {
      const path = `$.types.${name}`;
      if (typeof body === "string") {
        checkTypeExpression(file, path, body, false);
        typeDefinitions.push({ name, kind: "alias", target: body });
      } else if (isObject(body) && "enum" in body) {
        checkUnknownKeys(file, path, body, ["enum"]);
        const values = body.enum;
        if (!Array.isArray(values) || values.length === 0) {
          issues.push({
            file,
            path: `${path}.enum`,
            message: "must be a non-empty array of names",
          });
        } else {
          const seen = new Set<unknown>();
          values.forEach((value: unknown, at: number) => {
            if (typeof value !== "string" || !NAME.test(value))
              issues.push({
                file,
                path: `${path}.enum[${at}]`,
                message: `${quote(value)} must match [a-z][a-zA-Z0-9]*`,
              });
            else if (seen.has(value))
              issues.push({
                file,
                path: `${path}.enum[${at}]`,
                message: `duplicate enum value ${quote(value)}`,
              });
            seen.add(value);
          });
          typeDefinitions.push({ name, kind: "enum", values: values as string[] });
        }
      } else if (isObject(body)) {
        checkFields(file, path, body);
        typeDefinitions.push({ name, kind: "struct", fields: fieldsOf(body) });
      } else
        issues.push({
          file,
          path,
          message: 'must be a struct (object), an enum ({ "enum": [...] }) or an alias (string)',
        });
    }
  }

  return { issues, definitions: { constants, types: typeDefinitions, commands, events } };

  function checkConstantGroup(
    file: string,
    path: string,
    entry: unknown,
  ): ConstantGroup | undefined {
    const before = issues.length;
    const add = (at: string, message: string) =>
      issues.push({ file, path: `${path}${at}`, message });
    if (!isObject(entry)) {
      add("", "must be an object");
      return undefined;
    }
    checkUnknownKeys(file, path, entry, CONSTANT_KEYS);
    const { group, kind, prefix, comment, values } = entry;
    if (typeof group !== "string" || !NAME.test(group))
      add(".group", "must match [a-z][a-zA-Z0-9]*");
    if (!CONSTANT_KINDS.includes(kind as ConstantKind))
      add(".kind", `must be one of string, enum, flags; got ${quote(kind)}`);
    if (typeof prefix !== "string" || !MACRO_PREFIX.test(prefix))
      add(".prefix", "must start with BDK_ and end with _, in upper case");
    if (comment !== undefined && typeof comment !== "string") add(".comment", "must be a string");
    const pairs: Array<readonly [string, string | number]> = [];
    if (!isObject(values) || Object.keys(values).length === 0) {
      add(".values", "must be a non-empty object");
    } else {
      const seen = new Map<string | number, string>();
      for (const [suffix, value] of Object.entries(values)) {
        const at = `.values.${suffix}`;
        if (!MACRO_SUFFIX.test(suffix))
          add(at, "names must be upper case words joined by underscores");
        if (!CONSTANT_KINDS.includes(kind as ConstantKind)) continue;
        let valid = false;
        if (kind === "string")
          valid =
            typeof value === "string" &&
            value.length > 0 &&
            /^[\x20-\x7e]*$/.test(value) &&
            !/["\\]/.test(value);
        else if (kind === "enum")
          valid =
            typeof value === "number" &&
            Number.isInteger(value) &&
            value >= 0 &&
            value <= 0xffffffff;
        else if (kind === "flags")
          valid =
            typeof value === "number" &&
            Number.isInteger(value) &&
            value >= 1 &&
            value <= 0x80000000 &&
            (value & (value - 1)) === 0;
        if (!valid) {
          const expected =
            kind === "string"
              ? "a non-empty printable string without quotes or backslashes"
              : kind === "enum"
                ? "an integer from 0 to 4294967295"
                : "a single bit (a power of two up to 2147483648)";
          add(at, `${quote(value)} must be ${expected}`);
          continue;
        }
        const owner = seen.get(value as string | number);
        if (owner !== undefined) add(at, `value ${quote(value)} is already used by ${owner}`);
        seen.set(value as string | number, suffix);
        pairs.push([suffix, value as string | number]);
      }
    }
    if (issues.length > before) return undefined;
    return {
      group: group as string,
      kind: kind as ConstantKind,
      prefix: prefix as string,
      comment: comment as string | undefined,
      values: pairs,
    };
  }
}
