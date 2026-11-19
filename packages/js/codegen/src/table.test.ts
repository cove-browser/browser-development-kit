import { describe, expect, it } from "vitest";
import { build, type Inputs } from "./generate.js";
import { abi, history, mutate } from "./test-support.js";

const POLICY = JSON.stringify({
  neverForPages: ["contents.*"],
  neverForSessions: ["passwords.reveal"],
});

function inputs(history_: unknown = history()): Inputs {
  return {
    abiVersion: 1,
    definitions: [
      { file: "history.json", text: JSON.stringify(history_) },
      { file: "abi.json", text: JSON.stringify(abi()) },
    ],
    policy: POLICY,
    registry: undefined,
  };
}

describe("the broker tables", () => {
  it("lists every command with its identifier, capability, callers and marks", () => {
    const { table } = build(inputs());
    expect(table).toContain(
      '{1u, "history.search", "history.read", 7, 2, kPaginated, 0, nullptr, kArgs0, 4},',
    );
    expect(table).toContain(
      '{2u, "history.deleteAll", "history.write", 7, 2, kGesture, 0, nullptr, nullptr, 0},',
    );
    expect(table).toContain('{1u, "history.visited", "history.read"},');
  });

  it("describes argument fields and the types they name", () => {
    const { table } = build(inputs());
    expect(table).toContain('{"text", {Prim::kString, 0, false, false}},');
    expect(table).toContain('{"range", {Prim::kNamed, 1, false, true}},');
    expect(table).toContain('{"HistoryEntry", Kind::kStruct, kTypeFields0, 3,');
    expect(table).toContain('{"ChangeSource", Kind::kEnum, nullptr, 0, kTypeValues2, 2,');
  });

  it("carries the policy lists", () => {
    const { table } = build(inputs());
    expect(table).toContain(
      'inline constexpr const char* kNeverForPages[] = {\n    "contents.*",\n    nullptr,\n};',
    );
    expect(table).toContain('"passwords.reveal"');
  });

  it("carries the surface a command raises, authentication and conditional authentication", () => {
    const changed = mutate(history(), (f) => {
      const [search, deleteAll] = f.commands as Record<string, unknown>[];
      (search as Record<string, unknown>).raises = "permissions";
      (search as Record<string, unknown>).authenticateWhen = "text";
      (deleteAll as Record<string, unknown>).requires = ["gesture", "authentication"];
    });
    const { table } = build({
      ...inputs(changed),
    });
    expect(table).toContain(
      '{1u, "history.search", "history.read", 7, 2, kPaginated, 1, "text", kArgs0, 4},',
    );
    expect(table).toContain("kGesture | kAuthentication");
  });

  it("rejects authenticateWhen naming a field that is not an argument", () => {
    const changed = mutate(history(), (f) => {
      ((f.commands as Record<string, unknown>[])[0] as Record<string, unknown>).authenticateWhen =
        "nothing";
    });
    expect(() => build(inputs(changed))).toThrow("must name a field of args");
  });
});
