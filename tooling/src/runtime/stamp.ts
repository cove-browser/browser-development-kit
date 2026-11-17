import { existsSync, readFileSync } from "node:fs";
import { rm, writeFile } from "node:fs/promises";
import { join } from "node:path";
import { canonicalJson } from "./canonical.js";
import { type SourceIdentity, sourceKey } from "./identity.js";

/** Written into `out/<config>` by chromium:build; never part of a runtime (runtime-files.json lists paths). */
export const BUILD_STAMP_NAME = "bdk-build-identity.json";

export interface BuildStamp {
  readonly source_key: string;
  readonly identity: SourceIdentity;
}

export function buildStamp(identity: SourceIdentity): BuildStamp {
  return { source_key: sourceKey(identity), identity };
}

/** Removes the stamp: from here on `out/<config>` is not known to match any identity. */
export async function clearBuildStamp(outDir: string): Promise<void> {
  await rm(join(outDir, BUILD_STAMP_NAME), { force: true });
}

export async function writeBuildStamp(outDir: string, identity: SourceIdentity): Promise<void> {
  await writeFile(
    join(outDir, BUILD_STAMP_NAME),
    `${JSON.stringify(JSON.parse(canonicalJson(buildStamp(identity))), null, 2)}\n`,
  );
}

function flatten(value: unknown, prefix: string, out: Map<string, string>): void {
  if (value !== null && typeof value === "object") {
    for (const [k, v] of Object.entries(value))
      flatten(v, prefix === "" ? k : `${prefix}.${k}`, out);
  } else {
    out.set(prefix, JSON.stringify(value));
  }
}

/** Dotted names of the identity fields that differ between two identities. */
export function identityDifferences(a: SourceIdentity, b: SourceIdentity): string[] {
  const fa = new Map<string, string>();
  const fb = new Map<string, string>();
  flatten(a, "", fa);
  flatten(b, "", fb);
  return [...new Set([...fa.keys(), ...fb.keys()])].filter((k) => fa.get(k) !== fb.get(k)).sort();
}

/** Throws unless `outDir` was built from exactly `identity` (recorded by chromium:build). */
export function assertBuildStamp(outDir: string, identity: SourceIdentity): void {
  const file = join(outDir, BUILD_STAMP_NAME);
  const remedy = "Run pnpm chromium:build (for this config) and package again.";
  if (!existsSync(file)) {
    throw new Error(
      `${outDir} has no ${BUILD_STAMP_NAME}: it was not built by a completed chromium:build, so it cannot be tied to the current source identity. ${remedy}`,
    );
  }
  let stamp: BuildStamp;
  try {
    stamp = JSON.parse(readFileSync(file, "utf8")) as BuildStamp;
    if (typeof stamp.identity !== "object" || stamp.identity === null)
      throw new Error("no identity");
  } catch (error) {
    throw new Error(`${file} is unreadable (${(error as Error).message}). ${remedy}`);
  }
  const differs = identityDifferences(stamp.identity, identity);
  if (differs.length > 0) {
    throw new Error(
      `${outDir} was built from different inputs (overlay/patches/gn args/...) than the current repository: ` +
        `${differs.join(", ")} differ (built for source key ${stamp.source_key}, now ${sourceKey(identity)}). ${remedy}`,
    );
  }
}
