import { describe, expect, it } from "vitest";
import { abi, command, history, mutate, problems, sources } from "./test-support.js";
import { parsePolicy, validate } from "./validate.js";

const only = (file: Record<string, unknown>) =>
  problems(["history.json", file], ["abi.json", abi()]);

describe("a valid set", () => {
  it("has no issues and yields the commands, events and constants", () => {
    const result = validate(sources(["abi.json", abi()], ["history.json", history()]), {
      abiVersion: 1,
      policy: { neverForPages: [], neverForSessions: [] },
    });
    expect(result.issues).toEqual([]);
    expect(result.definitions.commands.map((c) => c.name)).toEqual([
      "history.search",
      "history.deleteAll",
    ]);
    expect(result.definitions.events.map((e) => e.name)).toEqual(["history.visited"]);
    expect(result.definitions.constants.map((g) => g.group)).toEqual([
      "surfaces",
      "flags",
      "names",
    ]);
  });
});

describe("files and namespaces", () => {
  it("rejects a file that is not an object", () => {
    expect(problems(["history.json", []])).toEqual(["history.json: $: must be an object"]);
  });

  it("rejects an unknown top-level field", () => {
    expect(only(mutate(history(), (f) => (f.extra = 1)))).toEqual([
      "history.json: $.extra: unknown field",
    ]);
  });

  it("rejects a malformed namespace", () => {
    expect(only(mutate(history(), (f) => (f.namespace = "History")))).toContain(
      "history.json: $.namespace: must match [a-z][a-zA-Z0-9]*",
    );
  });

  it("requires the namespace to match the file name", () => {
    expect(only(mutate(history(), (f) => (f.namespace = "bookmarks")))).toContain(
      'history.json: $.namespace: "bookmarks" does not match the file name "history.json"',
    );
  });

  it("reserves the app namespace", () => {
    expect(problems(["app.json", { namespace: "app", since: 1 }])).toEqual([
      'app.json: $.namespace: "app" is reserved for application commands',
    ]);
  });

  it("requires since", () => {
    expect(only(mutate(history(), (f) => delete f.since))).toEqual([
      "history.json: $.since: is required",
    ]);
  });

  it("rejects a since newer than the ABI version", () => {
    expect(only(mutate(history(), (f) => (f.since = 2)))).toEqual([
      "history.json: $.since: 2 is newer than the current ABI version 1",
    ]);
  });

  it("rejects a command since newer than the ABI version", () => {
    expect(only(mutate(history(), (f) => (command(f).since = 3)))).toEqual([
      "history.json: $.commands[0].since: 3 is newer than the current ABI version 1",
    ]);
  });

  it("rejects a since that is not a positive integer", () => {
    expect(only(mutate(history(), (f) => (f.since = 0)))).toEqual([
      "history.json: $.since: must be an integer of at least 1",
    ]);
  });
});

describe("names", () => {
  it("requires namespace.verb for commands", () => {
    expect(only(mutate(history(), (f) => (command(f).name = "history")))).toEqual([
      'history.json: $.commands[0].name: "history" does not match namespace.verb',
    ]);
    expect(only(mutate(history(), (f) => (command(f).name = "history.Search")))).toEqual([
      'history.json: $.commands[0].name: "history.Search" does not match namespace.verb',
    ]);
  });

  it("requires a command to be in the file's namespace", () => {
    expect(only(mutate(history(), (f) => (command(f).name = "bookmarks.list")))).toEqual([
      'history.json: $.commands[0].name: command "bookmarks.list" is not in the file\'s namespace "history"',
    ]);
  });

  it("allows nested verbs", () => {
    expect(only(mutate(history(), (f) => (command(f).name = "history.entries.list")))).toEqual([]);
  });

  it("refuses names that start with app.", () => {
    expect(only(mutate(history(), (f) => (command(f).name = "app.open")))).toEqual([
      'history.json: $.commands[0].name: "app.open" starts with "app."; application names are declared at run time',
    ]);
  });

  it("requires namespace.level for capabilities", () => {
    expect(only(mutate(history(), (f) => (command(f).capability = "read")))).toEqual([
      'history.json: $.commands[0].capability: "read" does not match namespace.level',
    ]);
  });

  it("refuses capabilities that start with app.", () => {
    expect(only(mutate(history(), (f) => (command(f).capability = "app.sidebar")))).toEqual([
      'history.json: $.commands[0].capability: "app.sidebar" starts with "app."; application names are declared at run time',
    ]);
  });

  it("requires an event capability", () => {
    expect(
      only(mutate(history(), (f) => delete (f.events as Record<string, unknown>[])[0]?.capability)),
    ).toEqual(["history.json: $.events[0].capability: is required"]);
  });

  it("rejects duplicate commands in one file", () => {
    const file = mutate(history(), (f) => (command(f, 1).name = "history.search"));
    expect(only(file)).toEqual([
      'history.json: $.commands[1].name: duplicate command "history.search" (first defined in history.json)',
    ]);
  });

  it("rejects duplicate events", () => {
    const file = mutate(history(), (f) => {
      const events = f.events as unknown[];
      events.push(structuredClone(events[0]));
    });
    expect(only(file)).toEqual([
      'history.json: $.events[1].name: duplicate event "history.visited" (first defined in history.json)',
    ]);
  });

  it("rejects a type defined twice across files", () => {
    const other = { namespace: "bookmarks", since: 1, types: { TimeRange: { start: "int" } } };
    expect(problems(["history.json", history()], ["bookmarks.json", other])).toEqual([
      'bookmarks.json: $.types.TimeRange: duplicate type "TimeRange" (first defined in history.json)',
    ]);
  });
});

