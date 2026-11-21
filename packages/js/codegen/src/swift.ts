import type {
  Command,
  Definitions,
  EventDefinition,
  FieldDefinition,
  TypeDefinition,
} from "./model.js";
import { PRIMITIVES } from "./model.js";

/** Where the Swift wrappers are written, relative to `packages/core`. */
export const SWIFT_PATH = "../swift/Sources/BDK/Generated/BDKServices.swift";

/** The path the generated file is reported by, from the repository root. */
export const SWIFT_REPORT_PATH = "packages/swift/Sources/BDK/Generated/BDKServices.swift";

/**
 * Namespaces the Swift layer implements by hand, on the ABI's own functions (`bdk_profiles_*`), so no
 * wrapper is generated for them. A namespace is native only because the ABI offers a richer, typed
 * entry point; the commands stay reachable through `runtime.commands.invoke`. `contents`, `debug` and
 * `devtools` address one page (the invoke options' `contents`), so they are methods of `BDKWebContents`
 * (`page.scripting`, `page.capture`, `page.devTools`, swift-api.md sections 6 and 14).
 */
export const NATIVE_NAMESPACES: readonly string[] = ["profiles", "contents", "debug", "devtools"];

/** Namespaces whose Swift name differs from the interface name, because `BDKRuntime` has the other. */
const SERVICE_NAMES: Readonly<Record<string, { accessor: string; type: string }>> = {
  runtime: { accessor: "about", type: "BDKRuntimeInformation" },
};

/** `info()` on `BDKRuntime` is a property, so the command `runtime.info` is `about.info()`. Other names are kept. */
const METHOD_NAMES: Readonly<Record<string, string>> = {
  "history.deleteUrls": "delete",
};

/**
 * Argument labels where swift-api.md names one. `_` is no label. Everything not listed is labelled with
 * its own name, except a first argument called `id` or `key`, which has no label.
 */
const LABELS: Readonly<Record<string, Readonly<Record<string, string>>>> = {
  "history.search": { text: "_", range: "in" },
  "bookmarks.create": { parentId: "in", index: "at" },
  // swift-api.md: createFolder("Reading", in: bar.id)
  "bookmarks.createFolder": { title: "_", parentId: "in", index: "at" },
  "bookmarks.move": { parentId: "to", index: "at" },
};

/** Parameter order where swift-api.md differs from the definition's argument order. Lists every argument. */
const ORDER: Readonly<Record<string, readonly string[]>> = {
  "bookmarks.create": ["url", "title", "parentId", "index"],
  "bookmarks.createFolder": ["title", "parentId", "index"],
};

const UNLABELLED_FIRST = new Set(["id", "key"]);

const KEYWORDS = new Set([
  "associatedtype",
  "class",
  "deinit",
  "enum",
  "extension",
  "fileprivate",
  "func",
  "import",
  "init",
  "inout",
  "internal",
  "let",
  "open",
  "operator",
  "private",
  "precedencegroup",
  "protocol",
  "public",
  "rethrows",
  "static",
  "struct",
  "subscript",
  "typealias",
  "var",
  "break",
  "case",
  "catch",
  "continue",
  "default",
  "defer",
  "do",
  "else",
  "fallthrough",
  "for",
  "guard",
  "if",
  "in",
  "repeat",
  "return",
  "throw",
  "switch",
  "where",
  "while",
  "as",
  "await",
  "false",
  "is",
  "nil",
  "self",
  "super",
  "throws",
  "true",
  "try",
]);

const SWIFT_PRIMITIVES = [
  "String",
  "Int",
  "Double",
  "Bool",
  "URL",
  "Date",
  "Data",
  "BDKJSValue",
] as const;

const PROGRESS_PARAMETER = "onProgress: (@MainActor (Double, String) -> Void)? = nil";

function pascal(text: string): string {
  return text.charAt(0).toUpperCase() + text.slice(1);
}

