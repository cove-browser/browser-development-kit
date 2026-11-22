import { describe, expect, it } from "vitest";
import { createContents, Page, toExpression } from "./page.js";
import { answeringConnection } from "./test-support.js";

function pageWith(
  answer: (command: string, args: Record<string, unknown>) => unknown,
  binary?: string,
) {
  const { connection, transport } = answeringConnection(answer, binary);
  return { page: new Page("42", () => connection), transport, connection };
}

/** The one call a page method made. */
function only(transport: ReturnType<typeof pageWith>["transport"]) {
  const calls = transport.calls();
  expect(calls).toHaveLength(1);
  return calls[0] as (typeof calls)[number];
}

describe("Page", () => {
  it("maps navigation and lifecycle onto contents.* with contents set to the page id", async () => {
    const { page, transport } = pageWith(() => ({ url: "https://a.test", errorPage: false }));
    expect(await page.navigate("https://a.test", { waitUntil: "finished" })).toEqual({
      url: "https://a.test",
      errorPage: false,
    });
    await page.reload({ bypassCache: true });
    await page.back();
    await page.forward();
    await page.close();
    await page.requestAttention();
    expect(transport.calls().map((c) => [c.command, c.args, c.contents])).toEqual([
      ["contents.navigate", { url: "https://a.test", waitUntil: "finished" }, "42"],
      ["contents.reload", { bypassCache: true }, "42"],
      ["contents.goBack", {}, "42"],
      ["contents.goForward", {}, "42"],
      ["contents.close", {}, "42"],
      ["contents.requestAttention", {}, "42"],
    ]);
  });

  it("reads url, title and state from contents.get", async () => {
    const { page, transport } = pageWith(() => ({ url: "https://a.test", title: "T" }));
    expect(await page.url()).toBe("https://a.test");
    expect(await page.title()).toBe("T");
    expect((await page.state()).title).toBe("T");
    expect(
      transport.calls().every((c) => c.command === "contents.get" && c.contents === "42"),
    ).toBe(true);
  });

  it("evaluate builds (fn)(...args) for a function and omits the default world", async () => {
    const { page, transport } = pageWith(() => ({ value: 7 }));
    const result = await page.evaluate((a: number, b: number) => a + b, { args: [3, 4] });
    expect(result).toBe(7);
    const call = only(transport);
    expect(call.command).toBe("contents.evaluate");
    expect(call.args).toEqual({ expression: "((a, b) => a + b)(...[3,4])" });
  });

  it("evaluate passes a string expression as is, with world, frame and userGesture", async () => {
    const { page, transport } = pageWith(() => ({ value: "x" }));
    await page.evaluate("document.title", { world: "page", frame: "f1", userGesture: true });
    expect(only(transport).args).toEqual({
      expression: "document.title",
      world: "page",
      frame: "f1",
      userGesture: true,
    });
    expect(toExpression(() => 1, [])).toBe("(() => 1)(...[])");
  });

  it("waitFor spreads the condition and passes timeoutMs and the signal", async () => {
    const { page, transport } = pageWith(() => null);
    const controller = new AbortController();
    await page.waitFor(
      { load: "finished", selector: "#a" },
      { timeoutMs: 500, signal: controller.signal },
    );
    expect(only(transport).args).toEqual({ load: "finished", selector: "#a", timeoutMs: 500 });
  });

  it("reads: query, text, snapshot, frames", async () => {
    const { page, transport } = pageWith((command) => {
      switch (command) {
        case "contents.query":
          return { elements: [{ tag: "a" }] };
        case "contents.text":
          return { text: "hello" };
        case "contents.snapshot":
          return { kind: "dom", root: {} };
        default:
          return { frames: [{ id: "f" }] };
      }
    });
    expect(await page.query("a", { frame: "f", limit: 3 })).toEqual([{ tag: "a" }]);
    expect(await page.text()).toBe("hello");
    expect(await page.snapshot("dom")).toEqual({ kind: "dom", root: {} });
    expect(await page.frames()).toEqual([{ id: "f" }]);
    expect(transport.calls().map((c) => [c.command, c.args])).toEqual([
      ["contents.query", { selector: "a", frame: "f", limit: 3 }],
      ["contents.text", {}],
      ["contents.snapshot", { kind: "dom" }],
      ["contents.frames", {}],
    ]);
  });

  it("insertCSS returns a handle whose remove() calls contents.removeCSS", async () => {
    const { page, transport } = pageWith(() => ({ id: "css-1" }));
    const css = await page.insertCSS("a{color:red}");
    await css.remove();
    expect(transport.calls().map((c) => [c.command, c.args])).toEqual([
      ["contents.insertCSS", { css: "a{color:red}" }],
      ["contents.removeCSS", { id: "css-1" }],
    ]);
  });

  it("input: click, hover, type, press, scroll", async () => {
    const { page, transport } = pageWith(() => null);
    await page.click("#go", { button: "right", clickCount: 2 });
    await page.click({ x: 1, y: 2 });
    await page.hover("#h");
    await page.type("#in", "text");
    await page.press("Enter", { modifiers: ["Shift"] });
    await page.scroll({ dx: 0, dy: 100 });
    await page.scroll("#end");
    expect(transport.calls().map((c) => [c.command, c.args])).toEqual([
      ["contents.click", { selector: "#go", button: "right", clickCount: 2 }],
      ["contents.click", { x: 1, y: 2 }],
      ["contents.hover", { selector: "#h" }],
      ["contents.type", { selector: "#in", text: "text" }],
      ["contents.press", { key: "Enter", modifiers: ["Shift"] }],
      ["contents.scroll", { dx: 0, dy: 100 }],
      ["contents.scroll", { selector: "#end" }],
    ]);
  });

  it("screenshot decodes the base64 binary into bytes", async () => {
    const { page, transport } = pageWith(() => ({ mimeType: "image/png", byteLength: 3 }), "AQID");
    expect(await page.screenshot({ format: "png", area: "fullPage" })).toEqual(
      Uint8Array.of(1, 2, 3),
    );
    const call = only(transport);
    expect(call).toMatchObject({ command: "contents.screenshot", contents: "42" });
    expect(call.args).toEqual({ format: "png", area: "fullPage" });
  });

  it("pdf returns the bytes and screenshot with a path returns none", async () => {
    const withBytes = pageWith(() => ({}), "/w==");
    expect(await withBytes.page.pdf({ landscape: true })).toEqual(Uint8Array.of(255));
    expect(only(withBytes.transport).command).toBe("contents.pdf");
    const toFile = pageWith(() => ({ path: "/tmp/x.png" }));
    expect(await toFile.page.screenshot({ path: "/tmp/x.png" })).toEqual(new Uint8Array());
  });

  it("translate and revertTranslation call translation.*", async () => {
    const { page, transport } = pageWith(() => null);
    await page.translate("fr");
    await page.revertTranslation();
    expect(transport.calls().map((c) => [c.command, c.args])).toEqual([
      ["translation.translate", { to: "fr" }],
      ["translation.revert", {}],
    ]);
  });
});

