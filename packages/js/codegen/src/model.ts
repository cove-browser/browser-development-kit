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

export interface Command {
  readonly name: string;
  readonly capability: string | undefined;
  readonly callers: readonly Caller[];
}

export interface EventDefinition {
  readonly name: string;
}

export interface Definitions {
  readonly constants: readonly ConstantGroup[];
  readonly commands: readonly Command[];
  readonly events: readonly EventDefinition[];
}
