import { decodeBase64 } from "./base64.js";
import type { Transport } from "./transport.js";
import type { Frame, WireError } from "./wire.js";

/** A command or protocol error, with the BDK error domain and code. */
export class BDKError extends Error {
  readonly domain: string;
  readonly code: string;

  constructor(error: WireError) {
    super(error.message);
    this.name = "BDKError";
    this.domain = error.domain;
    this.code = error.code;
  }
}

/** Options of one call. */
export interface CallOptions {
  profile?: string;
  contents?: string;
  /** Aborting sends `cancel` and rejects with `broker.cancelled`. */
  signal?: AbortSignal;
  onProgress?: (fraction: number, message: string) => void;
}

/** Options of one subscription. */
export interface SubscribeOptions {
  profile?: string;
  contents?: string;
}

/** The outcome of a call: the JSON value and, for binary commands, the decoded bytes. */
export interface CallResult {
  value: unknown;
  binary?: Uint8Array;
}

/** A live subscription. */
export interface Subscription {
  /** Settles when BDK has accepted (or refused) the subscription. */
  readonly ready: Promise<void>;
  unsubscribe(): void;
}

export type EventListener = (payload: unknown, name: string) => void;

interface PendingCall {
  kind: "call";
  resolve(result: CallResult): void;
  reject(error: Error): void;
  onProgress: CallOptions["onProgress"];
}

interface PendingSubscription {
  kind: "subscription";
  listener: EventListener;
  resolve(): void;
  reject(error: Error): void;
}

const CANCELLED: WireError = {
  domain: "broker",
  code: "cancelled",
  message: "the call was cancelled",
};
const CLOSED: WireError = { domain: "broker", code: "closed", message: "the connection closed" };

/** Calls and subscriptions over a transport. */
export class Connection {
  readonly #transport: Transport;
  readonly #entries = new Map<number, PendingCall | PendingSubscription>();
  #nextId = 1;
  #ended: BDKError | undefined;

  constructor(transport: Transport) {
    this.#transport = transport;
    transport.onFrame((frame) => this.#handle(frame));
    transport.onClose(() => this.#end(new BDKError(CLOSED)));
  }

  /** Invokes a command as the session. Rejects with a BDKError when it fails. */
  call(
    command: string,
    args: Record<string, unknown> = {},
    options: CallOptions = {},
  ): Promise<CallResult> {
    const { signal } = options;
    if (this.#ended) return Promise.reject(this.#ended);
    if (signal?.aborted) return Promise.reject(new BDKError(CANCELLED));
    const id = this.#nextId++;
    return new Promise<CallResult>((resolve, reject) => {
      const onAbort = () => {
        if (!this.#entries.delete(id)) return;
        this.#trySend({ type: "cancel", id });
        reject(new BDKError(CANCELLED));
      };
      signal?.addEventListener("abort", onAbort, { once: true });
      const settle = <T>(fn: (value: T) => void) => {
        return (value: T) => {
          signal?.removeEventListener("abort", onAbort);
          fn(value);
        };
      };
      this.#entries.set(id, {
        kind: "call",
        resolve: settle(resolve),
        reject: settle(reject),
        onProgress: options.onProgress,
      });
      try {
        this.#transport.send({
          type: "call",
          id,
          command,
          args,
          ...(options.profile !== undefined && { profile: options.profile }),
          ...(options.contents !== undefined && { contents: options.contents }),
        });
      } catch (error) {
        this.#entries.delete(id);
        settle(reject)(asError(error));
      }
    });
  }

  /** Subscribes to an event. The listener gets the payload and the event name. */
  subscribe(event: string, listener: EventListener, options: SubscribeOptions = {}): Subscription {
    const id = this.#nextId++;
    let resolve!: () => void;
    let reject!: (error: Error) => void;
    const ready = new Promise<void>((res, rej) => {
      resolve = res;
      reject = rej;
    });
    // A caller that never awaits `ready` must not trigger an unhandled rejection.
    ready.catch(() => undefined);
    if (this.#ended) {
      reject(this.#ended);
    } else {
      this.#entries.set(id, { kind: "subscription", listener, resolve, reject });
      try {
        this.#transport.send({
          type: "subscribe",
          id,
          event,
          ...(options.profile !== undefined && { profile: options.profile }),
          ...(options.contents !== undefined && { contents: options.contents }),
        });
      } catch (error) {
        this.#entries.delete(id);
        reject(asError(error));
      }
    }
    return {
      ready,
      unsubscribe: () => {
        if (this.#entries.delete(id)) this.#trySend({ type: "unsubscribe", id });
      },
    };
  }

  /** Ends the connection: closes the transport and rejects everything pending. */
  close(): void {
    this.#end(new BDKError(CLOSED));
    this.#transport.close();
  }

  #trySend(frame: Frame): void {
    try {
      this.#transport.send(frame);
    } catch {
      // The transport is gone; its close handler rejects whatever is left.
    }
  }

  #handle(frame: Frame): void {
    switch (frame.type) {
      case "result": {
        const entry = this.#entries.get(frame.id);
        if (entry?.kind === "call") {
          this.#entries.delete(frame.id);
          try {
            entry.resolve({
              value: frame.value,
              ...(frame.binary !== undefined && { binary: decodeBase64(frame.binary) }),
            });
          } catch (error) {
            entry.reject(asError(error));
          }
        } else if (entry?.kind === "subscription") {
          entry.resolve();
        }
        return;
      }
      case "error": {
        const entry = this.#entries.get(frame.id);
        if (entry) {
          this.#entries.delete(frame.id);
          entry.reject(new BDKError(frame.error));
        }
        return;
      }
      case "progress": {
        if (!("id" in frame)) return;
        const entry = this.#entries.get(frame.id);
        if (entry?.kind === "call") entry.onProgress?.(frame.fraction, frame.message);
        return;
      }
      case "event": {
        const entry = this.#entries.get(frame.id);
        if (entry?.kind === "subscription") {
          try {
            entry.listener(frame.payload, frame.name);
          } catch (error) {
            // Surface the listener's failure without breaking frame delivery.
            void Promise.reject(error);
          }
        }
        return;
      }
      case "closed":
        this.#end(new BDKError(frame.error));
        this.#transport.close();
        return;
      default:
        return;
    }
  }

  #end(error: BDKError): void {
    if (this.#ended) return;
    this.#ended = error;
    const entries = [...this.#entries.values()];
    this.#entries.clear();
    for (const entry of entries) entry.reject(error);
  }
}

function asError(error: unknown): Error {
  return error instanceof Error ? error : new Error(String(error));
}
