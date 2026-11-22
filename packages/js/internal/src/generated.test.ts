import { afterEach, describe, expect, expectTypeOf, it } from "vitest";
import { autofill } from "./autofill.js";
import { bookmarks } from "./bookmarks.js";
import { type Subscription, setTransport } from "./call.js";
import { history } from "./history.js";
import { cookies, privacy } from "./privacy.js";
import { settings } from "./settings.js";
import { system } from "./system.js";
import { createMockTransport, installMockTransport } from "./testing.js";
import type { BookmarkNode, HistoryEntry } from "./types.js";

afterEach(() => setTransport(undefined));

const entry = (n: number): HistoryEntry => ({
  url: `https://example.com/${n}`,
  title: `t${n}`,
  lastVisit: n,
  visitCount: 1,
  typedCount: 0,
});

describe("the generated modules", () => {
  it("wrap a command with an args object and unwrap a single-field result", async () => {
    const mock = createMockTransport().respond("settings.get", { value: "block" });
    installMockTransport(mock);
    const value = await settings.get({ key: "privacy.cookies.thirdParty" });
    expectTypeOf(value).toEqualTypeOf<import("./types.js").JsonValue>();
    expect(value).toBe("block");
    expect(mock.calls[0]).toMatchObject({
      command: "settings.get",
      args: { key: "privacy.cookies.thirdParty" },
    });
  });

  it("return a list result as the list", async () => {
    installMockTransport(
      createMockTransport().respond("bookmarks.children", { items: [{ id: "1" }] }),
    );
    const nodes = await bookmarks.children({ id: "0" });
    expectTypeOf(nodes).toEqualTypeOf<BookmarkNode[]>();
    expect(nodes).toEqual([{ id: "1" }]);
  });

  it("resolve void commands to undefined and take only call options when there are no args", async () => {
    installMockTransport(createMockTransport().respond("history.deleteAll", null));
    const controller = new AbortController();
    await expect(history.deleteAll({ signal: controller.signal })).resolves.toBeUndefined();
  });

  it("return an AsyncIterable for a paginated command, following the cursor", async () => {
    const mock = createMockTransport().respond("history.search", (args) =>
      args.cursor === undefined
        ? { items: [entry(1), entry(2)], nextCursor: "next" }
        : { items: [entry(3)] },
    );
    installMockTransport(mock);
    const urls: string[] = [];
    const results = history.search({ text: "example", limit: 2 });
    expectTypeOf(results).toEqualTypeOf<AsyncIterable<HistoryEntry>>();
    for await (const item of results) urls.push(item.url);
    expect(urls).toEqual([
      "https://example.com/1",
      "https://example.com/2",
      "https://example.com/3",
    ]);
    expect(mock.calls.map((call) => call.args)).toEqual([
      { text: "example", limit: 2 },
      { text: "example", limit: 2, cursor: "next" },
    ]);
  });

  it("accept no args at all when every argument is optional", async () => {
    const mock = createMockTransport().respond("history.search", { items: [] });
    installMockTransport(mock);
    for await (const _ of history.search()) {
      // Nothing to read.
    }
    expect(mock.calls[0]?.args).toEqual({});
  });

  it("nest a group of commands under its name", async () => {
    installMockTransport(
      createMockTransport()
        .respond("autofill.cards.list", { items: [] })
        .respond("system.defaultBrowser.status", { isDefault: false, canBeDefault: true }),
    );
    await expect(autofill.cards.list()).resolves.toEqual([]);
    await expect(system.defaultBrowser.status()).resolves.toEqual({
      isDefault: false,
      canBeDefault: true,
    });
  });

  it("put cookies without values and privacy in one module", async () => {
    installMockTransport(createMockTransport().respond("cookies.list", { items: [] }));
    await expect(cookies.list({ site: "example.com" })).resolves.toEqual([]);
    expect("getValues" in cookies).toBe(false);
    expect("set" in cookies).toBe(false);
    expect(typeof privacy.clear).toBe("function");
  });

  it("subscribe through on<Event> and hand back a Subscription", () => {
    const mock = createMockTransport();
    installMockTransport(mock);
    const changes: unknown[] = [];
    const subscription = settings.onChanged((change) => changes.push(change.key), {
      key: "a",
    });
    expectTypeOf(subscription).toEqualTypeOf<Subscription>();
    mock.emit("settings.changed", { key: "a", oldValue: 1, newValue: 2, source: "user" });
    subscription.unsubscribe();
    expect(changes).toEqual(["a"]);
    expect(mock.subscriptions).toEqual([]);
  });

  it("refuse an ungranted call with capability.denied, as at runtime", async () => {
    installMockTransport(createMockTransport({ grants: ["settings.read"] }));
    await expect(settings.set({ key: "a", value: 1 })).rejects.toMatchObject({
      domain: "capability",
      code: "denied",
    });
  });

  it("have no function for a command a page cannot call", () => {
    expect("getValues" in cookies).toBe(false);
  });
});
