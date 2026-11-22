import type { Connection, Subscription } from "./connection.js";
import { hostConnection } from "./host.js";
import type { EventMap } from "./services.js";

/**
 * The application's events: name to payload. `bdk build codegen` fills it by declaration merging
 * (`@bdk/automation/app.d.ts`); with nothing declared, any `app.` name is accepted.
 */
// biome-ignore lint/suspicious/noEmptyInterface: filled by declaration merging
export interface AppEvents {}

export type AppEventName = keyof AppEvents extends never
  ? `app.${string}`
  : keyof AppEvents & string;

/** Every event `events.on` takes: the browser's and the application's. */
export type EventName = keyof EventMap | AppEventName;

export type PayloadOf<N extends EventName> = N extends keyof EventMap
  ? EventMap[N]
  : N extends keyof AppEvents
    ? AppEvents[N]
    : unknown;

/** Where an event subscription looks. */
export interface EventOptions {
  contents?: string;
  profile?: string;
}

/** Subscribes to browser events and the application's `app.` events. */
export interface Events {
  /**
   * Starts listening. Returns at once; `ready` settles when BDK accepted the subscription and
   * `unsubscribe()` stops it. The listener gets the payload and the event name.
   */
  on<N extends EventName>(
    name: N,
    listener: (payload: PayloadOf<N>, name: N) => void,
    options?: EventOptions,
  ): Subscription;
}

/** Builds `events` over a connection that is resolved on first use. */
export function createEvents(connection: () => Connection): Events {
  return {
    on(name, listener, options = {}) {
      return connection().subscribe(
        name,
        listener as (payload: unknown, name: string) => void,
        options,
      );
    },
  };
}

/** Events through the automation host. Scripts import it as `@bdk/events`. */
export const events: Events = createEvents(hostConnection);
