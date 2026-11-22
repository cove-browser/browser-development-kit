import type { Connection, EventListener, Subscription } from "./connection.js";
import { hostConnection } from "./host.js";

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
  on(name: string, listener: EventListener, options?: EventOptions): Subscription;
}

/** Builds `events` over a connection that is resolved on first use. */
export function createEvents(connection: () => Connection): Events {
  return {
    on(name, listener, options = {}) {
      return connection().subscribe(name, listener, options);
    },
  };
}

/** Events through the automation host. Scripts import it as `@bdk/events`. */
export const events: Events = createEvents(hostConnection);
