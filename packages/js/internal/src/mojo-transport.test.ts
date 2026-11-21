import { describe, expect, it, vi } from "vitest";
import { BdkError } from "./errors.js";
import {
  type BrokerBindings,
  createMojoTransport,
  type PageBrokerRemote,
  type PageClientCallbackRouter,
} from "./mojo-transport.js";

interface Harness {
  readonly bindings: BrokerBindings;
  readonly remote: { [K in Exclude<keyof PageBrokerRemote, "$">]: ReturnType<typeof vi.fn> };
  readonly router: () => PageClientCallbackRouter & {
    progress: (callId: bigint, fraction: number, message: string) => void;
    event: (subscriptionId: bigint, payloadJson: string) => void;
  };
  readonly load: ReturnType<typeof vi.fn>;
  /** Closes the broker pipe, as Mojo reports it. */
  readonly disconnect: () => void;
}

function harness(): Harness {
  const connectionErrorListeners: (() => void)[] = [];
  const remote = {
    setClient: vi.fn(),
    invoke: vi.fn(async () => ({ ok: true, resultJson: "null" })),
    cancel: vi.fn(),
    subscribe: vi.fn(async () => ({ ok: true, errorJson: "" })),
    unsubscribe: vi.fn(),
    $: {
      onConnectionError: {
        addListener: (listener: () => void) => {
          connectionErrorListeners.push(listener);
        },
      },
    },
  };
  let created: ReturnType<Harness["router"]> | undefined;
  class Router {
    progress: (callId: bigint, fraction: number, message: string) => void = () => {};
    event: (subscriptionId: bigint, payloadJson: string) => void = () => {};
    onProgress = {
      addListener: (listener: Router["progress"]) => {
        this.progress = listener;
      },
    };
    onEvent = {
      addListener: (listener: Router["event"]) => {
        this.event = listener;
      },
    };
    $ = { bindNewPipeAndPassRemote: () => "client-remote" };
    constructor() {
      created = this as unknown as ReturnType<Harness["router"]>;
    }
  }
  const bindings: BrokerBindings = {
    PageBroker: { getRemote: () => remote as unknown as PageBrokerRemote },
    PageClientCallbackRouter: Router as unknown as BrokerBindings["PageClientCallbackRouter"],
  };
  const load = vi.fn(async () => bindings);
  return {
    bindings,
    remote,
    load,
    router: () => created as ReturnType<Harness["router"]>,
    disconnect: () => {
      for (const listener of connectionErrorListeners) listener();
    },
  };
}

