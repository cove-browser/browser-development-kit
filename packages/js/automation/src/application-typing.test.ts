import { execFile } from "node:child_process";
import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { promisify } from "node:util";
import { APP_AUTOMATION_PATH, generateApplication } from "@bdk/codegen";
import { describe, expect, it } from "vitest";

// An application command declared in Swift reaches a script as `bdk.commands.json`; `bdk build codegen`
// writes `@bdk/automation/app.d.ts` from it. This compiles a script against that file with `tsc`, because
// the declaration merges into the package for the whole program and so cannot sit in a test of its own.

const SOURCE = resolve(dirname(fileURLToPath(import.meta.url)));
const ROOT = join(SOURCE, "..", "..", "..", "..");
const run = promisify(execFile);

const COMMANDS = JSON.stringify({
  version: 1,
  commands: [
    {
      name: "app.notes.create",
      args: { title: "string", body: "string?" },
      result: { id: "string" },
    },
    { name: "app.notes.clear", result: "void" },
    {
      name: "app.notes.list",
      args: { tag: "string?" },
      result: "Note[]",
      types: { Note: { id: "string", title: "string" } },
    },
  ],
  events: [{ name: "app.notes.changed", payload: { id: "string" } }],
});
const RESOLVED = JSON.stringify({
  browser: { internal_scheme: "demo" },
  routes: { settings: { mode: "bundled", path: "Pages/Settings" } },
});

/** What each `@bdk` specifier of a script resolves to in the package's source. */
const PATHS: Record<string, string[]> = {
  "@bdk/automation": [join(SOURCE, "index.ts")],
  "@bdk/automation/commands": [join(SOURCE, "commands.ts")],
  "@bdk/automation/events": [join(SOURCE, "events.ts")],
  "@bdk/automation/types": [join(SOURCE, "types.ts")],
  "@bdk/commands": [join(SOURCE, "commands.ts")],
  "@bdk/events": [join(SOURCE, "events.ts")],
};

/** The errors `tsc` reports for a project of `files`, as lines of `file(line,col): error TSxxxx: message`. */
async function diagnose(files: Record<string, string>): Promise<string[]> {
  const directory = await mkdtemp(join(tmpdir(), "bdk-typing-"));
  try {
    for (const [name, text] of Object.entries(files)) await writeFile(join(directory, name), text);
    await writeFile(join(directory, "package.json"), '{"type":"module"}');
    await writeFile(
      join(directory, "tsconfig.json"),
      JSON.stringify({
        extends: join(ROOT, "tsconfig.base.json"),
        compilerOptions: {
          noEmit: true,
          types: ["node"],
          typeRoots: [join(ROOT, "node_modules", "@types")],
          paths: PATHS,
        },
        files: Object.keys(files),
      }),
    );
    try {
      await run("pnpm", [
        "exec",
        "tsc",
        "-p",
        join(directory, "tsconfig.json"),
        "--pretty",
        "false",
      ]);
      return [];
    } catch (error) {
      const { stdout } = error as { stdout: string };
      return stdout.split("\n").filter((line) => line.includes(": error TS"));
    }
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
}

const generated = generateApplication({ commands: COMMANDS, resolved: RESOLVED });
const DECLARATIONS = generated[APP_AUTOMATION_PATH] as string;

const SCRIPT = `
import { contents, history } from "@bdk/automation";
import { commands } from "@bdk/commands";
import { events } from "@bdk/events";

export default async function main(args: { url: string }) {
  const created = await commands.execute("app.notes.create", { title: args.url });
  const id: string = created.id;
  await commands.execute("app.notes.clear");
  const notes = await commands.execute("app.notes.list", { tag: "x" });
  const first: string | undefined = notes[0]?.title;
  const subscription = events.on("app.notes.changed", (payload) => {
    const changed: string = payload.id;
    return changed;
  });
  subscription.unsubscribe();
  await history.search({ text: args.url });
  await commands.execute("history.search", { text: args.url });
  return { id, first, page: await contents.list() };
}
`;

describe("a script against the application's declarations", () => {
  it("writes automation declarations beside the internal ones", () => {
    expect(DECLARATIONS).toContain('declare module "@bdk/automation/commands"');
    expect(DECLARATIONS).toContain('declare module "@bdk/automation/events"');
    expect(DECLARATIONS).not.toContain("@bdk/internal");
  });

  it("compiles when it uses the declared commands, events and types as declared", async () => {
    expect(await diagnose({ "app.d.ts": DECLARATIONS, "script.ts": SCRIPT })).toEqual([]);
  }, 60_000);

  it("fails tsc on a misspelled command, a wrong argument, a wrong result and a wrong payload", async () => {
    const script = `
import { commands } from "@bdk/commands";
import { events } from "@bdk/events";
import { history } from "@bdk/automation";

export default async function main() {
  await commands.execute("app.notes.creat", { title: "t" });
  await commands.execute("app.notes.create", { name: "t" });
  await commands.execute("app.notes.create");
  const id: number = (await commands.execute("app.notes.create", { title: "t" })).id;
  events.on("app.notes.changed", (payload) => payload.title);
  await history.search({ query: "t" });
  return id;
}
`;
    const messages = await diagnose({ "app.d.ts": DECLARATIONS, "script.ts": script });
    expect(messages).toHaveLength(6);
    expect(messages.join("\n")).toContain("app.notes.creat");
    expect(messages.join("\n")).toContain("'name' does not exist");
    expect(messages.join("\n")).toContain("'title' does not exist");
    expect(messages.join("\n")).toContain("'query' does not exist");
  }, 60_000);
});
