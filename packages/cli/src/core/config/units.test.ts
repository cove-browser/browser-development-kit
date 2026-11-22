import {
  cpSync,
  mkdirSync,
  mkdtempSync,
  readFileSync,
  rmSync,
  symlinkSync,
  writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { describe, expect, it } from "vitest";
import { classifyCapability } from "./capabilities.js";
import { denyReason } from "./denylist.js";
import { resolveConfig, serialiseResolved } from "./index.js";
import { KeyLocator } from "./locator.js";
import { normaliseProjectPath, pathProblem } from "./paths.js";
import { hasBuiltin, isReservedRoute, isValidRouteName, normaliseRouteName } from "./routes.js";
import { assertSchemaValid, nearestKey } from "./schema-check.js";

const repoRoot = fileURLToPath(new URL("../../../../../", import.meta.url));
const schemaUrl = new URL("../../../assets/bdk.schema.json", import.meta.url);
const schema: unknown = JSON.parse(readFileSync(schemaUrl, "utf8"));

describe("KeyLocator", () => {
  const text = [
    "# comment", // 1
    "[browser]", // 2
    'name = "Cove" # trailing', // 3
    "", // 4
    "[pages.overrides]", // 5
    'history = "chromium"', // 6
    '"two words" = "x"', // 7
    "", // 8
    "[pages.capabilities.settings]", // 9
    "allow = [", // 10
    '  "a",', // 11
    '  "b", # note', // 12
    "]", // 13
    'text = """', // 14
    "[not.a.header]", // 15
    '"""', // 16
    "inline = { a = 1, b.c = 2 }", // 17
    "[[items]]", // 18
    "[[items]]", // 19
    "x = 1", // 20
    "top.dotted = 1", // 21
  ].join("\n");
  const locator = new KeyLocator(text);

  it("locates tables, keys, quoted keys and array elements", () => {
    expect(locator.find(["browser"])).toEqual({ line: 2, column: 2 });
    expect(locator.find(["browser", "name"])).toEqual({ line: 3, column: 1 });
    expect(locator.find(["pages", "overrides", "history"])?.line).toBe(6);
    expect(locator.find(["pages", "overrides", "two words"])?.line).toBe(7);
    expect(locator.find(["pages", "capabilities", "settings", "allow", 1])).toEqual({
      line: 12,
      column: 3,
    });
  });

  it("ignores table-like text inside multi-line strings", () => {
    expect(locator.find(["not"])).toBeUndefined();
    expect(locator.find(["pages", "capabilities", "settings", "text"])?.line).toBe(14);
  });

  it("locates inline-table keys, dotted keys and array tables", () => {
    expect(locator.find(["pages", "capabilities", "settings", "inline", "b", "c"])?.line).toBe(17);
    expect(locator.find(["items", 1, "x"])?.line).toBe(20);
    expect(locator.find(["items", 1, "top"])?.line).toBe(21);
  });

  it("falls back to the nearest written ancestor, then the first line", () => {
    expect(locator.findNearest(["browser", "missing"]).line).toBe(2);
    expect(locator.findNearest(["nowhere"])).toEqual({ line: 1, column: 1 });
  });
});

describe("route names", () => {
  it.each([
    ["NewTab", "new-tab"],
    ["Agents", "agents"],
    ["HTMLPage", "html-page"],
    ["Agents2Go", "agents2-go"],
    ["new-tab", "new-tab"],
    ["Net-Export", "net-export"],
  ])("normalises %s to %s", (directory, route) => {
    expect(normaliseRouteName(directory)).toBe(route);
  });

  it("validates names and reserved names", () => {
    expect(isValidRouteName("new-tab")).toBe(true);
    for (const bad of ["New", "9a", "a_b", "-a", ""]) expect(isValidRouteName(bad)).toBe(false);
    expect(isReservedRoute("print")).toBe(true);
    expect(isReservedRoute("bdk-anything")).toBe(true);
    expect(isReservedRoute("agents")).toBe(false);
    expect(hasBuiltin("settings")).toBe(true);
    expect(hasBuiltin("agents")).toBe(false);
  });
});

describe("paths", () => {
  it("accepts paths inside the project and rejects the rest", () => {
    expect(pathProblem("Pages/Agents")).toBeUndefined();
    expect(pathProblem("a/../b")).toBeUndefined();
    expect(pathProblem("../x")).toBeDefined();
    expect(pathProblem("a/../../x")).toBeDefined();
    expect(pathProblem("/abs")).toBeDefined();
    expect(pathProblem("C:/abs")).toBeDefined();
    expect(pathProblem("a\\b")).toBeDefined();
    expect(normaliseProjectPath("./a//b/../c/")).toBe("a/c");
  });
});

describe("capabilities and deny list", () => {
  it("classifies capabilities", () => {
    expect(classifyCapability("settings.read")).toBe("ok");
    expect(classifyCapability("app.sidebar.*")).toBe("ok");
    expect(classifyCapability("app.tabs.open")).toBe("ok");
    expect(classifyCapability("search.*")).toBe("unknown");
    expect(classifyCapability("app.*")).toBe("unknown");
    expect(classifyCapability("contents.*")).toBe("denied");
    expect(classifyCapability("cookies.values")).toBe("denied");
  });

  it("matches switches however they are spelled", () => {
    for (const name of [
      "--no-sandbox",
      "disable_sandbox",
      "DisableWebSecurity",
      "remote-debugging-port",
    ]) {
      expect(denyReason(name), name).toBeDefined();
    }
    for (const name of ["is_debug", "symbol_level", "enable_nacl"]) {
      expect(denyReason(name), name).toBeUndefined();
    }
  });
});

describe("nearestKey", () => {
  it("suggests only close matches", () => {
    expect(nearestKey("nmae", ["name", "internal_scheme"])).toBe("name");
    expect(nearestKey("zzzzzzzz", ["name", "internal_scheme"])).toBeUndefined();
  });
});

describe("the schema", () => {
  it("is a valid strict draft 2020-12 schema", () => {
    expect((schema as { $schema: string }).$schema).toBe(
      "https://json-schema.org/draft/2020-12/schema",
    );
    expect(() => assertSchemaValid(schema as object)).not.toThrow();
  });
});

describe("the surface modes", () => {
  // The runtime core validates the resolved configuration again at start, from its own table
  // (packages/core/runtime/ui_router.cc). The two must agree on every surface the schema names.
  const router = readFileSync(
    new URL("../../../../core/runtime/ui_router.cc", import.meta.url),
    "utf8",
  );
  const coreModes = new Map<string, string[]>();
  for (const [, name, modes] of router.matchAll(/^\s*\{"([a-z_]+)", BDK_SURFACE_\w+, k(DCX?),/gm)) {
    coreModes.set(
      name as string,
      [...(modes as string)].map(
        (m) => ({ D: "default", C: "custom", X: "disabled" })[m] as string,
      ),
    );
  }
  type Defs = Record<string, { enum: string[] }>;
  const defs = (
    schema as {
      $defs: Defs & {
        ui: { properties: { surfaces: { properties: Record<string, { $ref: string }> } } };
      };
    }
  ).$defs;
  const surfaces = defs.ui.properties.surfaces.properties;

  it("reads the core's table", () => expect(coreModes.size).toBeGreaterThan(20));

  it.each(Object.entries(surfaces))(
    "%s allows the same modes in the schema and the core",
    (name, ref) => {
      const allowed = defs[ref.$ref.replace("#/$defs/", "")]?.enum;
      expect(coreModes.get(name), `${name} is missing from the core's table`).toEqual(allowed);
    },
  );

  // [extensions.ui] keys map to the core's extension_<key> surfaces (kExtensionKeys).
  const extensionKeys = [
    ...router.matchAll(/^\s*\{"([a-z_]+)", BDK_SURFACE_EXTENSION_\w+, (?:true|false)\},/gm),
  ].map(([, key]) => key as string);
  const extensionUi = (
    schema as {
      $defs: {
        extensions: { properties: { ui: { properties: Record<string, { $ref: string }> } } };
      };
    }
  ).$defs.extensions.properties.ui.properties;

  it("has the same [extensions.ui] keys in the schema and the core", () =>
    expect([...extensionKeys].sort()).toEqual(Object.keys(extensionUi).sort()));

  it.each(Object.entries(extensionUi))(
    "extensions.ui.%s allows the same modes in the schema and the core",
    (key, ref) => {
      const allowed = defs[ref.$ref.replace("#/$defs/", "")]?.enum;
      expect(coreModes.get(`extension_${key}`)).toEqual(allowed);
    },
  );
});

describe("aliases and routes through the resolver", () => {
  it("resolves an alias to its target and shares the target's capabilities", async () => {
    const result = await resolveConfig({
      projectDir: join(repoRoot, "tests/config/resolve/aliases"),
      schema,
    });
    expect(result.resolved?.aliases).toEqual({
      bots: "agents",
      preferences: "settings",
      zeta: "agents",
    });
    const routes = result.resolved?.routes as Record<string, { capabilities: string[] }>;
    expect(routes.agents?.capabilities).toEqual(["app.agents.*", "search.read"]);
  });
});

describe("byte stability", () => {
  it("resolves the same project to identical bytes, with a trailing newline", async () => {
    const projectDir = join(repoRoot, "tests/config/resolve/complete");
    const first = await resolveConfig({ projectDir, schema });
    const second = await resolveConfig({ projectDir, schema });
    const a = serialiseResolved(first.resolved ?? {});
    expect(a).toBe(serialiseResolved(second.resolved ?? {}));
    expect(a.endsWith("}\n")).toBe(true);
  });

  it("does not depend on where the project lives", async () => {
    const copy = mkdtempSync(join(tmpdir(), "bdk-config-"));
    try {
      const source = join(repoRoot, "tests/config/resolve/complete");
      cpSync(source, copy, { recursive: true });
      const original = await resolveConfig({ projectDir: source, schema });
      const moved = await resolveConfig({ projectDir: copy, schema });
      expect(serialiseResolved(moved.resolved ?? {})).toBe(
        serialiseResolved(original.resolved ?? {}),
      );
    } finally {
      rmSync(copy, { recursive: true, force: true });
    }
  });

  it("changes the digest when a page changes", async () => {
    const copy = mkdtempSync(join(tmpdir(), "bdk-config-"));
    try {
      cpSync(join(repoRoot, "tests/config/resolve/complete"), copy, { recursive: true });
      const before = await resolveConfig({ projectDir: copy, schema });
      const { writeFileSync } = await import("node:fs");
      writeFileSync(
        join(copy, "Pages/Agents/index.html"),
        "<!doctype html>\n<title>changed</title>\n",
      );
      const after = await resolveConfig({ projectDir: copy, schema });
      expect(after.resolved?.config_digest).not.toBe(before.resolved?.config_digest);
    } finally {
      rmSync(copy, { recursive: true, force: true });
    }
  });
});

describe("links and names in project directories", () => {
  async function withProject(
    toml: string,
    build: (dir: string) => void,
    check: (result: Awaited<ReturnType<typeof resolveConfig>>) => void,
  ): Promise<void> {
    const outer = mkdtempSync(join(tmpdir(), "bdk-config-"));
    const dir = join(outer, "project");
    try {
      mkdirSync(dir);
      writeFileSync(join(dir, "bdk.toml"), toml);
      build(dir);
      check(await resolveConfig({ projectDir: dir, schema }));
    } finally {
      rmSync(outer, { recursive: true, force: true });
    }
  }

  const page = (dir: string): string => {
    const path = join(dir, "Pages", "Agents");
    mkdirSync(path, { recursive: true });
    writeFileSync(join(path, "index.html"), "<!doctype html>\n");
    return path;
  };

  it("rejects a link inside a page that leaves the project", async () => {
    await withProject(
      '[browser]\nname = "Cove"\n',
      (dir) => {
        writeFileSync(join(dir, "..", "secret.txt"), "outside\n");
        symlinkSync(join(dir, "..", "secret.txt"), join(page(dir), "secret.txt"));
      },
      (result) => {
        expect(result.resolved).toBeUndefined();
        expect(result.diagnostics.map((d) => [d.code, d.file])).toEqual([
          ["config.path_escape", "Pages/Agents/secret.txt"],
        ]);
      },
    );
  });

  it("rejects a link inside a page that leads back to a directory it is in", async () => {
    await withProject(
      '[browser]\nname = "Cove"\n',
      (dir) => symlinkSync(join(dir, "Pages"), join(page(dir), "loop")),
      (result) => {
        expect(result.resolved).toBeUndefined();
        expect(result.diagnostics.map((d) => [d.code, d.file])).toEqual([
          ["pages.bad_link", "Pages/Agents/loop"],
        ]);
      },
    );
  });

  it("accepts a link inside a page to another place in the project", async () => {
    await withProject(
      '[browser]\nname = "Cove"\n',
      (dir) => {
        mkdirSync(join(dir, "Shared"));
        writeFileSync(join(dir, "Shared", "style.css"), "body {}\n");
        symlinkSync(join(dir, "Shared"), join(page(dir), "shared"));
      },
      (result) => {
        expect(result.diagnostics).toEqual([]);
        expect(Object.keys(result.resolved?.routes as object)).toContain("agents");
      },
    );
  });

  it("keeps a script called __proto__", async () => {
    await withProject(
      '[browser]\nname = "Cove"\n\n[automation]\nenabled = true\n',
      (dir) => {
        mkdirSync(join(dir, "Automation"));
        writeFileSync(join(dir, "Automation", "__proto__.js"), "export {};\n");
        writeFileSync(join(dir, "Automation", "summarise.js"), "export {};\n");
      },
      (result) => {
        expect(result.diagnostics).toEqual([]);
        const automation = result.resolved?.automation as { scripts: object };
        expect(Object.keys(automation.scripts)).toEqual(["__proto__", "summarise"]);
        expect(serialiseResolved(result.resolved ?? {})).toContain('"__proto__": {');
      },
    );
  });
});
