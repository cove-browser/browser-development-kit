import { CliError, ExitCode } from "../../core/exit-codes.js";
import type { Platform } from "../platform.js";

/** Used on hosts that have no platform module yet. It has no IDE build, no bundle facts and cannot sign. */
export const genericPlatform: Platform = {
  name: "generic",
  isIdeBuild: () => false,
  buildLocations: () => undefined,
  writeFileLists: () => Promise.resolve([]),
  bundleFacts: () => Promise.resolve(undefined),
  sign: () =>
    Promise.reject(
      new CliError(
        {
          code: "sign.unsupported_host",
          severity: "error",
          message: "Signing an application is only supported on macOS.",
          fix: "Run `bdk build sign` on a Mac, or set BDK_SKIP_SIGNING=1.",
        },
        ExitCode.Environment,
      ),
    ),
};
