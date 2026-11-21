/** Progress of a running call, as a fraction from 0 to 1 and a message. */
export interface Progress {
  readonly fraction: number;
  readonly message: string;
}

/** Options every call takes. */
export interface CallOptions {
  /** Aborting it cancels the call, which then rejects with `broker.cancelled`. */
  signal?: AbortSignal;
  onProgress?: (progress: Progress) => void;
}

/** A live event subscription. It also ends with the page or session that made it. */
export interface Subscription {
  unsubscribe(): void;
}

/** The filter of an event subscription: field name to the value the event must have. */
export type EventFilter = Readonly<Record<string, unknown>>;

/** The result of a paginated command: one page, and the cursor for the next when there is one. */
export interface Paginated<T> {
  readonly items: readonly T[];
  readonly nextCursor?: string;
}

/** The item type of a paginated result. */
export type ItemOf<P> = P extends Paginated<infer T> ? T : never;
