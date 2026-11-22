import type { CallOptions, EventFilter, Subscription, Transport } from "@bdk/protocol";
import { setTransport } from "./call.js";
import { holdsCapability } from "./capabilities.js";
import { BdkError, SdkErrors } from "./errors.js";
import { COMMAND_CAPABILITIES, EVENT_CAPABILITIES } from "./metadata.js";

/** A call the mock transport saw, in order. Refused calls are recorded too. */
export interface RecordedCall {
  readonly command: string;
  readonly args: Readonly<Record<string, unknown>>;
  readonly options: CallOptions | undefined;
}

/** A live subscription of the mock transport. */
export interface RecordedSubscription {
  readonly event: string;
  readonly filter: EventFilter | undefined;
}

export interface MockTransportOptions {
  /**
   * The capabilities the page holds. Without it every call is allowed. With it a call or subscription the
   * grants do not cover is refused with `capability.denied`, as the broker refuses it.
   */
  readonly grants?: readonly string[];
}

export interface MockTransport extends Transport {
  readonly calls: readonly RecordedCall[];
  readonly subscriptions: readonly RecordedSubscription[];
  /**
   * Answers a command with a value or with a function of the arguments and call options (it may be async
   * and may throw a `BdkError`). The answer passes through JSON, as it does on the wire.
   */
  respond(
    command: string,
    response: (args: Readonly<Record<string, unknown>>, options?: CallOptions) => unknown,
  ): this;
  respond(command: string, response: unknown): this;
  /** Delivers an event payload to the subscriptions of that event, in the order they subscribed. */
  emit(event: string, payload: unknown): void;
}

function wire(value: unknown): unknown {
  return value === undefined ? null : JSON.parse(JSON.stringify(value));
}

function denied(capability: string, what: string): BdkError {
  return new BdkError({
    ...SdkErrors.capabilityDenied,
    message: `${what} needs the capability ${capability}, which the page does not hold`,
    details: { capability },
  });
}

/** The capability a command needs: null for none, undefined for a name that is not a command. */
function commandCapability(command: string): string | null | undefined {
  return command.startsWith("app.") ? command : COMMAND_CAPABILITIES[command];
}

function eventCapability(event: string): string | undefined {
  return event.startsWith("app.") ? event : EVENT_CAPABILITIES[event];
}

/** A transport for a page's unit tests: scripted answers, recorded calls, optional grants. */
export function createMockTransport(options: MockTransportOptions = {}): MockTransport {
  const responses = new Map<string, unknown>();
  const calls: RecordedCall[] = [];
  const active = new Set<{
    readonly event: string;
    readonly filter: EventFilter | undefined;
    readonly listener: (payload: unknown) => void;
  }>();
  const allows = (capability: string | null) =>
    capability === null ||
    options.grants === undefined ||
    holdsCapability(options.grants, capability);

  const mock: MockTransport = {
    calls,
    get subscriptions() {
      return [...active].map(({ event, filter }) => ({ event, filter }));
    },

    respond(command, response) {
      responses.set(command, response);
      return this;
    },

    async invoke(command, args, callOptions) {
      calls.push({ command, args, options: callOptions });
      if (callOptions?.signal?.aborted) {
        throw new BdkError({ ...SdkErrors.cancelled, message: "the call was cancelled" });
      }
      const capability = commandCapability(command);
      if (capability === undefined) {
        throw new BdkError({ ...SdkErrors.unknownCommand, message: `${command} is not a command` });
      }
      if (!allows(capability)) throw denied(capability as string, command);
      if (!responses.has(command)) {
        throw new BdkError({
          ...SdkErrors.notImplemented,
          message: `the mock transport has no response for ${command}; register one with respond()`,
        });
      }
      const response = responses.get(command);
      const answer = typeof response === "function" ? await response(args, callOptions) : response;
      return wire(answer);
    },

    subscribe(event, filter, listener, onError): Subscription {
      const capability = eventCapability(event);
      const refusal =
        capability === undefined
          ? new BdkError({ ...SdkErrors.unknownCommand, message: `${event} is not an event` })
          : allows(capability)
            ? undefined
            : denied(capability, `subscribing to ${event}`);
      if (refusal !== undefined) {
        queueMicrotask(() => onError?.(refusal));
        return { unsubscribe() {} };
      }
      const entry = { event, filter, listener };
      active.add(entry);
      return { unsubscribe: () => void active.delete(entry) };
    },

    emit(event, payload) {
      for (const entry of [...active]) {
        if (entry.event === event) entry.listener(wire(payload));
      }
    },
  };
  return mock;
}

/** Makes `transport` the one every call of `@bdk/internal` goes through. Returns the function that undoes it. */
export function installMockTransport(transport: Transport): () => void {
  setTransport(transport);
  return () => setTransport(undefined);
}

/**
 * The capabilities of a route in the text of a `bdk.resolved.json`, an alias resolved to its target, to pass
 * as `grants`. Throws when the route is not in the file.
 */
export function grantsFromResolved(resolvedJson: string, route: string): readonly string[] {
  const resolved = JSON.parse(resolvedJson) as {
    routes?: Record<string, { capabilities?: unknown } | undefined>;
    aliases?: Record<string, string | undefined>;
  };
  const entry = resolved.routes?.[resolved.aliases?.[route] ?? route];
  if (entry === undefined) throw new Error(`bdk.resolved.json has no route or alias "${route}"`);
  const capabilities = entry.capabilities;
  if (!Array.isArray(capabilities) || capabilities.some((item) => typeof item !== "string")) {
    throw new Error(`bdk.resolved.json: the capabilities of "${route}" are not a list of names`);
  }
  return capabilities as string[];
}
