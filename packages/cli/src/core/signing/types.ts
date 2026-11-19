import type { Diagnostic } from "../diagnostics.js";

/** What the signing step asks of the platform module. */
export interface SignRequest {
  /** The built application bundle. */
  appPath: string;
  /** A signing identity, or `-` for ad hoc. */
  identity: string;
  /** The Apple team the signature must carry; checked after signing. Meaningless for ad hoc. */
  team?: string;
  /** The application's entitlements file. */
  entitlementsFile?: string;
}

export interface SignOutcome {
  /** Warnings and errors. A refused entitlement is an error and nothing was changed. */
  diagnostics: Diagnostic[];
  /** Paths signed, in order. */
  signed: string[];
}
