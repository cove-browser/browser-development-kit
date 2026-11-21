import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { rolldown } from "rolldown";
import { describe, expect, it } from "vitest";
import type { BdkHost } from "./host.js";
import type { ClientFrame, ServerFrame } from "./wire.js";

const here = dirname(fileURLToPath(import.meta.url));

async function bundleFixture(): Promise<string> {
  const build = await rolldown({
    input: resolve(here, "../test/fixtures/summarise.ts"),
    platform: "neutral",
    resolve: {
      alias: {
        "@bdk/automation": resolve(here, "index.ts"),
        "@bdk/commands": resolve(here, "commands.ts"),
        "@bdk/events": resolve(here, "events.ts"),
      },
    },
    logLevel: "silent",
  });
  const { output } = await build.generate({ format: "esm" });
  await build.close();
  const [chunk, ...rest] = output;
  expect(rest).toEqual([]);
  return (chunk as { code: string }).code;
}

describe("bundled script", () => {
  it("is one ES module with no import statements and inlines @bdk/*", async () => {
    const code = await bundleFixture();
    expect(code).not.toMatch(/^\s*import\s/m);
    expect(code).not.toMatch(/\bimport\s*\(/);
    expect(code).not.toMatch(/from\s*["']@bdk\//);
    expect(code).toMatch(/export\s*\{[^}]*as default/);
  });

  it("runs against a simulated V8 host (no real host process involved)", async () => {
    const code = await bundleFixture();
    const sent: ClientFrame[] = [];
    const host: BdkHost = { send: () => undefined, onmessage: null };
    const deliver = (frame: ServerFrame) => host.onmessage?.(JSON.stringify(frame));
    host.send = (text) => {
      const frame = JSON.parse(text) as ClientFrame;
      sent.push(frame);
      // Answer calls and subscriptions the way the core would.
      queueMicrotask(() => {
        if (frame.type === "subscribe") {
          deliver({ type: "result", id: frame.id, value: null });
        } else if (frame.type === "call") {
          const answers: Record<string, unknown> = {
            "contents.create": { id: "11", url: frame.args.url },
            "contents.waitFor": null,
            "contents.text": { text: "hello world" },
            "app.notes.create": { id: "note-1" },
            "contents.close": null,
          };
          if (frame.command in answers) {
            deliver({ type: "result", id: frame.id, value: answers[frame.command] });
          } else {
            deliver({
              type: "error",
              id: frame.id,
              error: { domain: "broker", code: "unknownCommand", message: frame.command },
            });
          }
        }
      });
    };

    // The host installs __bdkHost, evaluates the module, delivers `start`, calls the default export.
    const global = globalThis as { __bdkHost?: BdkHost };
    global.__bdkHost = host;
    try {
      const url = `data:text/javascript;base64,${Buffer.from(code).toString("base64")}`;
      const script = (await import(/* @vite-ignore */ url)) as {
        default: (args: unknown) => Promise<unknown>;
      };
      deliver({
        type: "start",
        protocol: 1,
        task: "task-1",
        script: "summarise",
        arguments: { url: "https://a.test" },
        session: { name: "tester", capabilities: ["app.notes.*"] },
      });
      const result = await script.default({ url: "https://a.test" });

      expect(result).toEqual({
        session: "tester",
        canNotes: true,
        note: { id: "note-1" },
        text: "hello world",
      });
      expect(sent.map((f) => f.type === "call" && f.command)).toEqual(
        expect.arrayContaining([
          "contents.create",
          "contents.waitFor",
          "contents.text",
          "app.notes.create",
          "contents.close",
        ]),
      );
      expect(sent).toContainEqual({ type: "progress", fraction: 0.5, message: "read page" });
      expect(sent).toContainEqual(
        expect.objectContaining({ type: "call", command: "contents.text", contents: "11" }),
      );
      // The library never reports completion itself: the host does.
      expect(sent.some((f) => f.type === "done" || f.type === "failed")).toBe(false);
    } finally {
      delete global.__bdkHost;
    }
  });
});
