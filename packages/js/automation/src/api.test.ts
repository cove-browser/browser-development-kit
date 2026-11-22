import { describe, expect, it } from "vitest";
import { createApi } from "./api.js";
import { answeringConnection } from "./test-support.js";

describe("createApi", () => {
  it("invokes <namespace>.<method> with one arguments object and returns the value", async () => {
    const { connection, transport } = answeringConnection(() => ({ items: [] }));
    const api = createApi(() => connection);
    expect(await api.history.search({ text: "bdk" }, { profile: "profile-1" })).toEqual({
      items: [],
    });
    await api.history.deleteAll();
    expect(transport.calls().map((c) => [c.command, c.args, c.profile])).toEqual([
      ["history.search", { text: "bdk" }, "profile-1"],
      ["history.deleteAll", {}, undefined],
    ]);
  });

  it("calls a command of a group by its dotted name", async () => {
    const { connection, transport } = answeringConnection(() => ({ items: [] }));
    const api = createApi(() => connection);
    expect(await api.autofill.addresses.list()).toEqual({ items: [] });
    expect(transport.calls().map((c) => c.command)).toEqual(["autofill.addresses.list"]);
  });

  it("offers every namespace of the API", () => {
    const { connection } = answeringConnection(() => null);
    const api = createApi(() => connection);
    for (const name of [
      "profiles",
      "history",
      "bookmarks",
      "downloads",
      "settings",
      "permissions",
      "cookies",
      "privacy",
      "extensions",
      "search",
      "passwords",
      "autofill",
    ] as const) {
      expect((api[name] as unknown as Record<string, unknown>).anything).toBeTypeOf("function");
    }
  });

  it("scripts register, unregister and list use scripts.* with the profile", async () => {
    const { connection, transport } = answeringConnection((command) =>
      command === "scripts.list" ? { scripts: [{ id: "a", matches: ["*://*/*"] }] } : null,
    );
    const { scripts } = createApi(() => connection);
    const script = { id: "a", matches: ["*://*/*"], javascript: ["1"] };
    await scripts.register(script, { profile: "profile-1" });
    expect(await scripts.list({ profile: "profile-1" })).toEqual([
      { id: "a", matches: ["*://*/*"] },
    ]);
    await scripts.unregister("a", { profile: "profile-1" });
    expect(transport.calls().map((c) => [c.command, c.args, c.profile])).toEqual([
      ["scripts.register", { script }, "profile-1"],
      ["scripts.list", {}, "profile-1"],
      ["scripts.unregister", { id: "a" }, "profile-1"],
    ]);
  });

  it("commands.execute invokes any command and events.on subscribes with its scope", async () => {
    const { connection, transport } = answeringConnection(() => ({ id: 1 }));
    const { commands, events } = createApi(() => connection);
    expect(await commands.execute("app.notes.create", { title: "t" })).toEqual({ id: 1 });
    const subscription = events.on("contents.navigated", () => undefined, {
      contents: "3",
      profile: "p",
    });
    expect(transport.sent).toEqual([
      { type: "call", id: 1, command: "app.notes.create", args: { title: "t" } },
      { type: "subscribe", id: 2, event: "contents.navigated", contents: "3", profile: "p" },
    ]);
    subscription.unsubscribe();
  });
});
