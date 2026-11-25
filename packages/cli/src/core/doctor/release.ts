import { AssetNotFoundError } from "../../assets.js";
import type { Context } from "../context.js";
import { CliError, ExitCode } from "../exit-codes.js";

/** The embedded asset that describes this BDK release (written by `cli:package`). */
export const RELEASE_ASSET = "bdk-release.json";

/** The Chromium pin, the ABI the SDK was compiled against, and the published runtime, if any. */
export interface ReleaseFacts {
  chromium: { tag: string; commit: string };
  abi: { version: number; hash: string };
  runtime: { source_key: string; manifest_sha256: string; abi_hash: string } | null;
}

/** Reads the release facts; a `bdk` without them is an environment failure (exit 3). */
export function readRelease(ctx: Context): ReleaseFacts {
  try {
    return JSON.parse(ctx.assets.readText(RELEASE_ASSET)) as ReleaseFacts;
  } catch (error) {
    if (!(error instanceof AssetNotFoundError)) throw error;
    throw new CliError(
      {
        code: "cli.asset_missing",
        severity: "error",
        message: `This bdk has no embedded ${RELEASE_ASSET}.`,
        fix: "Reinstall bdk from a release, or rebuild it with `pnpm cli:package`.",
      },
      ExitCode.Environment,
    );
  }
}
