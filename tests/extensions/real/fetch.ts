import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { mkdirSync, readFileSync, renameSync, rmSync, writeFileSync } from "node:fs";
import { join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { resolveBdkHome } from "../../../tooling/src/paths.js";
import { isMain } from "../../../tooling/src/runtime/cli.js";

export interface RealExtension {
  readonly id: string;
  readonly class: string;
  readonly name: string;
  readonly upstream: string;
  readonly licence: string;
  readonly version: string | null;
  readonly assetName: string | null;
  readonly assetUrl: string | null;
  readonly sha256: string | null;
  readonly note: string;
}

const LIST = resolve(fileURLToPath(import.meta.url), "..", "extensions.json");

export function loadExtensions(path: string = LIST): RealExtension[] {
  return (JSON.parse(readFileSync(path, "utf8")) as { extensions: RealExtension[] }).extensions;
}

/** `<BDK_HOME>/extensions-cache/<id>/<version>`: holds the zip and its `unpacked/` directory. */
export function cacheDir(home: string, entry: { id: string; version: string }): string {
  return join(home, "extensions-cache", entry.id, entry.version);
}

export function sha256Hex(bytes: Uint8Array): string {
  return createHash("sha256").update(bytes).digest("hex");
}

/** Throws when a recorded digest does not match the bytes. A null record accepts anything and says so. */
export function verifyDigest(
  entry: Pick<RealExtension, "id" | "sha256">,
  actual: string,
): "verified" | "unrecorded" {
  if (entry.sha256 === null) return "unrecorded";
  if (entry.sha256.toLowerCase() !== actual) {
    throw new Error(
      `${entry.id}: sha256 mismatch. Recorded ${entry.sha256}, downloaded ${actual}. Nothing was unpacked.`,
    );
  }
  return "verified";
}

/** The entry's version and asset URL, or an error saying what is still to be resolved. */
export function resolved(
  entry: RealExtension,
): RealExtension & { version: string; assetUrl: string; assetName: string } {
  const { version, assetUrl, assetName } = entry;
  if (version === null || assetUrl === null || assetName === null) {
    throw new Error(
      `${entry.id}: version, assetName or assetUrl is null in real/extensions.json. Resolve it from ${entry.upstream}/releases first.`,
    );
  }
  if (!assetUrl.startsWith("https://github.com/")) {
    throw new Error(`${entry.id}: assetUrl must be a GitHub release asset, got ${assetUrl}.`);
  }
  return { ...entry, version, assetUrl, assetName };
}

async function fetchOne(home: string, source: RealExtension): Promise<void> {
  const entry = resolved(source);
  const dir = cacheDir(home, entry);
  mkdirSync(dir, { recursive: true });

  const response = await fetch(entry.assetUrl, { redirect: "follow" });
  if (!response.ok) throw new Error(`${entry.id}: ${entry.assetUrl} answered ${response.status}.`);
  const bytes = new Uint8Array(await response.arrayBuffer());
  const digest = sha256Hex(bytes);
  const state = verifyDigest(entry, digest);

  const zip = join(dir, entry.assetName);
  writeFileSync(`${zip}.partial`, bytes);
  renameSync(`${zip}.partial`, zip);
  const unpacked = join(dir, "unpacked");
  rmSync(unpacked, { recursive: true, force: true });
  execFileSync("unzip", ["-q", "-o", zip, "-d", unpacked], { stdio: "inherit" });

  console.log(
    `${entry.id} ${entry.version}: ${state === "verified" ? "digest verified" : `digest not recorded; sha256 ${digest}`}`,
  );
  console.log(`  unpacked at ${unpacked}`);
  if (state === "unrecorded")
    console.log(
      `  record "sha256": "${digest}" for ${entry.id} in tests/extensions/real/extensions.json`,
    );
}

export async function main(argv: string[], home: string): Promise<number> {
  const all = loadExtensions();
  const chosen = argv.length === 0 ? all : all.filter((entry) => argv.includes(entry.id));
  const unknown = argv.filter((id) => !all.some((entry) => entry.id === id));
  if (unknown.length > 0) {
    console.error(
      `unknown extension: ${unknown.join(", ")}. Known: ${all.map((entry) => entry.id).join(", ")}`,
    );
    return 2;
  }
  let status = 0;
  for (const entry of chosen) {
    try {
      await fetchOne(home, entry);
    } catch (error) {
      console.error(error instanceof Error ? error.message : String(error));
      status = 1;
    }
  }
  return status;
}

if (isMain(import.meta.url)) process.exitCode = await main(process.argv.slice(2), resolveBdkHome());