describe("invoke", () => {
  it("sends SetClient once, first, then Invoke with a bigint call id and JSON arguments", async () => {
    const h = harness();
    h.remote.invoke.mockResolvedValue({ ok: true, resultJson: '{"value":5}' });
    const transport = createMojoTransport({ load: h.load as never });
    await expect(transport.invoke("settings.get", { key: "a" })).resolves.toEqual({ value: 5 });
    await transport.invoke("settings.get", { key: "b" });
    expect(h.load).toHaveBeenCalledTimes(1);
    expect(h.remote.setClient).toHaveBeenCalledTimes(1);
    expect(h.remote.setClient).toHaveBeenCalledWith("client-remote");
    expect(h.remote.invoke.mock.calls).toEqual([
      [1n, "settings.get", '{"key":"a"}'],
      [2n, "settings.get", '{"key":"b"}'],
    ]);
    expect(h.remote.setClient.mock.invocationCallOrder[0]).toBeLessThan(
      h.remote.invoke.mock.invocationCallOrder[0] as number,
    );
  });

  it("answers null for a void command", async () => {
    const h = harness();
    const transport = createMojoTransport({ load: h.load as never });
    await expect(transport.invoke("history.deleteAll", {})).resolves.toBeNull();
  });

  it("turns an error answer into a BdkError with domain, code, message and details", async () => {
    const h = harness();
    h.remote.invoke.mockResolvedValue({
      ok: false,
      resultJson: JSON.stringify({
        domain: "capability",
        code: "denied",
        message: "no grant",
        details: { capability: "history.read" },
      }),
    });
    const transport = createMojoTransport({ load: h.load as never });
    const error = (await transport
      .invoke("history.search", {})
      .catch((e: BdkError) => e)) as BdkError;
    expect(error).toBeInstanceOf(BdkError);
    expect(error).toMatchObject({ domain: "capability", code: "denied", message: "no grant" });
    expect(error.details).toEqual({ capability: "history.read" });
  });

  it("reports an error answer that is not an error, and a result that is not JSON", async () => {
    const h = harness();
    const transport = createMojoTransport({ load: h.load as never });
    h.remote.invoke.mockResolvedValueOnce({ ok: false, resultJson: "boom" });
    await expect(transport.invoke("a.b", {})).rejects.toMatchObject({ code: "invalidResponse" });
    h.remote.invoke.mockResolvedValueOnce({ ok: true, resultJson: "{" });
    await expect(transport.invoke("a.b", {})).rejects.toMatchObject({ code: "invalidResponse" });
  });

  it("refuses arguments it cannot serialise", async () => {
    const h = harness();
    const transport = createMojoTransport({ load: h.load as never });
    const circular: Record<string, unknown> = {};
    circular.self = circular;
    await expect(transport.invoke("a.b", circular)).rejects.toMatchObject({
      domain: "broker",
      code: "invalidArguments",
    });
    expect(h.remote.invoke).not.toHaveBeenCalled();
  });

  it("routes progress to the call's onProgress until it ends", async () => {
    const h = harness();
    const transport = createMojoTransport({ load: h.load as never });
    const seen: unknown[] = [];
    let finish: (reply: { ok: boolean; resultJson: string }) => void = () => {};
    h.remote.invoke.mockReturnValue(new Promise((resolve) => (finish = resolve)));
    const call = transport.invoke("privacy.clear", {}, { onProgress: (p) => seen.push(p) });
    await vi.waitFor(() => expect(h.remote.invoke).toHaveBeenCalled());
    h.router().progress(1n, 0.5, "half");
    h.router().progress(9n, 0.1, "someone else's call");
    finish({ ok: true, resultJson: "null" });
    await call;
    h.router().progress(1n, 1, "late");
    expect(seen).toEqual([{ fraction: 0.5, message: "half" }]);
  });

  it("sends Cancel when the signal aborts, and the broker's broker.cancelled answer rejects", async () => {
    const h = harness();
    const transport = createMojoTransport({ load: h.load as never });
    h.remote.invoke.mockReturnValue(
      new Promise((resolve) => {
        h.remote.cancel.mockImplementation(() =>
          resolve({
            ok: false,
            resultJson: '{"domain":"broker","code":"cancelled","message":"cancelled"}',
          }),
        );
      }),
    );
    const controller = new AbortController();
    const call = transport.invoke("downloads.list", {}, { signal: controller.signal });
    await vi.waitFor(() => expect(h.remote.invoke).toHaveBeenCalled());
    controller.abort();
    await expect(call).rejects.toMatchObject({ domain: "broker", code: "cancelled" });
    expect(h.remote.cancel).toHaveBeenCalledWith(1n);
  });

  it("does not call the broker with an aborted signal", async () => {
    const h = harness();
    const transport = createMojoTransport({ load: h.load as never });
    const controller = new AbortController();
    controller.abort();
    await expect(transport.invoke("a.b", {}, { signal: controller.signal })).rejects.toMatchObject({
      code: "cancelled",
    });
    expect(h.remote.invoke).not.toHaveBeenCalled();
  });

  it("rejects with broker.transportUnavailable when the bindings cannot be loaded, and tries again later", async () => {
    const h = harness();
    h.load.mockRejectedValueOnce(new Error("404"));
    const transport = createMojoTransport({ load: h.load as never });
    await expect(transport.invoke("a.b", {})).rejects.toMatchObject({
      domain: "broker",
      code: "transportUnavailable",
    });
    await expect(transport.invoke("a.b", {})).resolves.toBeNull();
  });

  it("reports the default bindings as unavailable outside a page", async () => {
    const transport = createMojoTransport();
    const error = (await transport.invoke("a.b", {}).catch((e: BdkError) => e)) as BdkError;
    expect(error).toMatchObject({ domain: "broker", code: "transportUnavailable" });
    expect(error.message).toContain("/@bdk/page_broker.mojom-webui.js");
  });
});

