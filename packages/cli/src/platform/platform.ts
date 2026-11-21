import type { BundleFacts } from "../core/config/index.js";
import type { SignOutcome, SignRequest } from "../core/signing/types.js";

/** Where a host build keeps generated files, so the core can default what the host would otherwise be told. */
export interface BuildLocations {
  /** The application's declarations, `bdk.commands.json`, which the build writes before `bdk build codegen`. */
  commandsFile: string;
  /** Where `bdk build codegen` writes the `@bdk` packages. */
  jsOut: string;
  /** Where the input and output file lists for the host's build phases go. */
  fileListDir: string;
}

/** One build phase's input and output paths, as the host lists them for its own dependency checks. */
export interface FileList {
  /** `runtime` for the phase that runs `config runtime codegen`; `bundle` for the one that runs `pages embed`. */
  phase: "runtime" | "bundle";
  inputs: readonly string[];
  outputs: readonly string[];
}

/** What the core needs from the host platform (sdk/cli.md section 2.2). One implementation per platform. */
export interface Platform {
  /** A short name for messages: `xcode`, `generic`. */
  readonly name: string;
  /** True when the process runs inside the host's IDE build, so diagnostics should use its inline form. */
  isIdeBuild(env: NodeJS.ProcessEnv): boolean;
  /** The facts the platform keeps outside `bdk.toml` (`Info.plist`, entitlements), when it names them. */
  bundleFacts(env: NodeJS.ProcessEnv, projectDir: string): Promise<BundleFacts | undefined>;
  /** The host build's default locations for generated files, or undefined outside a host build. */
  buildLocations(env: NodeJS.ProcessEnv): BuildLocations | undefined;
  /**
   * Writes the build phases' input and output lists in the form the host reads, and returns the files written.
   * Writes nothing, and returns none, outside a host build.
   */
  writeFileLists(env: NodeJS.ProcessEnv, lists: readonly FileList[]): Promise<string[]>;
  /**
   * Re-signs the application bundle and the runtime inside it. Throws a `CliError` when the host cannot
   * sign (exit 3) or the signature does not verify (exit 4).
   */
  sign(request: SignRequest): Promise<SignOutcome>;
}
