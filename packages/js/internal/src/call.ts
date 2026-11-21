import type { CallOptions, EventFilter, Paginated, Subscription, Transport } from "@bdk/protocol";
import { BdkError, SdkErrors } from "./errors.js";
import { createMojoTransport } from "./mojo-transport.js";

export type {
  CallOptions,
  EventFilter,
  Paginated,
  Progress,
  Subscription,
  Transport,
} from "@bdk/protocol";

let current: Transport | undefined;

/** Replaces the transport every call goes through; `undefined` restores the default. Tests install a mock. */
export function setTransport(transport: Transport | undefined): void {
  current = transport;
}

/** The transport in use. The default, the page broker's Mojo interface, is created on first use. */
export function getTransport(): Transport {
  current ??= createMojoTransport();
  return current;
}

function cancelled(): BdkError {
  return new BdkError({ ...SdkErrors.cancelled, message: "the call was cancelled" });
}

/**
 * Runs a command and resolves with its result. An aborted `signal` rejects with `broker.cancelled`, and the
 * transport is asked to stop the work.
 */
export function invoke<T = unknown>(
  name: string,
  args: Readonly<Record<string, unknown>> = {},
  options?: CallOptions,
): Promise<T> {
  const signal = options?.signal;
  if (signal?.aborted) return Promise.reject(cancelled());
  const call = getTransport().invoke(name, args, options) as Promise<T>;
  if (signal === undefined) return call;
  return new Promise<T>((resolve, reject) => {
    const onAbort = () => reject(cancelled());
    signal.addEventListener("abort", onAbort, { once: true });
    call.then(resolve, reject).finally(() => signal.removeEventListener("abort", onAbort));
  });
}

/**
 * The items of a paginated command, page after page: `for await (const item of paginate(...))`. `args.limit`
 * is the page size; the cursor is followed until the command stops returning one.
 */
export function paginate<T>(
  name: string,
  args: Readonly<Record<string, unknown>> = {},
  options?: CallOptions,
): AsyncIterable<T> {
  return pages<T>(name, args, options);
}

async function* pages<T>(
  name: string,
  args: Readonly<Record<string, unknown>>,
  options: CallOptions | undefined,
): AsyncGenerator<T> {
  let cursor: string | undefined;
  do {
    const page: Paginated<T> = await invoke<Paginated<T>>(
      name,
      cursor === undefined ? args : { ...args, cursor },
      options,
    );
    if (!Array.isArray(page?.items)) {
      throw new BdkError({
        ...SdkErrors.invalidResponse,
        message: `${name} did not answer a page of items`,
      });
    }
    yield* page.items;
    cursor = page.nextCursor ?? undefined;
  } while (cursor !== undefined && cursor !== "");
}

/** Subscribes to an event. `onError` hears a refusal or a lost connection, after which the subscription is over. */
export function subscribe(
  event: string,
  listener: (payload: unknown) => void,
  filter?: EventFilter,
  onError?: (error: BdkError) => void,
): Subscription {
  return getTransport().subscribe(event, filter, listener, onError);
}
