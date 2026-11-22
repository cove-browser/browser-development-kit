import { afterEach, describe, expect, it, vi } from "vitest";
import { invoke, setTransport, subscribe } from "./call.js";
import { BdkError } from "./errors.js";
import { createMockTransport, grantsFromResolved, installMockTransport } from "./testing.js";

afterEach(() => setTransport(undefined));

describe("the mock transport's answers", () => {
  it("answers a command with a registered value, through JSON", async () => {
    const mock = createMockTransport().respond("settings.get", { value: true });
    await expect(mock.invoke("settings.get", { key: "a" })).resolves.toEqual({ value: true });
  });

  it("answers null for a void command and undefined", async () => {
    const mock = createMockTransport().respond("history.deleteAll", undefined);
    await expect(mock.invoke("history.deleteAll", {})).resolves.toBeNull();
  });

  it("answers with a function of the arguments and options, which may be async or throw", async () => {
    const mock = createMockTransport()
      .respond("settings.get", async (args: { key: string }) => ({ value: `v-${args.key}` }))
      .respond("settings.reset", () => {
        throw new BdkError({ domain: "broker", code: "invalidArguments", message: "bad" });
      });
    await expect(mock.invoke("settings.get", { key: "x" })).resolves.toEqual({ value: "v-x" });
    await expect(mock.invoke("settings.reset", {})).rejects.toMatchObject({
      code: "invalidArguments",
    });
  });

  it("lets a function report progress through the call options", async () => {
    const mock = createMockTransport().respond("privacy.clear", (_args, options) => {
      (options as { onProgress: (p: unknown) => void }).onProgress({
        fraction: 1,
        message: "done",
      });
      return null;
    });
    const onProgress = vi.fn();
    await mock.invoke("privacy.clear", {}, { onProgress });
    expect(onProgress).toHaveBeenCalledWith({ fraction: 1, message: "done" });
  });

  it("rejects a command with no response as not implemented, and a made-up name as unknown", async () => {
    const mock = createMockTransport();
    await expect(mock.invoke("settings.get", {})).rejects.toMatchObject({
      domain: "broker",
      code: "notImplemented",
    });
    await expect(mock.invoke("settings.nope", {})).rejects.toMatchObject({
      domain: "broker",
      code: "unknownCommand",
    });
  });

  it("rejects an aborted call as cancelled", async () => {
    const mock = createMockTransport().respond("settings.get", {});
    const controller = new AbortController();
    controller.abort();
    await expect(
      mock.invoke("settings.get", {}, { signal: controller.signal }),
    ).rejects.toMatchObject({
      code: "cancelled",
    });
  });
});

describe("recording", () => {
  it("records every call in order, refused ones included", async () => {
    const mock = createMockTransport({ grants: [] }).respond("runtime.caller", {});
    await mock.invoke("runtime.caller", {});
    await mock.invoke("history.deleteAll", { a: 1 }).catch(() => {});
    expect(mock.calls).toEqual([
      { command: "runtime.caller", args: {}, options: undefined },
      { command: "history.deleteAll", args: { a: 1 }, options: undefined },
    ]);
  });
});

describe("grants", () => {
  const mock = () =>
    createMockTransport({ grants: ["history.read", "app.tabs.*"] })
      .respond("history.visits", { visits: [] })
      .respond("history.deleteAll", null)
      .respond("runtime.caller", {})
      .respond("app.tabs.open", null)
      .respond("app.notes.create", null);

  it("allows a command the grants cover", async () => {
    await expect(mock().invoke("history.visits", { url: "a" })).resolves.toEqual({ visits: [] });
  });

  it("refuses a command they do not cover with capability.denied", async () => {
    const error = (await mock()
      .invoke("history.deleteAll", {})
      .catch((e: BdkError) => e)) as BdkError;
    expect(error).toMatchObject({ domain: "capability", code: "denied" });
    expect(error.details).toEqual({ capability: "history.write" });
  });

  it("always allows a command with no capability, such as runtime.caller", async () => {
    await expect(
      createMockTransport({ grants: [] }).respond("runtime.caller", 1).invoke("runtime.caller", {}),
    ).resolves.toBe(1);
  });

  it("checks application commands by their own name and by pattern", async () => {
    const m = mock();
    await expect(m.invoke("app.tabs.open", {})).resolves.toBeNull();
    await expect(m.invoke("app.notes.create", {})).rejects.toMatchObject({ code: "denied" });
  });

  it("checks before the answer exists, as the broker does", async () => {
    const m = createMockTransport({ grants: [] });
    await expect(m.invoke("history.deleteAll", {})).rejects.toMatchObject({ code: "denied" });
  });

  it("allows everything without grants", async () => {
    await expect(
      createMockTransport().respond("history.deleteAll", null).invoke("history.deleteAll", {}),
    ).resolves.toBeNull();
  });
});

