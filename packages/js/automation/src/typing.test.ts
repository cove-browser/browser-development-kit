import { describe, expect, expectTypeOf, it } from "vitest";
import { createApi } from "./api.js";
import { createCommands } from "./commands.js";
import { createEvents } from "./events.js";
import { NAMESPACES, type NamespaceName } from "./namespaces.js";
import type { CommandMap, EventMap, Services } from "./services.js";
import { answeringConnection } from "./test-support.js";
import type { HistoryEntry } from "./types.js";

// The cases marked `@ts-expect-error` are the ones `tsc` must refuse: `nx run js-automation:lint` compiles
// this file, so a call that stops being refused fails there.

describe("the service namespaces", () => {
  it("are exactly the namespaces the generated declarations name", () => {
    expectTypeOf<keyof Services>().toEqualTypeOf<NamespaceName>();
    expect(NAMESPACES).toHaveLength(12);
  });

  it("take the arguments of their command, and refuse a wrong argument name", async () => {
    const { connection } = answeringConnection(() => ({ items: [] }));
    const { history, bookmarks, autofill } = createApi(() => connection);
    await history.search({ text: "bdk", limit: 10 });
    await history.search();
    // @ts-expect-error `query` is not an argument of history.search; `text` is
    await history.search({ query: "bdk" });
    // @ts-expect-error `limit` is a number
    await history.search({ limit: "ten" });
    // @ts-expect-error history.visits needs its `url`
    await history.visits({});
    // @ts-expect-error history.visits cannot be called without arguments
    await history.visits();
    // @ts-expect-error bookmarks has no `list`
    await bookmarks.list();
    // @ts-expect-error a group's commands are typed too
    await autofill.addresses.add({ fields: 1 });
  });

  it("resolve with the result of their command", async () => {
    const { connection } = answeringConnection(() => ({ items: [] }));
    const { history } = createApi(() => connection);
    const result = await history.search();
    expectTypeOf(result.items).toEqualTypeOf<HistoryEntry[]>();
    expectTypeOf(await history.deleteAll()).toEqualTypeOf<void>();
  });
});

describe("commands.execute", () => {
  it("takes a browser command by name with its arguments and result", async () => {
    const { connection } = answeringConnection(() => ({ items: [] }));
    const commands = createCommands(() => connection);
    const result = await commands.execute("history.search", { text: "bdk" });
    expectTypeOf(result).toEqualTypeOf<CommandMap["history.search"]["output"]>();
    await commands.execute("history.deleteAll");
    // @ts-expect-error no command has this name
    await commands.execute("history.erase", {});
    // @ts-expect-error `query` is not an argument of history.search
    await commands.execute("history.search", { query: "bdk" });
    // @ts-expect-error history.visits needs its arguments
    await commands.execute("history.visits");
  });

  it("takes any app. name while the application declares none", async () => {
    const { connection } = answeringConnection(() => ({ id: 1 }));
    const commands = createCommands(() => connection);
    expectTypeOf(
      await commands.execute("app.notes.create", { title: "t" }),
    ).toEqualTypeOf<unknown>();
    // @ts-expect-error a name that is neither the browser's nor an app. name
    await commands.execute("notes.create", {});
  });
});

describe("events.on", () => {
  it("types the payload of a browser event by its name", () => {
    const { connection } = answeringConnection(() => null);
    const events = createEvents(() => connection);
    const subscription = events.on("contents.navigated", (payload, name) => {
      expectTypeOf(payload).toEqualTypeOf<EventMap["contents.navigated"]>();
      expectTypeOf(name).toEqualTypeOf<"contents.navigated">();
    });
    subscription.unsubscribe();
    // @ts-expect-error no event has this name
    events.on("contents.navigate", () => undefined).unsubscribe();
    // @ts-expect-error the payload of contents.navigated has no `address`
    events.on("contents.navigated", (payload) => payload.address).unsubscribe();
  });

  it("takes any app. name while the application declares none", () => {
    const { connection } = answeringConnection(() => null);
    const events = createEvents(() => connection);
    events
      .on("app.notes.changed", (payload) => {
        expectTypeOf(payload).toEqualTypeOf<unknown>();
      })
      .unsubscribe();
  });
});
