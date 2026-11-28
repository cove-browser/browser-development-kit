/**
 * What this BDK version needs of the machine (sdk/cli.md section 6, "Machine"). Each value has one
 * source, named beside it; change it there and here together.
 */
export const REQUIREMENTS = {
  /** `platforms: [.macOS(.v14)]` in the root `Package.swift`; a runtime's manifest can raise it. */
  macos: "14.0",
  /** The oldest Xcode the Swift package and the template project are built with: Swift 6 language mode. */
  xcode: "26.0",
  /** `swift-tools-version: 6.0` in the root `Package.swift`. */
  swift: "6.0",
  /** Decision D6: v1 ships for Apple Silicon only. */
  arch: "arm64",
} as const;

/** Compares dotted version strings numerically: `14.10` is newer than `14.9`. Missing parts are 0. */
export function compareVersions(a: string, b: string): number {
  const left = a.split(".").map((part) => Number.parseInt(part, 10) || 0);
  const right = b.split(".").map((part) => Number.parseInt(part, 10) || 0);
  for (let i = 0; i < Math.max(left.length, right.length); i++) {
    const difference = (left[i] ?? 0) - (right[i] ?? 0);
    if (difference !== 0) return difference;
  }
  return 0;
}
