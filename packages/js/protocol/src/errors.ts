/** The wire form of an error: what the broker answers and what Swift sees as `BDKError`. */
export interface BdkErrorInfo {
  readonly domain: string;
  readonly code: string;
  readonly message: string;
  readonly details?: unknown;
}

/** An error from a BDK call. `domain` and `code` are the ones Swift sees, for example `capability.denied`. */
export class BdkError extends Error {
  readonly domain: string;
  readonly code: string;
  declare readonly details?: unknown;

  constructor(info: BdkErrorInfo) {
    super(info.message);
    this.name = "BdkError";
    this.domain = info.domain;
    this.code = info.code;
    if (info.details !== undefined) this.details = info.details;
  }
}

/**
 * True for a `BdkError` from any copy of this class. `@bdk/internal` ships its own copy because a page
 * loads it without a bundler, so the check is on the shape, not on `instanceof`.
 */
export function isBdkError(value: unknown): value is BdkError {
  return (
    value instanceof Error &&
    value.name === "BdkError" &&
    typeof (value as Partial<BdkError>).domain === "string" &&
    typeof (value as Partial<BdkError>).code === "string"
  );
}

/** An error identity: the pair a caller matches on. */
export interface ErrorId {
  readonly domain: string;
  readonly code: string;
}

/** Errors the SDK raises itself, as opposed to the ones the broker answers. */
export const SdkErrors = {
  /** The call was aborted through its `AbortSignal`. The broker answers the same for `cancel`. */
  cancelled: { domain: "broker", code: "cancelled" },
  /** The page broker bindings could not be loaded, so no call can be made. */
  transportUnavailable: { domain: "broker", code: "transportUnavailable" },
  /** The transport answered something that is not the JSON the protocol defines. */
  invalidResponse: { domain: "broker", code: "invalidResponse" },
  /** A call or subscription refused for a missing capability. Also raised by the broker. */
  capabilityDenied: { domain: "capability", code: "denied" },
  /** A command name the transport does not know. Also raised by the broker. */
  unknownCommand: { domain: "broker", code: "unknownCommand" },
  /** A command the transport has no implementation for. Also raised by the broker. */
  notImplemented: { domain: "broker", code: "notImplemented" },
  /** Arguments the SDK refuses before sending. Also raised by the broker. */
  invalidArguments: { domain: "broker", code: "invalidArguments" },
} as const satisfies Readonly<Record<string, ErrorId>>;
