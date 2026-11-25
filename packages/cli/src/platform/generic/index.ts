import { CliError, ExitCode } from "../../core/exit-codes.js";
import type { Platform } from "../platform.js";

/** Used on hosts that have no platform module yet. It has no IDE build, no bundle facts and cannot sign. */
export const genericPlatform: Platform = {
  name: "generic",
  isIdeBuild: () => false,
  buildLocations: () => undefined,
  writeFileLists: () => Promise.resolve([]),
  bundleFacts: () => Promise.resolve(undefined),
  toolchain: () =>
    Promise.resolve({
      os: process.platform === "win32" ? "windows" : process.platform,
      arch: process.arch,
    }),
  inspectSignature: () =>
    Promise.reject(
      new CliError(
        {
          code: "doctor.unsupported_host",
          severity: "error",
          message: "Checking an application's signature is only supported on macOS.",
          fix: "Run `bdk doctor --bundle` on a Mac.",
        },
        ExitCode.Environment,
      ),
    ),
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