describe("subscribe", () => {
  it("sends Subscribe with a bigint id, the event and the filter as JSON, and delivers payloads", async () => {
    const h = harness();
    const transport = createMojoTransport({ load: h.load as never });
    const received: unknown[] = [];
    transport.subscribe("settings.changed", { key: "a" }, (payload) => received.push(payload));
    await vi.waitFor(() => expect(h.remote.subscribe).toHaveBeenCalled());
    expect(h.remote.subscribe).toHaveBeenCalledWith(1n, "settings.changed", '{"key":"a"}');
    h.router().event(1n, '{"key":"a","newValue":true}');
    h.router().event(7n, '{"ignored":true}');
    expect(received).toEqual([{ key: "a", newValue: true }]);
  });

  it("sends an empty filter object when there is none", async () => {
    const h = harness();
    const transport = createMojoTransport({ load: h.load as never });
    transport.subscribe("history.visited", undefined, () => {});
    await vi.waitFor(() => expect(h.remote.subscribe).toHaveBeenCalled());
    expect(h.remote.subscribe).toHaveBeenCalledWith(1n, "history.visited", "{}");
  });

  it("sends Unsubscribe and stops delivering after unsubscribe()", async () => {
    const h = harness();
    const transport = createMojoTransport({ load: h.load as never });
    const received: unknown[] = [];
    const subscription = transport.subscribe("a.b", undefined, (p) => received.push(p));
    await vi.waitFor(() => expect(h.remote.subscribe).toHaveBeenCalled());
    subscription.unsubscribe();
    subscription.unsubscribe();
    h.router().event(1n, "1");
    expect(received).toEqual([]);
    expect(h.remote.unsubscribe).toHaveBeenCalledTimes(1);
    expect(h.remote.unsubscribe).toHaveBeenCalledWith(1n);
  });

  it("never subscribes when unsubscribed before the bindings load", async () => {
    const h = harness();
    const transport = createMojoTransport({ load: h.load as never });
    transport.subscribe("a.b", undefined, () => {}).unsubscribe();
    await vi.waitFor(() => expect(h.load).toHaveBeenCalled());
    await Promise.resolve();
    expect(h.remote.subscribe).not.toHaveBeenCalled();
    expect(h.remote.unsubscribe).not.toHaveBeenCalled();
  });

  it("reports a refusal through onError and stops delivering", async () => {
    const h = harness();
    h.remote.subscribe.mockResolvedValue({
      ok: false,
      errorJson: '{"domain":"capability","code":"denied","message":"no"}',
    });
    const transport = createMojoTransport({ load: h.load as never });
    const errors: BdkError[] = [];
    const received: unknown[] = [];
    transport.subscribe(
      "a.b",
      undefined,
      (p) => received.push(p),
      (e) => errors.push(e),
    );
    await vi.waitFor(() => expect(errors).toHaveLength(1));
    expect(errors[0]).toMatchObject({ domain: "capability", code: "denied" });
    h.router().event(1n, "1");
    expect(received).toEqual([]);
  });

  it("ends active subscriptions with transportUnavailable when the connection closes, then reconnects", async () => {
    const h = harness();
    const transport = createMojoTransport({ load: h.load as never });
    const errors: BdkError[] = [];
    const received: unknown[] = [];
    transport.subscribe(
      "a.b",
      undefined,
      (p) => received.push(p),
      (e) => errors.push(e),
    );
    const gone = transport.subscribe(
      "a.c",
      undefined,
      () => {},
      (e) => errors.push(e),
    );
    await vi.waitFor(() => expect(h.remote.subscribe).toHaveBeenCalledTimes(2));
    gone.unsubscribe();
    h.disconnect();
    expect(errors).toHaveLength(1);
    expect(errors[0]).toMatchObject({ code: "transportUnavailable" });
    h.router().event(1n, "1");
    expect(received).toEqual([]);
    await transport.invoke("settings.get", {});
    expect(h.load).toHaveBeenCalledTimes(2);
  });

  it("reports unavailable bindings through onError", async () => {
    const h = harness();
    h.load.mockRejectedValue(new Error("nope"));
    const transport = createMojoTransport({ load: h.load as never });
    const errors: BdkError[] = [];
    transport.subscribe(
      "a.b",
      undefined,
      () => {},
      (e) => errors.push(e),
    );
    await vi.waitFor(() => expect(errors).toHaveLength(1));
    expect(errors[0]).toMatchObject({ code: "transportUnavailable" });
  });
});