function ident(name: string): string {
  return KEYWORDS.has(name) ? `\`${name}\`` : name;
}

function typeName(name: string): string {
  return `BDK${name}`;
}

/** The Swift spelling of a type expression such as `HistoryEntry[]?`. */
function swiftType(expression: string): string {
  const optional = expression.endsWith("?");
  const bare = optional ? expression.slice(0, -1) : expression;
  const array = bare.endsWith("[]");
  const base = array ? bare.slice(0, -2) : bare;
  const prim = PRIMITIVES.indexOf(base);
  const name = prim >= 0 ? SWIFT_PRIMITIVES[prim] : typeName(base);
  return `${array ? `[${name}]` : name}${optional ? "?" : ""}`;
}

function isOptional(expression: string): boolean {
  return expression.endsWith("?");
}

interface Segments {
  readonly namespace: string;
  /** Set for a command such as `autofill.cards.list`. */
  readonly group: string | undefined;
  readonly method: string;
}

function segments(name: string): Segments {
  const parts = name.split(".");
  const [namespace, ...rest] = parts;
  const method = rest.pop();
  if (namespace === undefined || method === undefined || rest.length > 1) {
    throw new Error(`${name}: a command name has a namespace, an optional group and a method`);
  }
  return { namespace, group: rest[0], method };
}

function methodName(command: Command): string {
  return METHOD_NAMES[command.name] ?? segments(command.name).method;
}

function serviceType(namespace: string, group?: string): string {
  const base = SERVICE_NAMES[namespace]?.type ?? `BDK${pascal(namespace)}`;
  return group === undefined ? base : `${base}${pascal(group)}`;
}

function accessorName(namespace: string): string {
  return SERVICE_NAMES[namespace]?.accessor ?? namespace;
}

/** Where a namespace's wrapper hangs: the runtime, a profile, or both (a setting has an application scope). */
function scopesOf(
  namespace: string,
  commands: readonly Command[],
): readonly ("runtime" | "profile")[] {
  const modes = new Set(commands.map((command) => command.profile));
  if (modes.has("none") && modes.size > 1) {
    throw new Error(
      `${namespace}: commands that take a profile and commands that do not cannot share a namespace`,
    );
  }
  if (modes.has("none")) return ["runtime"];
  if (modes.has("optional") && modes.has("required")) {
    throw new Error(`${namespace}: profile "required" and "optional" cannot share a namespace`);
  }
  return modes.has("optional") ? ["profile", "runtime"] : ["profile"];
}

// MARK: types

function renderStruct(name: string, fields: readonly FieldDefinition[], indent: string): string[] {
  const out: string[] = [];
  out.push(`${indent}public struct ${name}: Codable, Sendable, Equatable {`);
  for (const field of fields) {
    out.push(`${indent}    public var ${ident(field.name)}: ${swiftType(field.type)}`);
  }
  if (fields.length === 0) {
    out.push(`${indent}    public init() {}`);
  } else {
    out.push("");
    out.push(`${indent}    public init(`);
    fields.forEach((field, index) => {
      const fallback = isOptional(field.type) ? " = nil" : "";
      const comma = index < fields.length - 1 ? "," : "";
      out.push(
        `${indent}        ${ident(field.name)}: ${swiftType(field.type)}${fallback}${comma}`,
      );
    });
    out.push(`${indent}    ) {`);
    for (const field of fields) {
      out.push(`${indent}        self.${ident(field.name)} = ${ident(field.name)}`);
    }
    out.push(`${indent}    }`);
  }
  out.push(`${indent}}`);
  return out;
}

function renderType(type: TypeDefinition): string[] {
  const name = typeName(type.name);
  if (type.kind === "alias") return [`public typealias ${name} = ${swiftType(type.target)}`, ""];
  if (type.kind === "enum") {
    return [
      `public enum ${name}: String, Codable, Sendable, Equatable, CaseIterable {`,
      ...type.values.map((value) => `    case ${ident(value)}`),
      "}",
      "",
    ];
  }
  return [...renderStruct(name, type.fields, ""), ""];
}

