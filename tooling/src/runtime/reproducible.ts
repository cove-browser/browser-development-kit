import { mkdtemp, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { bdkPaths } from "../paths.js";
import { archiveFileName, createArchive, defaultFormat } from "./archive.js";
import { canonicalJson } from "./canonical.js";
import { compareManifests } from "./compare.js";
import type { Manifest } from "./manifest.js";
import { type PackageOptions, packageRuntime } from "./packager.js";
import { removeTree } from "./store.js";

export interface ReproducibilityReport {
  readonly reproducible: boolean;
  readonly sourceKey: string;
  readonly differences: readonly string[];
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
    const comparison = compareManifests(a, b, "strict");
    const differences = [...comparison.differences];
    // Both packagings come from one machine and one commit: the only provenance that may differ is the time.
    const provenance = (m: Manifest) =>
      canonicalJson([{ ...m.builder, timestamp: "" }, m.bdk.repo_commit, m.attestation]);
    if (provenance(a) !== provenance(b))
      differences.push("manifests differ beyond builder.timestamp");
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