describe("types", () => {
  it("rejects an unknown type in args", () => {
    expect(
      only(
        mutate(history(), (f) => ((command(f).args as Record<string, unknown>).range = "Nope?")),
      ),
    ).toEqual(['history.json: $.commands[0].args.range: unknown type "Nope"']);
  });

  it("rejects an unknown type in a result and an event payload", () => {
    const file = mutate(history(), (f) => {
      command(f, 1).result = "Missing[]";
      ((f.events as Record<string, unknown>[])[0] as Record<string, unknown>).payload = "Gone";
    });
    expect(only(file)).toEqual([
      'history.json: $.commands[1].result: unknown type "Missing"',
      'history.json: $.events[0].payload: unknown type "Gone"',
    ]);
  });

  it("rejects an unknown type inside a struct", () => {
    const file = mutate(history(), (f) => {
      (
        (f.types as Record<string, Record<string, unknown>>).TimeRange as Record<string, unknown>
      ).start = "Instant";
    });
    expect(only(file)).toEqual(['history.json: $.types.TimeRange.start: unknown type "Instant"']);
  });

  it("rejects a malformed type expression", () => {
    expect(
      only(
        mutate(history(), (f) => ((command(f).args as Record<string, unknown>).text = "string??")),
      ),
    ).toEqual(['history.json: $.commands[0].args.text: invalid type expression "string??"']);
  });

  it("accepts the primitives with ? and []", () => {
    const args = {
      a: "string",
      b: "int?",
      c: "double[]",
      d: "bool",
      e: "url[]?",
      f: "timestamp",
      g: "bytes",
      h: "json?",
    };
    expect(only(mutate(history(), (f) => (command(f, 1).args = args)))).toEqual([]);
  });

  it("allows void only for results", () => {
    expect(
      only(mutate(history(), (f) => ((command(f).args as Record<string, unknown>).text = "void"))),
    ).toEqual(['history.json: $.commands[0].args.text: unknown type "void"']);
  });

  it("rejects bad type names, field names and enums", () => {
    const file = mutate(history(), (f) => {
      const types = f.types as Record<string, unknown>;
      types.lower = { a: "int" };
      types.String = { a: "int" };
      types.Bad = { BadField: "int" };
      types.Empty = { enum: [] };
      types.Dup = { enum: ["a", "a"] };
      types.Weird = 5;
    });
    expect(only(file)).toEqual([
      "history.json: $.types.lower: type names must match [A-Z][a-zA-Z0-9]*",
      'history.json: $.types.String: "String" is a primitive type name',
      "history.json: $.types.Bad.BadField: field names must match [a-z][a-zA-Z0-9]*",
      "history.json: $.types.Empty.enum: must be a non-empty array of names",
      'history.json: $.types.Dup.enum[1]: duplicate enum value "a"',
      'history.json: $.types.Weird: must be a struct (object), an enum ({ "enum": [...] }) or an alias (string)',
    ]);
  });
});