// MARK: commands

interface Parameter {
  readonly field: FieldDefinition;
  readonly label: string;
}

function parameters(command: Command): Parameter[] {
  const labels = LABELS[command.name] ?? {};
  const order = ORDER[command.name];
  const fields =
    order === undefined
      ? command.args
      : [...command.args].sort((a, b) => order.indexOf(a.name) - order.indexOf(b.name));
  return fields.map((field, index) => ({
    field,
    label:
      labels[field.name] ??
      (index === 0 && UNLABELLED_FIRST.has(field.name) && !isOptional(field.type)
        ? "_"
        : field.name),
  }));
}

function parameterLine(parameter: Parameter): string {
  const { field, label } = parameter;
  const external = label === field.name ? "" : `${label} `;
  const fallback = isOptional(field.type) ? " = nil" : "";
  return `${external}${ident(field.name)}: ${swiftType(field.type)}${fallback}`;
}

function documentation(command: Command, hasProgress: boolean): string[] {
  const lines = [
    `\`${command.name}\`. Needs the capability \`${command.capability ?? "none"}\` from a page or a session; a call from Swift is not checked.`,
  ];
  if (command.paginated) {
    lines.push("Paginated: pass the returned `nextCursor` as `cursor` for the next page.");
  }
  if (command.requires.includes("gesture")) {
    lines.push(
      "A page must have been given a user gesture to call it; a call from Swift needs none.",
    );
  }
  if (command.requires.includes("authentication")) {
    lines.push(
      "Asks the operating system to authenticate (Touch ID or the login password) before it runs, whoever calls. A refusal throws `capability.reauthenticationFailed`.",
    );
  }
  if (command.authenticateWhen !== undefined) {
    lines.push(
      `Asks the operating system to authenticate when \`${command.authenticateWhen}\` is given.`,
    );
  }
  if (command.raises !== undefined) {
    lines.push(`Raises a UI request on the \`${command.raises}\` surface.`);
  }
  if (hasProgress) lines.push("`onProgress` runs on the main actor for each progress report.");
  lines.push("Cancelling the task cancels the call (`broker.cancelled`).");
  return lines.map((line) => `/// ${line}`);
}

function renderCommand(command: Command, indent: string): string[] {
  const method = methodName(command);
  const params = parameters(command);
  const lines: string[] = [];
  const result = command.result;
  const resultFields = typeof result === "string" ? undefined : result;
  const resultStruct = resultFields !== undefined && resultFields.length !== 1;
  const resultName = `${pascal(method)}Result`;

  let returns: string;
  if (result === "void") returns = "";
  else if (typeof result === "string") returns = ` -> ${swiftType(result)}`;
  else if (resultFields !== undefined && resultFields.length === 1) {
    returns = ` -> ${swiftType((resultFields[0] as FieldDefinition).type)}`;
  } else returns = ` -> ${resultName}`;

  if (resultStruct && resultFields !== undefined) {
    lines.push(`${indent}public struct ${resultName}: Codable, Sendable, Equatable {`);
    for (const field of resultFields) {
      lines.push(`${indent}    public var ${ident(field.name)}: ${swiftType(field.type)}`);
    }
    lines.push(`${indent}}`, "");
  }

  for (const line of documentation(command, command.progress)) lines.push(`${indent}${line}`);
  const signature = params.map((parameter) => `${indent}    ${parameterLine(parameter)}`);
  if (command.progress) signature.push(`${indent}    ${PROGRESS_PARAMETER}`);
  if (signature.length === 0) {
    lines.push(`${indent}public func ${ident(method)}() async throws${returns} {`);
  } else {
    lines.push(`${indent}public func ${ident(method)}(`);
    lines.push(signature.join(",\n"));
    lines.push(`${indent}) async throws${returns} {`);
  }

  const argumentsValue = renderArguments(params, indent, lines);
  const progress = command.progress ? ", onProgress: onProgress" : "";
  const name = JSON.stringify(command.name);
  const call = (output: string) =>
    `scope.call(${name}, ${argumentsValue}, as: ${output}.self${progress})`;
  if (result === "void") {
    lines.push(`${indent}    try await scope.run(${name}, ${argumentsValue}${progress})`);
  } else if (typeof result === "string") {
    lines.push(`${indent}    return try await ${call(swiftType(result))}`);
  } else if (resultFields !== undefined && resultFields.length === 1) {
    const only = resultFields[0] as FieldDefinition;
    lines.push(
      `${indent}    struct Output: Decodable {`,
      `${indent}        var ${ident(only.name)}: ${swiftType(only.type)}`,
      `${indent}    }`,
      `${indent}    return try await ${call("Output")}.${ident(only.name)}`,
    );
  } else {
    lines.push(`${indent}    return try await ${call(resultName)}`);
  }
  lines.push(`${indent}}`);
  return lines;
}

