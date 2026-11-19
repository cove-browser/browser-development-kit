/** The checked shape of an interface definition. Produced by `validate`, consumed by the generators. */

export type Caller = "swift" | "page" | "session";
export type ProfileMode = "required" | "optional" | "none";
export type Requirement = "gesture" | "authentication";
export type ConstantKind = "string" | "enum" | "flags";

export const CALLERS: readonly Caller[] = ["swift", "page", "session"];
export const PROFILE_MODES: readonly ProfileMode[] = ["required", "optional", "none"];
export const REQUIREMENTS: readonly Requirement[] = ["gesture", "authentication"];
export const CONSTANT_KINDS: readonly ConstantKind[] = ["string", "enum", "flags"];
export const PRIMITIVES: readonly string[] = [
  "string",
  "int",
  "double",
  "bool",
  "url",
  "timestamp",
  "bytes",
  "json",
];

/** Capabilities that may not be granted to a page, or held by a session (data, in `policy.json`). */
export interface Policy {
  readonly neverForPages: readonly string[];
  readonly neverForSessions: readonly string[];
}

export interface ConstantGroup {
  readonly group: string;
  readonly kind: ConstantKind;
  readonly prefix: string;
  readonly comment: string | undefined;
  /** Suffix after the prefix, then the value: a string for `string` groups, a number otherwise. */
  readonly values: ReadonlyArray<readonly [string, string | number]>;
}

/** One field of a struct, an argument list or a result: its name and its type expression. */
export interface FieldDefinition {
  readonly name: string;
  readonly type: string;
}

export interface Command {
  readonly name: string;
  readonly capability: string | undefined;
  readonly callers: readonly Caller[];
  readonly profile: ProfileMode;
  readonly requires: readonly Requirement[];
  /** Lower-case surface name, as in the `surfaces` constants group. */
  readonly raises: string | undefined;
  readonly paginated: boolean;
  readonly progress: boolean;
  readonly stream: boolean;
  readonly binary: boolean;
  /** An argument field that, when present, makes the call need authentication (services.md 2). */
  readonly authenticateWhen: string | undefined;
  readonly args: readonly FieldDefinition[];
  /** A struct body, or a type expression (`void` included). */
  readonly result: string | readonly FieldDefinition[];
}

export interface EventDefinition {
  readonly name: string;
  readonly capability: string;
}

export type TypeDefinition =
  | { readonly name: string; readonly kind: "struct"; readonly fields: readonly FieldDefinition[] }
  | { readonly name: string; readonly kind: "enum"; readonly values: readonly string[] }
  | { readonly name: string; readonly kind: "alias"; readonly target: string };

export interface Definitions {
  readonly constants: readonly ConstantGroup[];
  readonly types: readonly TypeDefinition[];
  readonly commands: readonly Command[];
  readonly events: readonly EventDefinition[];
}