describe("commands", () => {
  it("requires callers to be a non-empty list of known callers", () => {
    expect(only(mutate(history(), (f) => (command(f).callers = [])))).toEqual([
      "history.json: $.commands[0].callers: must be a non-empty array of swift, page or session",
    ]);
    expect(
      only(mutate(history(), (f) => (command(f).callers = ["swift", "robot", "swift"]))),
    ).toEqual([
      'history.json: $.commands[0].callers[1]: unknown caller "robot"; expected swift, page or session',
      'history.json: $.commands[0].callers[2]: duplicate caller "swift"',
    ]);
  });

  it("requires a known profile mode", () => {
    expect(only(mutate(history(), (f) => (command(f).profile = "maybe")))).toEqual([
      'history.json: $.commands[0].profile: must be one of required, optional, none; got "maybe"',
    ]);
    expect(only(mutate(history(), (f) => delete command(f).profile))).toEqual([
      "history.json: $.commands[0].profile: must be one of required, optional, none; got undefined",
    ]);
  });

  it("checks requires", () => {
    expect(
      only(mutate(history(), (f) => (command(f).requires = ["gesture", "biometrics"]))),
    ).toEqual([
      'history.json: $.commands[0].requires[1]: unknown requirement "biometrics"; expected gesture or authentication',
    ]);
    expect(only(mutate(history(), (f) => (command(f).requires = "gesture")))).toEqual([
      "history.json: $.commands[0].requires: must be an array of gesture or authentication",
    ]);
    expect(
      only(mutate(history(), (f) => (command(f).requires = ["gesture", "authentication"]))),
    ).toEqual([]);
  });

  it("requires raises to name a known surface", () => {
    expect(only(mutate(history(), (f) => (command(f).raises = "nowhere")))).toEqual([
      'history.json: $.commands[0].raises: unknown surface "nowhere"',
    ]);
    expect(only(mutate(history(), (f) => (command(f).raises = "permissions")))).toEqual([]);
  });

  it("checks the behaviour flags", () => {
    expect(only(mutate(history(), (f) => (command(f, 1).progress = "yes")))).toEqual([
      "history.json: $.commands[1].progress: must be true or false",
    ]);
    expect(
      only(
        mutate(history(), (f) =>
          Object.assign(command(f, 1), { progress: true, stream: true, binary: true }),
        ),
      ),
    ).toEqual([]);
  });

  it("requires args and result", () => {
    const file = mutate(history(), (f) => {
      delete command(f, 1).args;
      delete command(f, 1).result;
    });
    expect(only(file)).toEqual([
      "history.json: $.commands[1].args: is required and must be an object of field types",
      "history.json: $.commands[1].result: is required",
    ]);
  });

  it("rejects unknown command fields", () => {
    expect(only(mutate(history(), (f) => (command(f).slow = true)))).toEqual([
      "history.json: $.commands[0].slow: unknown field",
    ]);
  });

  it("requires pagination fields on a paginated command", () => {
    const file = mutate(history(), (f) => {
      command(f, 1).paginated = true;
    });
    expect(only(file)).toEqual([
      "history.json: $.commands[1].args.limit: is required for a paginated command",
      "history.json: $.commands[1].args.cursor: is required for a paginated command",
      "history.json: $.commands[1].result.items: is required for a paginated command",
      "history.json: $.commands[1].result.nextCursor: is required for a paginated command",
    ]);
  });

  it("allows a command without a capability", () => {
    expect(only(mutate(history(), (f) => delete command(f, 1).capability))).toEqual([]);
  });
});

describe("the never-for-pages and never-for-sessions lists", () => {
  it("refuses a page caller on a pattern match", () => {
    const file = mutate(history(), (f) => (command(f).capability = "contents.read"));
    expect(only(file)).toEqual([
      'history.json: $.commands[0].capability: "contents.read" is on the never-for-pages list (contents.*) but callers includes "page"',
    ]);
  });

  it("refuses a page caller on an exact match", () => {
    const file = mutate(history(), (f) => (command(f).capability = "cookies.values"));
    expect(only(file)).toEqual([
      'history.json: $.commands[0].capability: "cookies.values" is on the never-for-pages list (cookies.values) but callers includes "page"',
    ]);
  });

  it("allows the same capability without page", () => {
    const file = mutate(history(), (f) => {
      command(f).capability = "contents.read";
      command(f).callers = ["swift", "session"];
    });
    expect(only(file)).toEqual([]);
  });

  it("does not treat a longer namespace as a match", () => {
    expect(only(mutate(history(), (f) => (command(f).capability = "contentsx.read")))).toEqual([]);
  });

  it("refuses a session caller on a never-for-sessions capability", () => {
    const file = mutate(history(), (f) => (command(f).capability = "passwords.reveal"));
    expect(only(file)).toEqual([
      'history.json: $.commands[0].capability: "passwords.reveal" is on the never-for-sessions list (passwords.reveal) but callers includes "session"',
    ]);
  });
});