/** Emits the local `Arguments` struct when there are arguments; returns the expression that builds them. */
function renderArguments(params: readonly Parameter[], indent: string, lines: string[]): string {
  if (params.length === 0) return "BDKVoid()";
  lines.push(`${indent}    struct Arguments: Encodable {`);
  for (const { field } of params) {
    lines.push(`${indent}        var ${ident(field.name)}: ${swiftType(field.type)}`);
  }
  lines.push(`${indent}    }`);
  const values = params.map(({ field }) => `${ident(field.name)}: ${ident(field.name)}`);
  return `Arguments(${values.join(", ")})`;
}

// MARK: events

function renderEvents(
  namespace: string,
  events: readonly EventDefinition[],
  indent: string,
): string[] {
  const lines: string[] = [];
  const cases = events.map((event) => {
    const name = event.name.slice(namespace.length + 1);
    const payload = event.payload;
    // An inline payload becomes a struct nested in the enum; a named type is used as it is.
    const fields = typeof payload === "string" ? undefined : payload;
    const type = fields === undefined ? swiftType(payload as string) : pascal(name);
    const empty = fields !== undefined && fields.length === 0;
    return { event, name, fields, type, empty };
  });
  lines.push(`${indent}/// \`${namespace}.*\` events. Each is delivered with the event's payload.`);
  lines.push(`${indent}public enum Event: Sendable, Equatable {`);
  for (const { name, type, empty } of cases) {
    lines.push(`${indent}    case ${ident(name)}${empty ? "" : `(${type})`}`);
  }
  for (const { fields, type } of cases) {
    if (fields === undefined || fields.length === 0) continue;
    lines.push("", ...renderStruct(type, fields, `${indent}    `));
  }
  const names = cases.map(({ event }) => JSON.stringify(event.name)).join(", ");
  lines.push("", `${indent}    static let names = [${names}]`, "");
  lines.push(
    `${indent}    /// Decodes one delivered event; nil for a name this version does not know.`,
  );
  lines.push(`${indent}    static func decode(name: String, payload: String) throws -> Event? {`);
  lines.push(`${indent}        switch name {`);
  for (const { event, name, type, empty } of cases) {
    lines.push(`${indent}        case ${JSON.stringify(event.name)}:`);
    lines.push(
      empty
        ? `${indent}            return .${ident(name)}`
        : `${indent}            return .${ident(name)}(try BDKServiceCoding.decode(${type}.self, from: payload))`,
    );
  }
  lines.push(`${indent}        default:`, `${indent}            return nil`, `${indent}        }`);
  lines.push(`${indent}    }`, `${indent}}`, "");
  lines.push(
    `${indent}/// The events of this namespace. Reading the property subscribes at once, so events that happen`,
  );
  lines.push(
    `${indent}/// after it are delivered. The stream ends when it is no longer iterated or the runtime shuts down.`,
  );
  lines.push(`${indent}public var events: AsyncStream<Event> {`);
  lines.push(`${indent}    scope.events(Event.names, decode: Event.decode)`);
  lines.push(`${indent}}`);
  return lines;
}

