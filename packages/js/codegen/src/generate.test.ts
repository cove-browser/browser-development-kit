import { mkdirSync, mkdtempSync, readFileSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { describe, expect, it } from "vitest";
import { build, check, generate, HEADER_PATH, type Inputs, readInputs } from "./generate.js";
import { InterfaceError } from "./issue.js";
import { SWIFT_PATH, SWIFT_REPORT_PATH } from "./swift.js";
import { TABLE_PATH } from "./table.js";
import { abi, history, mutate } from "./test-support.js";

const POLICY = JSON.stringify({ neverForPages: ["contents.*"], neverForSessions: [] });

function inputs(registry?: string): Inputs {
  return {
    abiVersion: 1,
    definitions: [
      { file: "history.json", text: JSON.stringify(history()) },
      { file: "abi.json", text: JSON.stringify(abi()) },
    ],
    policy: POLICY,
    registry,
  };
}

describe("build", () => {
  it("generates the header and the registry from a valid set", () => {
    const { header, registry } = build(inputs());
    expect(header).toContain("#define BDK_SURFACE_PERMISSIONS 1u\n");
    expect(header).toContain("#define BDK_FLAG_C 0x4u\n");
    expect(header).toContain('#define BDK_NAME_ONE "one"\n');
    expect(header).toContain("#define BDK_COMMAND_HISTORY_SEARCH 1u\n");
    expect(header).toContain("#define BDK_COMMAND_HISTORY_DELETE_ALL 2u\n");
    expect(header).toContain("#define BDK_EVENT_HISTORY_VISITED 1u\n");
    expect(JSON.parse(registry).commands).toEqual([
      { name: "history.search", id: 1 },
      { name: "history.deleteAll", id: 2 },
    ]);
  });

  it("is byte-stable: the same input gives the same bytes, and so does feeding the output back", () => {
    const first = build(inputs());
    expect(build(inputs())).toEqual(first);
    expect(build(inputs(first.registry))).toEqual(first);
  });

  it("does not depend on the order the files are given in", () => {
    const reversed = { ...inputs(), definitions: [...inputs().definitions].reverse() };
    expect(build(reversed)).toEqual(build(inputs()));
  });

  it("drops a retired command from the header but keeps it in the registry", () => {
    const first = build(inputs());
    const trimmed = mutate(history(), (f) => (f.commands as unknown[]).shift());
    const next = build({
      ...inputs(first.registry),
      definitions: [
        { file: "history.json", text: JSON.stringify(trimmed) },
        { file: "abi.json", text: JSON.stringify(abi()) },
      ],
    });
    expect(next.header).not.toContain("BDK_COMMAND_HISTORY_SEARCH");
    expect(next.header).toContain("#define BDK_COMMAND_HISTORY_DELETE_ALL 2u\n");
    expect(JSON.parse(next.registry).commands[0]).toEqual({
      name: "history.search",
      id: 1,
      retired: true,
    });
  });

  it("fails with every issue, naming file and path", () => {
    const broken = mutate(history(), (f) => {
      ((f.commands as Record<string, unknown>[])[0] as Record<string, unknown>).capability =
        "contents.read";
    });
    const run = () =>
      build({ ...inputs(), definitions: [{ file: "history.json", text: JSON.stringify(broken) }] });
    expect(run).toThrow(InterfaceError);
    expect(run).toThrow(
      'history.json: $.commands[0].capability: "contents.read" is on the never-for-pages list (contents.*) but callers includes "page"',
    );
  });

  it("reports invalid JSON", () => {
    const run = () => build({ ...inputs(), definitions: [{ file: "history.json", text: "{" }] });
    expect(run).toThrow(/^history\.json: \$: invalid JSON: /);
  });

  it("fails when the registry disagrees with the definitions", () => {
    const first = build(inputs());
    const renumbered = JSON.stringify(
      mutate(JSON.parse(first.registry), (r) => {
        const group = (r.constants as Record<string, Record<string, unknown>[]>).surfaces;
        (group?.[0] as Record<string, unknown>).id = 9;
      }),
    );
    expect(() => build(inputs(renumbered))).toThrow(
      'ids.json: $.constants.surfaces["PERMISSIONS"]: "PERMISSIONS" is defined as 1 but was assigned 9; identifiers are never renumbered',
    );
  });
});

describe("generate and check on disk", () => {
  function fixture(): string {
    // The Swift wrappers go to a sibling of the core directory, as in the repository.
    const core = join(mkdtempSync(join(tmpdir(), "bdk-codegen-")), "packages", "core");
    mkdirSync(join(core, "interface"), { recursive: true });
    mkdirSync(join(core, "include", "bdk"), { recursive: true });
    writeFileSync(join(core, "include", "bdk", "bdk_base.h"), "#define BDK_ABI_VERSION 1\n");
    writeFileSync(join(core, "interface", "policy.json"), POLICY);
    writeFileSync(join(core, "interface", "history.json"), JSON.stringify(history()));
    writeFileSync(join(core, "interface", "abi.json"), JSON.stringify(abi()));
    return core;
  }

  it("writes both files once and then reports nothing to do", () => {
    const core = fixture();
    expect(generate(core)).toHaveLength(4);
    expect(generate(core)).toEqual([]);
    expect(check(core)).toEqual([]);
  });

  it("names the stale outputs", () => {
    const core = fixture();
    expect(check(core)).toEqual([
      "packages/core/interface/ids.json",
      `packages/core/${HEADER_PATH}`,
      `packages/core/${TABLE_PATH}`,
      SWIFT_REPORT_PATH,
    ]);
    generate(core);
    writeFileSync(join(core, HEADER_PATH), "edited\n");
    writeFileSync(join(core, TABLE_PATH), "edited\n");
    writeFileSync(join(core, SWIFT_PATH), "edited\n");
    expect(check(core)).toEqual([
      `packages/core/${HEADER_PATH}`,
      `packages/core/${TABLE_PATH}`,
      SWIFT_REPORT_PATH,
    ]);
  });

  it("goes stale when a definition changes", () => {
    const core = fixture();
    generate(core);
    const changed = mutate(abi(), (f) => {
      ((f.constants as Record<string, unknown>[])[1] as Record<string, unknown>).values = {
        A: 1,
        B: 2,
        C: 4,
        D: 8,
      };
    });
    writeFileSync(join(core, "interface", "abi.json"), JSON.stringify(changed));
    expect(check(core)).toEqual([
      "packages/core/interface/ids.json",
      `packages/core/${HEADER_PATH}`,
    ]);
  });

  it("goes stale when a command's checks change", () => {
    const core = fixture();
    generate(core);
    const changed = mutate(history(), (f) => {
      ((f.commands as Record<string, unknown>[])[0] as Record<string, unknown>).requires = [
        "gesture",
      ];
    });
    writeFileSync(join(core, "interface", "history.json"), JSON.stringify(changed));
    expect(check(core)).toEqual([`packages/core/${TABLE_PATH}`, SWIFT_REPORT_PATH]);
  });

  it("goes stale when a command's Swift signature changes", () => {
    const core = fixture();
    generate(core);
    const changed = mutate(history(), (f) => {
      ((f.commands as Record<string, unknown>[])[0] as Record<string, unknown>).args = {
        text: "string?",
        limit: "int?",
        cursor: "string?",
      };
    });
    writeFileSync(join(core, "interface", "history.json"), JSON.stringify(changed));
    expect(check(core)).toContain(SWIFT_REPORT_PATH);
  });
});

describe("the committed interface", () => {
  const core = join(dirname(fileURLToPath(import.meta.url)), "..", "..", "..", "core");

  it("generates exactly the committed bdk_ids.h and ids.json", () => {
    expect(check(core)).toEqual([]);
  });

  it("writes comments that the C compiler and the ABI hash normaliser both accept", () => {
    const header = build(readInputs(core)).header;
    const withoutComments = header.replace(/\/\*([^*]|\*+[^*/])*\*+\//g, "");
    expect(withoutComments).not.toMatch(/\/\*|\*\/|\/\//);
  });

  it("generates the committed broker tables", () => {
    const committed = readFileSync(join(core, TABLE_PATH), "utf8");
    expect(committed.startsWith("/* BDK broker tables. Generated by")).toBe(true);
    expect(build(readInputs(core)).table).toBe(committed);
  });

  it("generates the committed Swift wrappers", () => {
    const committed = readFileSync(join(core, SWIFT_PATH), "utf8");
    expect(committed).toContain("// Generated by packages/js/codegen");
    expect(build(readInputs(core)).swift).toBe(committed);
  });

  it("generates the header twice with the same bytes", () => {
    const committed = readFileSync(join(core, HEADER_PATH), "utf8");
    expect(committed.startsWith("/* BDK C ABI: identifiers. Generated by")).toBe(true);
    expect(build(readInputs(core)).header).toBe(committed);
    expect(build(readInputs(core)).header).toBe(committed);
  });
});
