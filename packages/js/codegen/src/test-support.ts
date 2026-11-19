import { formatIssue } from "./issue.js";
import type { Policy } from "./model.js";
import { type SourceFile, validate } from "./validate.js";

export const POLICY: Policy = {
  neverForPages: ["contents.*", "cookies.values"],
  neverForSessions: ["passwords.reveal"],
};

export type Json = Record<string, unknown>;

/** A valid `history` namespace; tests copy it and break one thing. */
export function history(): Json {
  return {
    namespace: "history",
    since: 1,
    types: {
      HistoryEntry: { url: "url", title: "string", lastVisit: "timestamp" },
      TimeRange: { start: "timestamp?", end: "timestamp?" },
      ChangeSource: { enum: ["user", "sync"] },
    },
    commands: [
      {
        name: "history.search",
        capability: "history.read",
        callers: ["swift", "page", "session"],
        profile: "required",
        args: { text: "string", range: "TimeRange?", limit: "int?", cursor: "string?" },
        result: { items: "HistoryEntry[]", nextCursor: "string?" },
        paginated: true,
      },
      {
        name: "history.deleteAll",
        capability: "history.write",
        callers: ["swift", "page", "session"],
        profile: "required",
        requires: ["gesture"],
        args: {},
        result: "void",
      },
    ],
    events: [
      {
        name: "history.visited",
        capability: "history.read",
        payload: { entry: "HistoryEntry", source: "ChangeSource" },
      },
    ],
  };
}

/** A valid `abi` namespace with a surfaces group and two other groups. */
export function abi(): Json {
  return {
    namespace: "abi",
    since: 1,
    constants: [
      {
        group: "surfaces",
        kind: "enum",
        prefix: "BDK_SURFACE_",
        comment: "UI surfaces.",
        values: { PERMISSIONS: 1, FIND: 2 },
      },
      { group: "flags", kind: "flags", prefix: "BDK_FLAG_", values: { A: 1, B: 2, C: 4 } },
      { group: "names", kind: "string", prefix: "BDK_NAME_", values: { ONE: "one" } },
    ],
  };
}

export function sources(...files: Array<[string, unknown]>): SourceFile[] {
  return files.map(([file, json]) => ({ file, json }));
}

/** The formatted issues for a set of files, with abiVersion 1 and `POLICY`. */
export function problems(...files: Array<[string, unknown]>): string[] {
  return validate(sources(...files), { abiVersion: 1, policy: POLICY }).issues.map(formatIssue);
}

/** Deep copy with one change, so tests do not share state. */
export function mutate(base: Json, change: (copy: Json) => void): Json {
  const copy = structuredClone(base);
  change(copy);
  return copy;
}

export function command(file: Json, index = 0): Json {
  return (file.commands as Json[])[index] as Json;
}