describe("Page.debug", () => {
  it("attaches, sends, filters events by session id and detaches", async () => {
    const { page, transport } = pageWith((command) => {
      if (command === "debug.attach") return { session: "dbg-1" };
      if (command === "debug.send") return { result: { ok: true } };
      return null;
    });
    // The subscription is answered by the test, the calls by the mock.
    queueMicrotask(() => transport.deliver({ type: "result", id: 1, value: null }));
    const debug = await page.debug();
    expect(debug.id).toBe("dbg-1");
    expect(transport.sent[0]).toEqual({
      type: "subscribe",
      id: 1,
      event: "debug.event",
      contents: "42",
    });
    const seen: [string, unknown][] = [];
    debug.on((method, params) => seen.push([method, params]));
    transport.deliver({
      type: "event",
      id: 1,
      name: "debug.event",
      payload: { session: "other", method: "Page.x", params: {} },
    });
    transport.deliver({
      type: "event",
      id: 1,
      name: "debug.event",
      payload: { session: "dbg-1", method: "Page.loadEventFired", params: { t: 1 } },
    });
    expect(seen).toEqual([["Page.loadEventFired", { t: 1 }]]);
    expect(await debug.send("Runtime.evaluate", { expression: "1" })).toEqual({ ok: true });
    await debug.detach();
    expect(transport.calls().map((c) => [c.command, c.args, c.contents])).toEqual([
      ["debug.attach", {}, "42"],
      [
        "debug.send",
        { session: "dbg-1", method: "Runtime.evaluate", params: { expression: "1" } },
        "42",
      ],
      ["debug.detach", { session: "dbg-1" }, "42"],
    ]);
    expect(transport.sent).toContainEqual({ type: "unsubscribe", id: 1 });
  });
});

describe("contents", () => {
  it("list returns Pages, get verifies the page, create passes url and profile", async () => {
    const { connection, transport } = answeringConnection((command) =>
      command === "contents.list" ? { items: [{ id: "1" }, { id: "2" }] } : { id: "9" },
    );
    const contents = createContents(() => connection);
    expect((await contents.list({ profile: "profile-1" })).map((p) => p.id)).toEqual(["1", "2"]);
    expect((await contents.get("5")).id).toBe("5");
    const created = await contents.create({ url: "https://a.test", profile: "profile-1" });
    expect(created).toBeInstanceOf(Page);
    expect(created.id).toBe("9");
    expect(transport.calls().map((c) => [c.command, c.args, c.profile, c.contents])).toEqual([
      ["contents.list", {}, "profile-1", undefined],
      ["contents.get", {}, undefined, "5"],
      ["contents.create", { url: "https://a.test" }, "profile-1", undefined],
    ]);
  });
});
