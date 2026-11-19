import type { BundleFacts } from "../core/config/index.js";
import type { SignOutcome, SignRequest } from "../core/signing/types.js";

/** What the core needs from the host platform (sdk/cli.md section 2.2). One implementation per platform. */
export interface Platform {
  /** A short name for messages: `xcode`, `generic`. */
  readonly name: string;
  /** True when the process runs inside the host's IDE build, so diagnostics should use its inline form. */
  isIdeBuild(env: NodeJS.ProcessEnv): boolean;
  /** The facts the platform keeps outside `bdk.toml` (`Info.plist`, entitlements), when it names them. */
  bundleFacts(env: NodeJS.ProcessEnv, projectDir: string): Promise<BundleFacts | undefined>;
  /**
   * Re-signs the application bundle and the runtime inside it. Throws a `CliError` when the host cannot
   * sign (exit 3) or the signature does not verify (exit 4).
   */
  sign(request: SignRequest): Promise<SignOutcome>;
}
