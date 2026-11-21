import type { CallOptions, EventFilter, Subscription } from "./call.js";
import type { BdkError } from "./errors.js";

/**
 * How a caller reaches the broker: a page through its Mojo interface, a session through the automation
 * host's channel, a test through a mock. Payloads are plain JSON values.
 */
export interface Transport {
  /** Runs a command. Resolves with its result (`null` for a void command) or rejects with a `BdkError`. */
  invoke(
    command: string,
    args: Readonly<Record<string, unknown>>,
    options?: CallOptions,
  ): Promise<unknown>;

  /** Subscribes to an event. `onError` hears a refusal or a lost connection; the subscription then ends. */
  subscribe(
    event: string,
    filter: EventFilter | undefined,
    listener: (payload: unknown) => void,
    onError?: (error: BdkError) => void,
  ): Subscription;
}
