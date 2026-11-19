import { describe, expect, it } from "vitest";
import { formatIssue } from "./issue.js";
import type { ConstantGroup, Definitions } from "./model.js";
import {
  EMPTY_REGISTRY,
  parseRegistry,
  type Registry,
  reconcile,
  serializeRegistry,
} from "./registry.js";

const flags = (values: ReadonlyArray<readonly [string, number]>): ConstantGroup => ({
  group: "flags",
  kind: "flags",
  prefix: "BDK_FLAG_",
  comment: undefined,
  values,
});

function definitions(
  commands: string[],
  events: string[] = [],
  constants: ConstantGroup[] = [],
): Definitions {
  return {
    commands: commands.map((name) => ({ name, capability: undefined, callers: ["swift"] })),
    events: events.map((name) => ({ name })),
    constants,
  };
}

const messages = (issues: Parameters<typeof formatIssue>[0][]) => issues.map(formatIssue);

describe("assigning identifiers", () => {
  it("gives new commands and events the next free identifier, starting at 1", () => {
    const { registry, issues } = reconcile(
      EMPTY_REGISTRY,
      definitions(["a.one", "a.two"], ["a.changed"]),
    );
    expect(issues).toEqual([]);
    expect(registry.commands).toEqual([
      { name: "a.one", id: 1 },
      { name: "a.two", id: 2 },
    ]);
    expect(registry.events).toEqual([{ name: "a.changed", id: 1 }]);
  });

  it("keeps existing identifiers and appends new ones after the highest", () => {
    const first = reconcile(EMPTY_REGISTRY, definitions(["a.one", "a.two"])).registry;
    const { registry, issues } = reconcile(first, definitions(["a.zero", "a.one", "a.two"]));
    expect(issues).toEqual([]);
    expect(registry.commands).toEqual([
      { name: "a.one", id: 1 },
      { name: "a.two", id: 2 },
      { name: "a.zero", id: 3 },
    ]);
  });

  it("retires a removed command and never gives its identifier to another", () => {
    const first = reconcile(EMPTY_REGISTRY, definitions(["a.one", "a.two"])).registry;
    const retired = reconcile(first, definitions(["a.one"])).registry;
    expect(retired.commands).toEqual([
      { name: "a.one", id: 1 },
      { name: "a.two", id: 2, retired: true },
    ]);
    const later = reconcile(retired, definitions(["a.one", "a.three"])).registry;
    expect(later.commands.map((entry) => [entry.name, entry.id])).toEqual([
      ["a.one", 1],
      ["a.two", 2],
      ["a.three", 3],
    ]);
  });

  it("does not reuse the identifier of a retired command even when it was the highest", () => {
    const first = reconcile(EMPTY_REGISTRY, definitions(["a.one", "a.two"])).registry;
    const retired = reconcile(first, definitions(["a.one"])).registry;
    const later = reconcile(retired, definitions(["a.one", "a.new"])).registry;
    expect(later.commands.find((entry) => entry.name === "a.new")?.id).toBe(3);
  });

  it("refuses to define a retired name again", () => {
    const retired: Registry = {
      ...EMPTY_REGISTRY,
      commands: [{ name: "a.one", id: 1, retired: true }],
    };
    const { issues } = reconcile(retired, definitions(["a.one"]));
    expect(messages(issues)).toEqual([
      'ids.json: $.commands["a.one"]: "a.one" was retired with id 1; a retired name cannot be defined again',
    ]);
  });

  it("treats a rename as a retirement and a new identifier", () => {
    const first = reconcile(EMPTY_REGISTRY, definitions([], ["a.old"])).registry;
    const { registry } = reconcile(first, definitions([], ["a.new"]));
    expect(registry.events).toEqual([
      { name: "a.old", id: 1, retired: true },
      { name: "a.new", id: 2 },
    ]);
  });
});