describe("constants", () => {
  const withGroup = (change: (group: Record<string, unknown>) => void) =>
    mutate(abi(), (f) =>
      change((f.constants as Record<string, unknown>[])[1] as Record<string, unknown>),
    );
  const run = (file: Record<string, unknown>) => problems(["abi.json", file]);

  it("requires flags to be single bits", () => {
    expect(run(withGroup((g) => ((g.values as Record<string, number>).C = 3)))).toEqual([
      "abi.json: $.constants[1].values.C: 3 must be a single bit (a power of two up to 2147483648)",
    ]);
  });

  it("requires enum values to be small non-negative integers", () => {
    const file = mutate(abi(), (f) => {
      (
        ((f.constants as Record<string, unknown>[])[0] as Record<string, unknown>).values as Record<
          string,
          number
        >
      ).FIND = -1;
    });
    expect(run(file)).toEqual([
      "abi.json: $.constants[0].values.FIND: -1 must be an integer from 0 to 4294967295",
    ]);
  });

  it("requires string values to be printable and unquoted", () => {
    const file = mutate(abi(), (f) => {
      (
        ((f.constants as Record<string, unknown>[])[2] as Record<string, unknown>).values as Record<
          string,
          string
        >
      ).ONE = 'a"b';
    });
    expect(run(file)).toEqual([
      'abi.json: $.constants[2].values.ONE: "a\\"b" must be a non-empty printable string without quotes or backslashes',
    ]);
  });

  it("rejects two names with one value", () => {
    expect(run(withGroup((g) => ((g.values as Record<string, number>).C = 2)))).toEqual([
      "abi.json: $.constants[1].values.C: value 2 is already used by B",
    ]);
  });

  it("rejects a bad prefix, group, kind and name", () => {
    const file = withGroup((g) => {
      g.prefix = "FLAG_";
      g.group = "Flags";
      g.kind = "bits";
      g.values = { lower: 1 };
    });
    expect(run(file)).toEqual([
      "abi.json: $.constants[1].group: must match [a-z][a-zA-Z0-9]*",
      'abi.json: $.constants[1].kind: must be one of string, enum, flags; got "bits"',
      "abi.json: $.constants[1].prefix: must start with BDK_ and end with _, in upper case",
      "abi.json: $.constants[1].values.lower: names must be upper case words joined by underscores",
    ]);
  });

  it("rejects a duplicate group, prefix or macro", () => {
    const file = mutate(abi(), (f) => {
      const groups = f.constants as Record<string, unknown>[];
      groups.push({ group: "flags", kind: "enum", prefix: "BDK_FLAG_", values: { A: 1 } });
    });
    expect(run(file)).toEqual([
      'abi.json: $.constants[3].group: duplicate group "flags" (first defined in abi.json)',
      'abi.json: $.constants[3].prefix: prefix BDK_FLAG_ is already used by group "flags"',
      "abi.json: $.constants[3].values.A: macro BDK_FLAG_A is already defined at abi.json $.constants[1].values.A",
    ]);
  });

  it("rejects a command whose macro collides with a constant", () => {
    const other = mutate(abi(), (f) => {
      (f.constants as unknown[]).push({
        group: "cmd",
        kind: "enum",
        prefix: "BDK_COMMAND_",
        values: { HISTORY_SEARCH: 1 },
      });
    });
    expect(problems(["abi.json", other], ["history.json", history()])).toEqual([
      "history.json: $.commands[0].name: macro BDK_COMMAND_HISTORY_SEARCH is already defined at abi.json $.constants[3].values.HISTORY_SEARCH",
    ]);
  });
});

describe("the policy file", () => {
  it("accepts names and namespace patterns", () => {
    expect(
      parsePolicy("policy.json", { neverForPages: ["a.b", "c.*"], neverForSessions: [] }).issues,
    ).toEqual([]);
  });

  it("rejects anything else", () => {
    const { issues } = parsePolicy("policy.json", { neverForPages: ["*", 3], extra: 1 });
    expect(issues.map((i) => `${i.file}: ${i.path}: ${i.message}`)).toEqual([
      "policy.json: $.extra: unknown field",
      'policy.json: $.neverForPages[0]: "*" is not a capability name or namespace.* pattern',
      "policy.json: $.neverForPages[1]: 3 is not a capability name or namespace.* pattern",
      "policy.json: $.neverForSessions: must be an array of capability names or namespace.* patterns",
    ]);
  });
});
