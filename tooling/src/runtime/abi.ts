import { existsSync, readFileSync } from "node:fs";
import { join } from "node:path";

export interface AbiInfo {
  readonly version: number;
  /** Hex SHA-256 over the normalised public headers and interface definitions. */
  readonly hash: string;
}

/**
 * The one place that learns the BDK ABI version and hash. packages/core owns both:
 *
 * - version: `#define BDK_ABI_VERSION n` in `packages/core/include/bdk/bdk_base.h`;
 * - hash: `packages/core/generated/abi-hash.txt`, written by
 *   `cmake -DMODE=generate -P packages/core/cmake/abi-hash.cmake` (`MODE=check` fails when the file
 *   is stale), with `generated/bdk_abi_hash.h` as a second source.
 *
 * Adjust this function, and nothing else, if packages/core changes how it exposes them.
 */
export function readAbi(repoRoot: string): AbiInfo {
  const core = join(repoRoot, "packages", "core");
  const baseHeader = join(core, "include", "bdk", "bdk_base.h");
  if (!existsSync(baseHeader)) {
    throw new Error(`ABI version unavailable: ${baseHeader} does not exist.`);
  }
  const versionMatch = /^\s*#\s*define\s+BDK_ABI_VERSION\s+(\d+)\b/m.exec(
    readFileSync(baseHeader, "utf8"),
  );
  if (versionMatch?.[1] === undefined) {
    throw new Error(`ABI version unavailable: no "#define BDK_ABI_VERSION <n>" in ${baseHeader}.`);
  }

  let hash: string | undefined;
  const txt = join(core, "generated", "abi-hash.txt");
  const header = join(core, "generated", "bdk_abi_hash.h");
  if (existsSync(txt)) {
    hash = readFileSync(txt, "utf8").trim();
  } else if (existsSync(header)) {
    hash = /#define\s+BDK_ABI_HASH\s+"([0-9a-f]{64})"/.exec(readFileSync(header, "utf8"))?.[1];
  }
  if (hash === undefined || !/^[0-9a-f]{64}$/.test(hash)) {
    throw new Error(
      "ABI hash unavailable: packages/core/generated/abi-hash.txt is missing or malformed. " +
        "Run: cmake -DMODE=generate -P packages/core/cmake/abi-hash.cmake",
    );
  }
  return { version: Number(versionMatch[1]), hash };
}
