import { describe, expect, it } from "vitest";
import { BDKError, Connection } from "./connection.js";
import { MockTransport } from "./test-support.js";

function setup() {
  const transport = new MockTransport();
  return { transport, connection: new Connection(transport) };
}

describe("Connection.call", () => {
  it("sends a call frame with ids, args, profile and contents, and resolves the result", async () => {
    const { transport, connection } = setup();
    const first = connection.call("contents.get", {}, { contents: "7" });
    const second = connection.call(
      "contents.create",
      { url: "https://a.test" },
      { profile: "profile-1" },
    );
    expect(transport.sent).toEqual([
      { type: "call", id: 1, command: "contents.get", args: {}, contents: "7" },
      {
        type: "call",
        id: 2,
        command: "contents.create",
        args: { url: "https://a.test" },
        profile: "profile-1",
      },
    ]);
    transport.deliver({ type: "result", id: 2, value: { id: "9" } });
    transport.deliver({ type: "result", id: 1, value: { url: "x" } });
    expect(await first).toEqual({ value: { url: "x" } });
    expect(await second).toEqual({ value: { id: "9" } });
  });

  it("decodes the base64 binary of a result", async () => {
    const { transport, connection } = setup();
    const pending = connection.call("contents.screenshot");
    transport.deliver({ type: "result", id: 1, value: {}, binary: "AQID/w==" });
    expect((await pending).binary).toEqual(Uint8Array.of(1, 2, 3, 255));
  });

  it("rejects with a BDKError carrying domain, code and message", async () => {
    const { transport, connection } = setup();
    const pending = connection.call("contents.get");
    transport.deliver({
      type: "error",
      id: 1,
      error: { domain: "capability", code: "denied", message: "no" },
    });
    const error = await pending.catch((e) => e);
    expect(error).toBeInstanceOf(BDKError);
    expect(error).toMatchObject({ domain: "capability", code: "denied", message: "no" });
  });

  it("reports progress to the call's callback", async () => {
    const { transport, connection } = setup();
    const seen: [number, string][] = [];
    const pending = connection.call(
      "contents.pdf",
      {},
      { onProgress: (f, m) => seen.push([f, m]) },
    );
    transport.deliver({ type: "progress", id: 1, fraction: 0.5, message: "half" });
    transport.deliver({ type: "result", id: 1, value: {} });
    await pending;
    expect(seen).toEqual([[0.5, "half"]]);
  });

  it("cancels through an AbortSignal: sends cancel and rejects with broker.cancelled", async () => {
    const { transport, connection } = setup();
    const controller = new AbortController();
    const pending = connection.call("contents.waitFor", {}, { signal: controller.signal });
    controller.abort();
    const error = await pending.catch((e) => e);
    expect(error).toMatchObject({ domain: "broker", code: "cancelled" });
    expect(transport.sent.at(-1)).toEqual({ type: "cancel", id: 1 });
    // A late result for the cancelled call is ignored.
    transport.deliver({ type: "result", id: 1, value: null });
  });

  it("rejects at once, sending nothing, when the signal is already aborted", async () => {
    const { transport, connection } = setup();
    const error = await connection.call("x.y", {}, { signal: AbortSignal.abort() }).catch((e) => e);
    expect(error).toMatchObject({ domain: "broker", code: "cancelled" });
    expect(transport.sent).toEqual([]);
  });
});

describe("Connection.subscribe", () => {
  it("subscribes, delivers events, and stops after unsubscribe", async () => {
    const { transport, connection } = setup();
    const got: unknown[] = [];
    const subscription = connection.subscribe(
      "contents.navigated",
      (payload, name) => got.push([name, payload]),
      {
        contents: "3",
      },
    );
    expect(transport.sent).toEqual([
      { type: "subscribe", id: 1, event: "contents.navigated", contents: "3" },
    ]);
    transport.deliver({ type: "result", id: 1, value: null });
    await subscription.ready;
    transport.deliver({ type: "event", id: 1, name: "contents.navigated", payload: { url: "u" } });
    subscription.unsubscribe();
    expect(transport.sent.at(-1)).toEqual({ type: "unsubscribe", id: 1 });
    transport.deliver({ type: "event", id: 1, name: "contents.navigated", payload: { url: "v" } });
    expect(got).toEqual([["contents.navigated", { url: "u" }]]);
  });

  it("rejects `ready` when BDK refuses the subscription", async () => {
    const { transport, connection } = setup();
    const subscription = connection.subscribe("nope", () => undefined);
    transport.deliver({
      type: "error",
      id: 1,
      error: { domain: "event", code: "unknown", message: "no such event" },
    });
    await expect(subscription.ready).rejects.toMatchObject({ code: "unknown" });
  });
});

describe("Connection when the connection ends", () => {
  it("rejects pending calls and subscriptions with the closed frame's error", async () => {
    const { transport, connection } = setup();
    const call = connection.call("contents.list");
    const subscription = connection.subscribe("contents.created", () => undefined);
    transport.deliver({
      type: "closed",
      error: { domain: "automation", code: "sessionRevoked", message: "revoked" },
    });
    await expect(call).rejects.toMatchObject({ domain: "automation", code: "sessionRevoked" });
    await expect(subscription.ready).rejects.toMatchObject({ code: "sessionRevoked" });
    expect(transport.closed).toBe(true);
    await expect(connection.call("contents.list")).rejects.toMatchObject({
      code: "sessionRevoked",
    });
  });

  it("rejects pending calls when the transport closes without a closed frame", async () => {
    const { transport, connection } = setup();
    const call = connection.call("contents.list");
    transport.close();
    await expect(call).rejects.toMatchObject({ domain: "broker", code: "closed" });
  });
});
