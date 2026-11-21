/**
 * The error type of every call. It repeats `BdkError` of `@bdk/protocol` shape for shape, because a page loads
 * this package without a bundler and so cannot import that one at run time. `isBdkError` of `@bdk/protocol`
 * accepts it.
 */
export class BdkError extends Error {
  readonly domain: string;
  readonly code: string;
  declare readonly details?: unknown;

  constructor(info: {
    readonly domain: string;
    readonly code: string;
    readonly message: string;
    readonly details?: unknown;
  }) {
    super(info.message);
    this.name = "BdkError";
    this.domain = info.domain;
    this.code = info.code;
    if (info.details !== undefined) this.details = info.details;
  }
}

/** The errors this package raises itself; the same values as `SdkErrors` of `@bdk/protocol`. */
export const SdkErrors = {
  cancelled: { domain: "broker", code: "cancelled" },
  transportUnavailable: { domain: "broker", code: "transportUnavailable" },
  invalidResponse: { domain: "broker", code: "invalidResponse" },
  capabilityDenied: { domain: "capability", code: "denied" },
  unknownCommand: { domain: "broker", code: "unknownCommand" },
  notImplemented: { domain: "broker", code: "notImplemented" },
  invalidArguments: { domain: "broker", code: "invalidArguments" },
} as const;
