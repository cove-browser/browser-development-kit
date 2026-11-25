import type { Command, Definitions, EventDefinition, FieldDefinition } from "./model.js";
import { fieldLine, isOptional, Usage } from "./ts-types.js";
import { HEAD, renderTypes, segments } from "./typescript.js";

/** Where the `@bdk/automation` modules are written, relative to `packages/core`. */
export const AUTOMATION_DIR = "../js/automation/src";

/** The directory the generated modules are reported by, from the repository root. */
export const AUTOMATION_REPORT_DIR = "packages/js/automation/src";

/**
 * Namespaces `@bdk/automation` writes by hand or does not offer as a service namespace: `contents`,
 * `scripts` and `debug` are `contents`, `scripts` and `Page`; `runtime` is `session`; `translation` is
 * `Page.translate`; `system` is the application's. Their commands are still named in `CommandMap`.
 */
export const HAND_WRITTEN_AUTOMATION_NAMESPACES: readonly string[] = [
  "contents",
  "scripts",
  "debug",
  "runtime",
  "translation",
  "system",
];

/** Only a command a session may call can be called from a script or an external tool. */
function sessionCommands(definitions: Definitions): Command[] {
  return definitions.commands.filter((command) => command.callers.includes("session"));
}

function inputType(command: Command, usage: Usage): string {
  if (command.args.length === 0) return "Record<string, never>";
  return `{ ${command.args.map((field) => fieldLine(usage, field, false)).join(" ")} }`;
}

function outputType(result: string | readonly FieldDefinition[], usage: Usage): string {
  if (typeof result === "string") return result === "void" ? "void" : usage.type(result);
  if (result.length === 0) return "void";
  return `{ ${result.map((field) => fieldLine(usage, field, true)).join(" ")} }`;
}

function payloadType(event: EventDefinition, usage: Usage): string {
  if (typeof event.payload === "string") return usage.type(event.payload);
  if (event.payload.length === 0) return "Record<string, never>";
  return `{ ${event.payload.map((field) => fieldLine(usage, field, true)).join(" ")} }`;
}

function renderCommandMap(commands: readonly Command[], usage: Usage): string[] {
  return [
    "/** Every command a session may call: name to the arguments it takes and the result it returns. */",
    "export interface CommandMap {",
    ...commands.map(
      (command) =>
        `  ${JSON.stringify(command.name)}: { input: ${inputType(command, usage)}; output: ${outputType(command.result, usage)} };`,
    ),
    "}",
    "",
    'export type CommandInput<N extends keyof CommandMap> = CommandMap[N]["input"];',
    'export type CommandOutput<N extends keyof CommandMap> = CommandMap[N]["output"];',
    "",
  ];
}

function renderEventMap(events: readonly EventDefinition[], usage: Usage): string[] {
  return [
    "/** Every browser event a session may subscribe to: name to its payload. */",
    "export interface EventMap {",
    ...events.map((event) => `  ${JSON.stringify(event.name)}: ${payloadType(event, usage)};`),
    "}",
    "",
  ];
}

function renderMethod(command: Command): string[] {
  const name = JSON.stringify(command.name);
  const { method } = segments(command.name);
  const optional = command.args.every((field) => isOptional(field.type)) ? "?" : "";
  return [
    `/** \`${command.name}\`. Needs the capability \`${command.capability ?? "none"}\`. */`,
    `${method}(args${optional}: CommandInput<${name}>, options?: CallOptions): Promise<CommandOutput<${name}>>;`,
  ];
}

function renderService(namespace: string, commands: readonly Command[]): string[] {
  const indent = (lines: readonly string[], by: string) => lines.map((line) => `${by}${line}`);
  const direct = commands.filter((command) => segments(command.name).group === undefined);
  const groups = [...new Set(commands.map((command) => segments(command.name).group))].filter(
    (group): group is string => group !== undefined,
  );
  const body = direct.flatMap((command) => indent(renderMethod(command), "  "));
  for (const group of groups) {
    body.push(`  ${group}: {`);
    for (const command of commands.filter((c) => segments(c.name).group === group)) {
      body.push(...indent(renderMethod(command), "    "));
    }
    body.push("  };");
  }
  const type = `${namespace.charAt(0).toUpperCase()}${namespace.slice(1)}Service`;
  return [`export interface ${type} {`, ...body, "}", ""];
}

/**
 * Renders the `@bdk/automation` modules, as file name to text: `types.ts` (every named type) and
 * `services.ts` (`CommandMap`, `EventMap`, and an interface for each service namespace, gathered in
 * `Services`). Types only: the runtime stays the one proxy in `namespaces.ts`. Same definitions in, same
 * bytes out.
 */
export function renderAutomationModules(definitions: Definitions): Record<string, string> {
  const commands = sessionCommands(definitions);
  const namespaces = [...new Set(commands.map((command) => segments(command.name).namespace))]
    .filter((namespace) => !HAND_WRITTEN_AUTOMATION_NAMESPACES.includes(namespace))
    .sort();
  const usage = new Usage();
  usage.types.add("CallOptions");
  const body = [
    ...renderCommandMap(commands, usage),
    ...renderEventMap(definitions.events, usage),
    ...namespaces.flatMap((namespace) =>
      renderService(
        namespace,
        commands.filter((command) => segments(command.name).namespace === namespace),
      ),
    ),
    "/** The service namespaces of a session, by name. */",
    "export interface Services {",
    ...namespaces.map(
      (namespace) =>
        `  ${namespace}: ${namespace.charAt(0).toUpperCase()}${namespace.slice(1)}Service;`,
    ),
    "}",
  ];
  const dataTypes = [...usage.types].filter((name) => name !== "CallOptions").sort();
  const services = [
    ...HEAD,
    'import type { CallOptions } from "./connection.js";',
    ...(dataTypes.length > 0 ? [`import type { ${dataTypes.join(", ")} } from "./types.js";`] : []),
    "",
    ...body,
  ];
  return {
    "types.ts": renderTypes(definitions),
    "services.ts": `${services.join("\n").replace(/\n+$/, "")}\n`,
  };
}