describe("constants", () => {
  const start = reconcile(
    EMPTY_REGISTRY,
    definitions(
      [],
      [],
      [
        flags([
          ["A", 1],
          ["B", 2],
        ]),
      ],
    ),
  ).registry;

  it("records the stated values in order", () => {
    expect(start.constants.flags).toEqual([
      { name: "A", id: 1 },
      { name: "B", id: 2 },
    ]);
  });

  it("fails when a value is renumbered", () => {
    const { issues } = reconcile(
      start,
      definitions(
        [],
        [],
        [
          flags([
            ["A", 4],
            ["B", 2],
          ]),
        ],
      ),
    );
    expect(messages(issues)).toEqual([
      'ids.json: $.constants.flags["A"]: "A" is defined as 4 but was assigned 1; identifiers are never renumbered',
    ]);
  });

  it("fails when a new name takes a live value", () => {
    const { issues } = reconcile(
      start,
      definitions(
        [],
        [],
        [
          flags([
            ["A", 1],
            ["B", 2],
            ["C", 2],
          ]),
        ],
      ),
    );
    expect(messages(issues)).toEqual([
      'ids.json: $.constants.flags["C"]: value 2 belongs to "B"; identifiers are never reused',
    ]);
  });

  it("fails when a new name takes a retired value", () => {
    const retired = reconcile(start, definitions([], [], [flags([["A", 1]])])).registry;
    expect(retired.constants.flags?.[1]).toEqual({ name: "B", id: 2, retired: true });
    const { issues } = reconcile(
      retired,
      definitions(
        [],
        [],
        [
          flags([
            ["A", 1],
            ["D", 2],
          ]),
        ],
      ),
    );
    expect(messages(issues)).toEqual([
      'ids.json: $.constants.flags["D"]: value 2 belongs to retired "B"; identifiers are never reused',
    ]);
  });

  it("retires every entry of a group that is removed", () => {
    const { registry } = reconcile(start, definitions([]));
    expect(registry.constants.flags).toEqual([
      { name: "A", id: 1, retired: true },
      { name: "B", id: 2, retired: true },
    ]);
  });

  it("appends a new value to an existing group", () => {
    const { registry, issues } = reconcile(
      start,
      definitions(
        [],
        [],
        [
          flags([
            ["A", 1],
            ["B", 2],
            ["C", 4],
          ]),
        ],
      ),
    );
    expect(issues).toEqual([]);
    expect(registry.constants.flags?.map((entry) => entry.name)).toEqual(["A", "B", "C"]);
  });
});

describe("reading and writing the registry", () => {
  it("round-trips through the serialised form", () => {
    const registry = reconcile(
      EMPTY_REGISTRY,
      definitions(["a.one"], ["a.changed"], [flags([["A", 1]])]),
    ).registry;
    const text = serializeRegistry(registry);
    expect(parseRegistry(JSON.parse(text))).toEqual({ registry, issues: [] });
    expect(serializeRegistry(parseRegistry(JSON.parse(text)).registry)).toBe(text);
  });

  it("prints empty lists compactly", () => {
    expect(serializeRegistry(EMPTY_REGISTRY)).toBe(
      '{\n  "commands": [],\n  "events": [],\n  "constants": {}\n}\n',
    );
  });

  it("rejects an identifier assigned twice", () => {
    const { issues } = parseRegistry({
      commands: [
        { name: "a.one", id: 1 },
        { name: "a.two", id: 1 },
      ],
      events: [],
      constants: {},
    });
    expect(messages(issues)).toEqual([
      "ids.json: $.commands[1].id: id 1 is assigned twice; identifiers are never reused",
    ]);
  });

  it("rejects a repeated name, a bad id and unknown fields", () => {
    const { issues } = parseRegistry({
      commands: [
        { name: "a.one", id: 1 },
        { name: "a.one", id: 2 },
        { name: "a.two", id: 0 },
        { name: "a.three", id: 3, retired: false, note: "x" },
      ],
      events: {},
      constants: [],
      other: 1,
    });
    expect(messages(issues)).toEqual([
      "ids.json: $.other: unknown field",
      'ids.json: $.commands[1].name: duplicate name "a.one"',
      "ids.json: $.commands[2].id: must be a positive integer",
      "ids.json: $.commands[3].note: unknown field",
      "ids.json: $.commands[3].retired: must be true when present",
      "ids.json: $.events: must be an array of identifier entries",
      "ids.json: $.constants: must be an object of groups",
    ]);
  });
});
