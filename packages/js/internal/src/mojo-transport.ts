import type { CallOptions, EventFilter, Subscription, Transport } from "@bdk/protocol";
import { BdkError, SdkErrors } from "./errors.js";

/** One broker answer: `resultJson` is the result when `ok`, else the error. */
interface InvokeReply {
  readonly ok: boolean;
  readonly resultJson: string;
}

interface SubscribeReply {
  readonly ok: boolean;
  readonly errorJson: string;
}

/** The generated `PageBrokerRemote` (chromium/bdk/engine/page_broker.mojom), as far as this package uses it. */
export interface PageBrokerRemote {
  setClient(client: unknown): void;
  invoke(callId: bigint, command: string, argumentsJson: string): Promise<InvokeReply>;
  cancel(callId: bigint): void;
  subscribe(subscriptionId: bigint, event: string, filterJson: string): Promise<SubscribeReply>;
  unsubscribe(subscriptionId: bigint): void;
}

/** The generated `PageClientCallbackRouter`: where the broker's progress and events arrive. */
export interface PageClientCallbackRouter {
  readonly onProgress: {
    addListener(listener: (callId: bigint, fraction: number, message: string) => void): unknown;
  };
  readonly onEvent: {
    addListener(listener: (subscriptionId: bigint, payloadJson: string) => void): unknown;
  };
  readonly $: { bindNewPipeAndPassRemote(): unknown };
}

/** The shape of `/@bdk/page_broker.mojom-webui.js`, which the runtime serves to every bundled page. */
export interface BrokerBindings {
  readonly PageBroker: { getRemote(): PageBrokerRemote };
  readonly PageClientCallbackRouter: new () => PageClientCallbackRouter;
}

export interface MojoTransportOptions {
  /** Loads the bindings. The default imports the runtime's module; tests pass a fake. */
  readonly load?: () => Promise<BrokerBindings>;
}

const BINDINGS_URL = "/@bdk/page_broker.mojom-webui.js";

function transportUnavailable(message: string): BdkError {
  return new BdkError({ ...SdkErrors.transportUnavailable, message });
}

function describe(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

async function loadBindings(): Promise<BrokerBindings> {
  let bindings: Partial<BrokerBindings>;
  try {
    bindings = (await import(BINDINGS_URL)) as Partial<BrokerBindings>;
  } catch (error) {
    throw transportUnavailable(`cannot load ${BINDINGS_URL}: ${describe(error)}`);
  }
  if (bindings.PageBroker === undefined || bindings.PageClientCallbackRouter === undefined) {
    throw transportUnavailable(
      `${BINDINGS_URL} does not export PageBroker and PageClientCallbackRouter`,
    );
  }
  return bindings as BrokerBindings;
}

/** The error a broker answer carries, `{"domain","code","message","details"}`. */
function errorFromJson(json: string): BdkError {
  try {
    const value: unknown = JSON.parse(json);
    if (typeof value === "object" && value !== null) {
      const { domain, code, message, details } = value as Record<string, unknown>;
      if (typeof domain === "string" && typeof code === "string") {
        return new BdkError({
          domain,
          code,
          message: typeof message === "string" ? message : "",
          details,
        });
      }
    }
  } catch {
    // Falls through to the error below.
  }
  return new BdkError({
    ...SdkErrors.invalidResponse,
    message: `the broker answered an error that is not {domain, code, message}: ${json.slice(0, 200)}`,
  });
}

function resultFromJson(json: string): unknown {
  try {
    return JSON.parse(json);
  } catch {
    throw new BdkError({
      ...SdkErrors.invalidResponse,
      message: `the broker answered a result that is not JSON: ${json.slice(0, 200)}`,
    });
  }
}

function asBdkError(error: unknown): BdkError {
  return error instanceof BdkError ? error : transportUnavailable(describe(error));
}

function cancelled(): BdkError {
  return new BdkError({ ...SdkErrors.cancelled, message: "the call was cancelled" });
}

/**
 * A `Transport` over the page broker's Mojo interface (architecture.md section 9). Calls and subscriptions
 * are numbered with `bigint` identifiers, progress and events come back through the client router, and an
 * aborted call sends `cancel`. The bindings load on the first call, so importing this costs nothing.
 */
export function createMojoTransport(options: MojoTransportOptions = {}): Transport {
  const load = options.load ?? loadBindings;
  const progress = new Map<bigint, NonNullable<CallOptions["onProgress"]>>();
  const listeners = new Map<bigint, (payload: unknown) => void>();
  let nextCallId = 1n;
  let nextSubscriptionId = 1n;
  let connection: Promise<PageBrokerRemote> | undefined;

  async function open(): Promise<PageBrokerRemote> {
    const bindings = await load();
    const remote = bindings.PageBroker.getRemote();
    const router = new bindings.PageClientCallbackRouter();
    router.onProgress.addListener((callId, fraction, message) => {
      progress.get(callId)?.({ fraction, message });
    });
    router.onEvent.addListener((subscriptionId, payloadJson) => {
      listeners.get(subscriptionId)?.(JSON.parse(payloadJson));
    });
    remote.setClient(router.$.bindNewPipeAndPassRemote());
    return remote;
  }

  const connect = (): Promise<PageBrokerRemote> => {
    connection ??= open().catch((error: unknown) => {
      connection = undefined;
      throw asBdkError(error);
    });
    return connection;
  };

  function stringify(value: unknown): string {
    try {
      return JSON.stringify(value);
    } catch (error) {
      throw new BdkError({ ...SdkErrors.invalidArguments, message: describe(error) });
    }
  }

  return {
    async invoke(command, args, callOptions) {
      const signal = callOptions?.signal;
      if (signal?.aborted) throw cancelled();
      const argumentsJson = stringify(args);
      const remote = await connect();
      if (signal?.aborted) throw cancelled();
      const callId = nextCallId++;
      if (callOptions?.onProgress !== undefined) progress.set(callId, callOptions.onProgress);
      const abort = () => remote.cancel(callId);
      signal?.addEventListener("abort", abort, { once: true });
      try {
        const reply = await remote.invoke(callId, command, argumentsJson);
        if (!reply.ok) throw errorFromJson(reply.resultJson);
        return resultFromJson(reply.resultJson);
      } finally {
        progress.delete(callId);
        signal?.removeEventListener("abort", abort);
      }
    },

    subscribe(
      event: string,
      filter: EventFilter | undefined,
      listener: (payload: unknown) => void,
      onError?: (error: BdkError) => void,
    ): Subscription {
      const subscriptionId = nextSubscriptionId++;
      let active = true;
      let remote: PageBrokerRemote | undefined;
      const end = (error: BdkError) => {
        if (!active) return;
        active = false;
        listeners.delete(subscriptionId);
        onError?.(error);
      };
      listeners.set(subscriptionId, listener);
      connect()
        .then(async (connected) => {
          if (!active) return;
          remote = connected;
          const reply = await connected.subscribe(
            subscriptionId,
            event,
            JSON.stringify(filter ?? {}),
          );
          if (!reply.ok) end(errorFromJson(reply.errorJson));
        })
        .catch((error: unknown) => end(asBdkError(error)));
      return {
        unsubscribe() {
          if (!active) return;
          active = false;
          listeners.delete(subscriptionId);
          remote?.unsubscribe(subscriptionId);
        },
      };
    },
  };
}
