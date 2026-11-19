import { mkdtemp, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { bdkPaths } from "../paths.js";
import { archiveFileName, createArchive, defaultFormat } from "./archive.js";
import { canonicalJson } from "./canonical.js";
import type { Manifest } from "./manifest.js";
import { type PackageOptions, packageRuntime } from "./packager.js";
import { removeTree } from "./store.js";

export interface ReproducibilityReport {
  readonly reproducible: boolean;
  readonly sourceKey: string;
  readonly differences: readonly string[];
}

/** The manifest with the one field that may differ between two packagings blanked. */
function withoutTimestamp(m: Manifest): unknown {
  return { ...m, builder: { ...m.builder, timestamp: "" } };
}

/**
 * Packages the same build output twice into two throwaway stores. Reproducible means: identical file
 * digests, identical manifests apart from `builder.timestamp`, and archiving one store entry twice
 * gives the same bytes. (Two packagings cannot share an archive digest: the timestamp is in the
 * manifest, which is in the archive.)
 */
export async function checkReproducible(
  opts: Omit<PackageOptions, "paths">,
): Promise<ReproducibilityReport> {
  const scratch = await mkdtemp(join(tmpdir(), "bdk-repro-"));
  try {
    const manifests: Manifest[] = [];
    let firstDir = "";
    for (const [i, offsetMs] of [0, 1000].entries()) {
      const stamp = new Date(Date.parse(opts.builder.timestamp) + offsetMs).toISOString();
      const r = await packageRuntime({
        ...opts,
        paths: bdkPaths(join(scratch, `home${i}`)),
        builder: { ...opts.builder, timestamp: stamp },
      });
      manifests.push(r.manifest);
      if (i === 0) firstDir = r.runtimeDir;
    }
    const [a, b] = manifests as [Manifest, Manifest];
    const differences: string[] = [];
    for (const path of new Set([...Object.keys(a.files), ...Object.keys(b.files)])) {
      if (a.files[path] !== b.files[path]) differences.push(`file differs: ${path}`);
    }
    if (canonicalJson(withoutTimestamp(a)) !== canonicalJson(withoutTimestamp(b))) {
      differences.push("manifests differ beyond builder.timestamp");
    }
    const format = defaultFormat();
    const name = archiveFileName(a.source_key, format);
    const x = await createArchive(firstDir, join(scratch, "a1", name), format);
    const y = await createArchive(firstDir, join(scratch, "a2", name), format);
    if (x.sha256 !== y.sha256)
      differences.push("archiving the same runtime twice gave different bytes");
    return { reproducible: differences.length === 0, sourceKey: a.source_key, differences };
  } finally {
    await removeTree(scratch).catch(() => rm(scratch, { recursive: true, force: true }));
  }
}
