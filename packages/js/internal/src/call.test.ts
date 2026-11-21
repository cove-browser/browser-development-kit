import { afterEach, describe, expect, it, vi } from "vitest";
import { getTransport, invoke, paginate, setTransport, subscribe, type Transport } from "./call.js";
import { BdkError } from "./errors.js";

function transport(invokeImpl: Transport["invoke"]): Transport {
  return { invoke: invokeImpl, subscribe: () => ({ unsubscribe() {} }) };
}

afterEach(() => setTransport(undefined));

describe("invoke", () => {
  it("sends the command and arguments through the transport and returns the result", async () => {
    const send = vi.fn(async () => ({ value: 1 }));
    setTransport(transport(send));
    await expect(invoke("settings.get", { key: "a" })).resolves.toEqual({ value: 1 });
    expect(send).toHaveBeenCalledWith("settings.get", { key: "a" }, undefined);
  });

  it("defaults the arguments to an empty object", async () => {
    const send = vi.fn(async () => null);
    setTransport(transport(send));
    await invoke("history.deleteAll");
    expect(send).toHaveBeenCalledWith("history.deleteAll", {}, undefined);
  });

  it("rejects with broker.cancelled, without a call, when the signal is already aborted", async () => {
    const send = vi.fn(async () => null);
    setTransport(transport(send));
    const controller = new AbortController();
    controller.abort();
    const error = await invoke("a.b", {}, { signal: controller.signal }).catch((e) => e);
    expect(error).toBeInstanceOf(BdkError);
    expect(error).toMatchObject({ domain: "broker", code: "cancelled" });
    expect(send).not.toHaveBeenCalled();
  });

  it("rejects with broker.cancelled when the signal aborts while the call runs", async () => {
    setTransport(transport(() => new Promise(() => {})));
    const controller = new AbortController();
    const pending = invoke("a.b", {}, { signal: controller.signal });
    controller.abort();
    await expect(pending).rejects.toMatchObject({ domain: "broker", code: "cancelled" });
  });

  it("passes the transport's own rejection on and stops listening to the signal", async () => {
    const failure = new BdkError({ domain: "capability", code: "denied", message: "no" });
    setTransport(
      transport(async () => {
        throw failure;
      }),
    );
    const controller = new AbortController();
    await expect(invoke("a.b", {}, { signal: controller.signal })).rejects.toBe(failure);
  });
});

describe("paginate", () => {
  function pages() {
    const send = vi.fn(async (_name: string, args: Readonly<Record<string, unknown>>) => {
      if (args.cursor === undefined) return { items: [1, 2], nextCursor: "c1" };
      if (args.cursor === "c1") return { items: [3], nextCursor: "c2" };
      return { items: [4] };
    });
    setTransport(transport(send));
    return send;
  }

  it("yields the items of every page and follows nextCursor", async () => {
    const send = pages();
    const items: number[] = [];
    for await (const item of paginate<number>("history.search", { text: "a", limit: 2 })) {
      items.push(item);
    }
    expect(items).toEqual([1, 2, 3, 4]);
    expect(send.mock.calls.map((call) => call[1])).toEqual([
      { text: "a", limit: 2 },
      { text: "a", limit: 2, cursor: "c1" },
      { text: "a", limit: 2, cursor: "c2" },
    ]);
  });

  it("asks for nothing until it is iterated, and stops asking when the loop ends", async () => {
    const send = pages();
    const iterable = paginate<number>("history.search");
    expect(send).not.toHaveBeenCalled();
    for await (const _ of iterable) break;
    expect(send).toHaveBeenCalledTimes(1);
  });

  it("stops on an empty cursor", async () => {
    setTransport(transport(async () => ({ items: [1], nextCursor: "" })));
    const items: number[] = [];
    for await (const item of paginate<number>("x.y")) items.push(item);
    expect(items).toEqual([1]);
  });

  it("rejects with broker.invalidResponse when the answer is not a page", async () => {
    setTransport(transport(async () => ({ nope: true })));
    const read = async () => {
      for await (const _ of paginate("x.y")) {
        // Nothing to read.
      }
    };
    await expect(read()).rejects.toMatchObject({ domain: "broker", code: "invalidResponse" });
  });
});

describe("subscribe", () => {
  it("hands the event, listener and filter to the transport and returns its subscription", () => {
    const subscription = { unsubscribe() {} };
    const subscribeImpl = vi.fn(() => subscription);
    setTransport({ invoke: async () => null, subscribe: subscribeImpl });
    const listener = () => {};
    const onError = () => {};
    expect(subscribe("settings.changed", listener, { key: "a" }, onError)).toBe(subscription);
    expect(subscribeImpl).toHaveBeenCalledWith("settings.changed", { key: "a" }, listener, onError);
  });
});

describe("the transport seam", () => {
  it("uses the installed transport, and the default again once it is removed", () => {
    const mine = transport(async () => null);
    setTransport(mine);
    expect(getTransport()).toBe(mine);
    setTransport(undefined);
    expect(getTransport()).not.toBe(mine);
    expect(getTransport()).toBe(getTransport());
  });
});
