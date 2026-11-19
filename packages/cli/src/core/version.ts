import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";

/** Replaced with a string literal when the bundle is built (`scripts/package.ts`). */
declare const __BDK_CLI_VERSION__: string | undefined;

/** The CLI version: embedded at bundle time, read from `package.json` when running unpackaged. */
export function cliVersion(): string {
  if (typeof __BDK_CLI_VERSION__ === "string") return __BDK_CLI_VERSION__;
  const file = fileURLToPath(new URL("../../package.json", import.meta.url));
  const parsed: unknown = JSON.parse(readFileSync(file, "utf8"));
  if (typeof parsed === "object" && parsed !== null && "version" in parsed) {
    const { version } = parsed;
    if (typeof version === "string") return version;
  }
  throw new Error(`${file} has no version.`);
}
