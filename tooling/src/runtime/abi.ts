import { execFile } from "node:child_process";
import { existsSync, readdirSync, readFileSync } from "node:fs";
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

/**
 * Fails when the committed `generated/abi-hash.txt` no longer matches the headers and interface
 * definitions, by running `cmake -DMODE=check -P packages/core/cmake/abi-hash.cmake` from the repo root.
 * The source key embeds that file, so a stale one would name a runtime by the wrong ABI.
 */
export async function assertAbiHashCurrent(repoRoot: string): Promise<void> {
  const script = join("packages", "core", "cmake", "abi-hash.cmake");
  await new Promise<void>((resolve, reject) => {
    execFile(
      "cmake",
      ["-DMODE=check", "-P", script],
      { cwd: repoRoot, encoding: "utf8" },
      (error, stdout, stderr) => {
        if (error === null) return resolve();
        const detail = `${stderr}${stdout}`.trim() || error.message;
        reject(
          new Error(
            `Refusing to package: the generated ABI hash is out of date or could not be checked ` +
              `(cmake -DMODE=check -P packages/core/cmake/abi-hash.cmake):\n${detail}`,
          ),
        );
      },
    );
  });
}

/**
 * The symbols the runtime framework must export: every `BDK_EXPORT` function declared in
 * `packages/core/include/bdk/*.h`, and the Objective-C classes declared in
 * `packages/core/include/bdk_mac/*.h`, in Mach-O spelling (leading underscore), sorted. The
 * framework hides everything its export list does not name, so a function missing from the
 * `chromium/patches` export list links in the engine but is invisible to applications.
 */
export function abiExports(repoRoot: string): string[] {
  const include = join(repoRoot, "packages", "core", "include");
  const names = new Set<string>();
  const headers = (dir: string) =>
    existsSync(dir)
      ? readdirSync(dir)
          .filter((f) => f.endsWith(".h"))
          .map((f) => readFileSync(join(dir, f), "utf8"))
      : [];
  for (const text of headers(join(include, "bdk"))) {
    for (const name of exportedFunctions(text)) names.add(name);
  }
  for (const text of headers(join(include, "bdk_mac"))) {
    for (const m of text.matchAll(/^@interface\s+([A-Za-z_][A-Za-z0-9_]*)\s*:/gm)) {
      if (m[1] !== undefined) {
        names.add(`_OBJC_CLASS_$_${m[1]}`);
        names.add(`_OBJC_METACLASS_$_${m[1]}`);
      }
    }
  }
  return [...names].sort();
}

/**
 * The `BDK_EXPORT` functions one header declares, in Mach-O spelling (leading underscore), ignoring
 * those inside block comments.
 */
export function exportedFunctions(header: string): string[] {
  const code = header.replace(/\/\*[\s\S]*?\*\//g, "");
  const names: string[] = [];
  for (const m of code.matchAll(/\bBDK_EXPORT\b[^;{]*?\b(bdk_[a-z0-9_]+)\s*\(/g)) {
    if (m[1] !== undefined) names.push(`_${m[1]}`);
  }
  return names;
}
