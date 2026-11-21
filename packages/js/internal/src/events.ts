import type { EventFilter, Subscription } from "@bdk/protocol";
import { subscribe } from "./call.js";
import { BdkError, SdkErrors } from "./errors.js";

/**
 * The application's events: name to payload. `bdk build codegen` fills it by declaration merging
 * (`@bdk/internal/app.d.ts`); with nothing declared, any `app.` name is accepted.
 */
// biome-ignore lint/suspicious/noEmptyInterface: filled by declaration merging
export interface AppEvents {}

export type AppEventName = keyof AppEvents extends never
  ? `app.${string}`
  : keyof AppEvents & string;

export type AppEventPayload<N extends AppEventName> = N extends keyof AppEvents
  ? AppEvents[N]
  : unknown;

export const events = {
  /** Listens to an application event. Needs the capability named like it, or a covering `app.<prefix>.*`. */
  on<N extends AppEventName>(
    name: N,
    listener: (payload: AppEventPayload<N>) => void,
    filter?: EventFilter,
  ): Subscription {
    if (!name.startsWith("app.")) {
      throw new BdkError({
        ...SdkErrors.invalidArguments,
        message: `${name} is not an application event; its name starts with "app."`,
      });
    }
    return subscribe(name, listener as (payload: unknown) => void, filter);
  },
};