describe("events", () => {
  it("delivers emitted payloads to the subscriptions of that event until unsubscribed", () => {
    const mock = createMockTransport();
    const a: unknown[] = [];
    const b: unknown[] = [];
    const first = mock.subscribe("settings.changed", { key: "k" }, (p) => a.push(p));
    mock.subscribe("history.visited", undefined, (p) => b.push(p));
    expect(mock.subscriptions).toEqual([
      { event: "settings.changed", filter: { key: "k" } },
      { event: "history.visited", filter: undefined },
    ]);
    mock.emit("settings.changed", { key: "k", newValue: 1 });
    first.unsubscribe();
    mock.emit("settings.changed", { key: "k", newValue: 2 });
    expect(a).toEqual([{ key: "k", newValue: 1 }]);
    expect(b).toEqual([]);
    expect(mock.subscriptions).toHaveLength(1);
  });

  it("refuses an ungranted subscription through onError, with no delivery", async () => {
    const mock = createMockTransport({ grants: ["settings.read"] });
    const errors: BdkError[] = [];
    const received: unknown[] = [];
    mock.subscribe(
      "history.visited",
      undefined,
      (p) => received.push(p),
      (e) => errors.push(e),
    );
    await vi.waitFor(() => expect(errors).toHaveLength(1));
    expect(errors[0]).toMatchObject({ domain: "capability", code: "denied" });
    mock.emit("history.visited", {});
    expect(received).toEqual([]);
    expect(mock.subscriptions).toEqual([]);
  });

  it("checks application events by name and by pattern", async () => {
    const mock = createMockTransport({ grants: ["app.notes.*"] });
    const errors: BdkError[] = [];
    const received: unknown[] = [];
    mock.subscribe("app.notes.changed", undefined, (p) => received.push(p));
    mock.subscribe(
      "app.tabs.changed",
      undefined,
      () => {},
      (e) => errors.push(e),
    );
    mock.emit("app.notes.changed", 1);
    await vi.waitFor(() => expect(errors).toHaveLength(1));
    expect(received).toEqual([1]);
  });

  it("reports an event that does not exist", async () => {
    const errors: BdkError[] = [];
    createMockTransport().subscribe(
      "history.nope",
      undefined,
      () => {},
      (e) => errors.push(e),
    );
    await vi.waitFor(() => expect(errors[0]).toMatchObject({ code: "unknownCommand" }));
  });
});

describe("installing", () => {
  it("makes the mock the transport of invoke and subscribe, and undoes it", async () => {
    const mock = createMockTransport().respond("settings.get", { value: 7 });
    const uninstall = installMockTransport(mock);
    await expect(invoke("settings.get", { key: "a" })).resolves.toEqual({ value: 7 });
    const seen: unknown[] = [];
    subscribe("settings.changed", (p) => seen.push(p));
    mock.emit("settings.changed", { key: "a" });
    expect(seen).toEqual([{ key: "a" }]);
    uninstall();
    await expect(invoke("settings.get", { key: "a" })).rejects.toMatchObject({
      code: "transportUnavailable",
    });
  });
});

describe("grantsFromResolved", () => {
  const resolved = JSON.stringify({
    routes: {
      settings: { capabilities: ["settings.read", "app.appearance.*"] },
      agents: { capabilities: [] },
    },
    aliases: { preferences: "settings" },
  });

  it("reads the capabilities of a route", () => {
    expect(grantsFromResolved(resolved, "settings")).toEqual(["settings.read", "app.appearance.*"]);
    expect(grantsFromResolved(resolved, "agents")).toEqual([]);
  });

  it("resolves an alias to its target", () => {
    expect(grantsFromResolved(resolved, "preferences")).toEqual([
      "settings.read",
      "app.appearance.*",
    ]);
  });

  it("fails for a route that is not there or has bad capabilities", () => {
    expect(() => grantsFromResolved(resolved, "nope")).toThrowError(/no route or alias "nope"/);
    expect(() =>
      grantsFromResolved(JSON.stringify({ routes: { a: { capabilities: [1] } } }), "a"),
    ).toThrowError(/not a list of names/);
  });

  it("feeds the mock's grants", async () => {
    const mock = createMockTransport({ grants: grantsFromResolved(resolved, "settings") }).respond(
      "settings.get",
      { value: 1 },
    );
    await expect(mock.invoke("settings.get", { key: "a" })).resolves.toEqual({ value: 1 });
    await expect(mock.invoke("settings.set", { key: "a", value: 2 })).rejects.toMatchObject({
      code: "denied",
    });
  });
});