// MARK: assembly

const HEAD = [
  "// swift-format-ignore-file",
  "// Generated by packages/js/codegen from packages/core/interface. Do not edit.",
  "// Regenerate with: pnpm exec nx run js-codegen:codegen",
  "",
  "import Foundation",
  "",
];

/**
 * Renders the Swift wrappers: a type for each named type, a service for each namespace that is not
 * native (`profile.history`, `runtime.settings`, ...), a method per command and an event stream per
 * namespace. Same definitions in, same bytes out.
 */
export function renderSwiftServices(definitions: Definitions): string {
  const out: string[] = [...HEAD];

  out.push("// MARK: - Types", "");
  for (const type of definitions.types) out.push(...renderType(type));

  const commands = definitions.commands.filter(
    (command) =>
      command.callers.includes("swift") &&
      !NATIVE_NAMESPACES.includes(segments(command.name).namespace),
  );
  const namespaces = [
    ...new Set(commands.map((command) => segments(command.name).namespace)),
  ].sort();

  const accessors: { scope: "runtime" | "profile"; namespace: string }[] = [];
  out.push("// MARK: - Services", "");
  for (const namespace of namespaces) {
    const own = commands.filter((command) => segments(command.name).namespace === namespace);
    for (const scope of scopesOf(namespace, own)) accessors.push({ scope, namespace });
    const groups = [...new Set(own.map((command) => segments(command.name).group))].filter(
      (group): group is string => group !== undefined,
    );
    const events = definitions.events.filter((event) => event.name.startsWith(`${namespace}.`));

    const direct = own.filter((command) => segments(command.name).group === undefined);
    const type = serviceType(namespace);
    out.push(`/// The \`${namespace}\` commands.`, "@MainActor", `public struct ${type} {`);
    out.push("    let scope: BDKServiceScope");
    for (const group of groups) {
      out.push("", `    /// The \`${namespace}.${group}\` commands.`);
      out.push(`    public var ${ident(group)}: ${serviceType(namespace, group)} {`);
      out.push(`        ${serviceType(namespace, group)}(scope: scope)`, "    }");
    }
    for (const command of direct) out.push("", ...renderCommand(command, "    "));
    if (events.length > 0) out.push("", ...renderEvents(namespace, events, "    "));
    out.push("}", "");

    for (const group of groups) {
      const grouped = own.filter((command) => segments(command.name).group === group);
      out.push(`/// The \`${namespace}.${group}\` commands.`, "@MainActor");
      out.push(
        `public struct ${serviceType(namespace, group)} {`,
        "    let scope: BDKServiceScope",
      );
      for (const command of grouped) out.push("", ...renderCommand(command, "    "));
      out.push("}", "");
    }
  }

  out.push("// MARK: - Entry points", "");
  for (const holder of ["profile", "runtime"] as const) {
    const mine = accessors.filter((accessor) => accessor.scope === holder);
    if (mine.length === 0) continue;
    out.push(holder === "profile" ? "extension BDKProfile {" : "extension BDKRuntime {");
    mine.forEach(({ namespace }, index) => {
      const type = serviceType(namespace);
      const scope =
        holder === "profile"
          ? "BDKServiceScope(runtime: runtime, profile: self)"
          : "BDKServiceScope(runtime: self, profile: nil)";
      const where = holder === "profile" ? "in this profile" : "of the application";
      if (index > 0) out.push("");
      out.push(`    /// The \`${namespace}\` commands ${where}.`);
      out.push(`    public var ${ident(accessorName(namespace))}: ${type} {`);
      out.push(`        ${type}(scope: ${scope})`, "    }");
    });
    out.push("}", "");
  }

  return `${out.join("\n").replace(/\n+$/, "")}\n`;
}
