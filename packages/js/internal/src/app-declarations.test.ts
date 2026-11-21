import { afterEach, describe, expect, expectTypeOf, it } from "vitest";
import type { SettingKey } from "./app-settings.js";
import { setTransport } from "./call.js";
import { commands } from "./commands.js";
import { BdkError } from "./errors.js";
import { events } from "./events.js";
import { createMockTransport, installMockTransport } from "./testing.js";

// What `bdk build codegen` writes for an application (@bdk/internal/app.d.ts), by hand.
declare module "./commands.js" {
  interface AppCommands {
    "app.notes.create": { input: { title: string; body?: string }; output: { id: string } };
    "app.notes.clear": { input: Record<string, never>; output: undefined };
  }
}
declare module "./events.js" {
  interface AppEvents {
    "app.notes.changed": { id: string };
  }
}
declare module "./app-settings.js" {
  interface AppSettings {
    "app.theme": string;
  }
}

afterEach(() => setTransport(undefined));

describe("commands.execute", () => {
  it("runs the command with its input and returns its typed output", async () => {
    const mock = createMockTransport({ grants: ["app.notes.*"] }).respond("app.notes.create", {
      id: "n1",
    });
    installMockTransport(mock);
    const output = await commands.execute("app.notes.create", { title: "a" });
    expectTypeOf(output).toEqualTypeOf<{ id: string }>();
    expect(output).toEqual({ id: "n1" });
    expect(mock.calls[0]?.args).toEqual({ title: "a" });
  });

  it("passes call options on", async () => {
    const mock = createMockTransport().respond("app.notes.clear", null);
    installMockTransport(mock);
    const controller = new AbortController();
    await commands.execute("app.notes.clear", {}, { signal: controller.signal });
    expect(mock.calls[0]?.options?.signal).toBe(controller.signal);
  });

  it("is refused by the broker without the capability", async () => {
    installMockTransport(createMockTransport({ grants: ["app.tabs.*"] }));
    await expect(commands.execute("app.notes.create", { title: "a" })).rejects.toMatchObject({
      domain: "capability",
      code: "denied",
    });
  });

  it("is a compile error to misspell a name or the input, and a rejection to leave app.", async () => {
    installMockTransport(createMockTransport());
    // @ts-expect-error the name is not declared
    const unknown = commands.execute("app.notes.creat", { title: "a" });
    // @ts-expect-error title is required
    const missing = commands.execute("app.notes.create", {});
    // @ts-expect-error a browser command is not an application command
    const browser = commands.execute("history.deleteAll", {});
    await expect(browser).rejects.toBeInstanceOf(BdkError);
    await expect(browser).rejects.toMatchObject({ code: "invalidArguments" });
    await Promise.allSettled([unknown, missing]);
  });
});

describe("events.on", () => {
  it("delivers typed payloads", () => {
    const mock = createMockTransport();
    installMockTransport(mock);
    const seen: string[] = [];
    const subscription = events.on("app.notes.changed", (payload) => {
      expectTypeOf(payload).toEqualTypeOf<{ id: string }>();
      seen.push(payload.id);
    });
    mock.emit("app.notes.changed", { id: "n1" });
    subscription.unsubscribe();
    mock.emit("app.notes.changed", { id: "n2" });
    expect(seen).toEqual(["n1"]);
  });

  it("refuses a name outside app.", () => {
    installMockTransport(createMockTransport());
    // @ts-expect-error not an application event
    expect(() => events.on("history.visited", () => {})).toThrowError(/application event/);
  });
});

describe("setting keys", () => {
  it("complete the application's keys and accept any other string", () => {
    expectTypeOf<"app.theme">().toExtend<SettingKey>();
    expectTypeOf<"privacy.cookies.thirdParty">().toExtend<SettingKey>();
  });
});
