import type { FieldDefinition, TypeDefinition } from "./model.js";
import { PRIMITIVES } from "./model.js";

const TS_PRIMITIVES = [
  "string",
  "number",
  "number",
  "boolean",
  "string",
  "number",
  "string",
  "JsonValue",
] as const;

export function pascal(text: string): string {
  return text.charAt(0).toUpperCase() + text.slice(1);
}

export function pascalName(name: string): string {
  return name.split(".").map(pascal).join("");
}

export function isOptional(expression: string): boolean {
  return expression.endsWith("?");
}

/** The element type of an array expression (`HistoryEntry[]` gives `HistoryEntry`), else undefined. */
export function elementOf(expression: string): string | undefined {
  const bare = isOptional(expression) ? expression.slice(0, -1) : expression;
  return bare.endsWith("[]") ? bare.slice(0, -2) : undefined;
}

/** Collects the named types and local imports a module uses while it renders. */
export class Usage {
  readonly types = new Set<string>();
  readonly values = new Set<string>();
  readonly local = new Set<string>();

  /** The TypeScript spelling of a type expression, without its optional mark. */
  type(expression: string): string {
    const bare = isOptional(expression) ? expression.slice(0, -1) : expression;
    const array = bare.endsWith("[]");
    const base = array ? bare.slice(0, -2) : bare;
    const prim = PRIMITIVES.indexOf(base);
    const name = prim >= 0 ? (TS_PRIMITIVES[prim] as string) : base;
    if (prim < 0 || name === "JsonValue") this.types.add(name);
    return array ? `${name}[]` : name;
  }
}

export function fieldLine(usage: Usage, field: FieldDefinition, nullable: boolean): string {
  const optional = isOptional(field.type);
  const type = usage.type(field.type);
  return `${field.name}${optional ? "?" : ""}: ${type}${optional && nullable ? " | null" : ""};`;
}

export function renderType(type: TypeDefinition, usage: Usage): string[] {
  if (type.kind === "alias") {
    return [`export type ${type.name} = ${usage.type(type.target)};`, ""];
  }
  if (type.kind === "enum") {
    return [
      `export type ${type.name} = ${type.values.map((v) => JSON.stringify(v)).join(" | ")};`,
      "",
    ];
  }
  if (type.fields.length === 0) return [`export type ${type.name} = Record<string, never>;`, ""];
  // A value the engine leaves out may arrive as null, so an optional field of a type admits both.
  return [
    `export interface ${type.name} {`,
    ...type.fields.map((field) => `  ${fieldLine(usage, field, true)}`),
    "}",
    "",
  ];
}
