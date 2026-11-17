import { readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";

export type ArchiveFormat = "tar.zst" | "tar.gz";

/** The `runtime` record in `chromium/pin.json`: what `chromium:publish` wrote for one source key. */
export interface RuntimeRecord {
  /** Source key (store directory name) this archive was packaged for. */
  readonly source_key: string;
  /** `https://...` or `file:///...` location of the archive. */
  readonly url: string;
  readonly archive_format: ArchiveFormat;
  /** SHA-256 (hex) of the archive file. Checked before anything is unpacked. */
  readonly archive_sha256: string;
  /** SHA-256 (hex) of the canonical manifest. Checked after unpacking. */
  readonly manifest_sha256: string;
  /** ABI hash the runtime was built against. */
  readonly abi_hash: string;
  /** Archive size in bytes. */
  readonly size: number;
}

export interface Pin {
  readonly tag: string;
  readonly commit: string;
  readonly bdk_abi_version: number;
  readonly runtime: RuntimeRecord | null;
}

const HEX64 = /^[0-9a-f]{64}$/;

export class PinError extends Error {
  override readonly name = "PinError";
}

export function pinPath(repoRoot: string): string {
  return join(repoRoot, "chromium", "pin.json");
}

export function parsePin(text: string): Pin {
  const raw: unknown = JSON.parse(text);
  if (raw === null || typeof raw !== "object")
    throw new PinError("chromium/pin.json: not an object.");
  const o = raw as Record<string, unknown>;
  const str = (k: string): string => {
    const v = o[k];
    if (typeof v !== "string" || v === "")
      throw new PinError(`chromium/pin.json: "${k}" must be a string.`);
    return v;
  };
  const abi = o.bdk_abi_version;
  if (typeof abi !== "number" || !Number.isInteger(abi)) {
    throw new PinError('chromium/pin.json: "bdk_abi_version" must be an integer.');
  }
  return {
    tag: str("tag"),
    commit: str("commit"),
    bdk_abi_version: abi,
    runtime: parseRecord(o.runtime),
  };
}

function parseRecord(raw: unknown): RuntimeRecord | null {
  if (raw === null || raw === undefined) return null;
  if (typeof raw !== "object")
    throw new PinError('chromium/pin.json: "runtime" must be an object or null.');
  const r = raw as Record<string, unknown>;
  const need = (k: string): string => {
    const v = r[k];
    if (typeof v !== "string" || v === "")
      throw new PinError(`chromium/pin.json: runtime.${k} must be a string.`);
    return v;
  };
  const hex = (k: string): string => {
    const v = need(k);
    if (!HEX64.test(v))
      throw new PinError(`chromium/pin.json: runtime.${k} must be 64 lowercase hex digits.`);
    return v;
  };
  const format = need("archive_format");
  if (format !== "tar.zst" && format !== "tar.gz") {
    throw new PinError(
      `chromium/pin.json: runtime.archive_format "${format}" is not tar.zst or tar.gz.`,
    );
  }
  const size = r.size;
  if (typeof size !== "number" || !Number.isInteger(size) || size < 0) {
    throw new PinError("chromium/pin.json: runtime.size must be a non-negative integer.");
  }
  const url = need("url");
  if (!/^(https:\/\/|file:\/\/\/)/.test(url)) {
    throw new PinError(
      `chromium/pin.json: runtime.url must be https:// or file:///, got "${url}".`,
    );
  }
  return {
    source_key: need("source_key"),
    url,
    archive_format: format,
    archive_sha256: hex("archive_sha256"),
    manifest_sha256: hex("manifest_sha256"),
    abi_hash: hex("abi_hash"),
    size,
  };
}

export function readPin(repoRoot: string): Pin {
  return parsePin(readFileSync(pinPath(repoRoot), "utf8"));
}

/** Writes pin.json in the shape Biome's formatter keeps (two-space indent, trailing newline). */
export function writePin(repoRoot: string, pin: Pin): void {
  const ordered = {
    tag: pin.tag,
    commit: pin.commit,
    bdk_abi_version: pin.bdk_abi_version,
    runtime:
      pin.runtime === null
        ? null
        : {
            source_key: pin.runtime.source_key,
            url: pin.runtime.url,
            archive_format: pin.runtime.archive_format,
            archive_sha256: pin.runtime.archive_sha256,
            manifest_sha256: pin.runtime.manifest_sha256,
            abi_hash: pin.runtime.abi_hash,
            size: pin.runtime.size,
          },
  };
  writeFileSync(pinPath(repoRoot), `${JSON.stringify(ordered, null, 2)}\n`);
}
