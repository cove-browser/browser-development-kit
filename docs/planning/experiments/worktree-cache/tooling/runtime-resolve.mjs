// Prebuilt mode: resolve the immutable shared runtime for this worktree's identity key and verify it.
import { createHash } from "node:crypto";
import { existsSync, mkdirSync, readFileSync, rmSync, symlinkSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { bdkHome, runtimeIdentity } from "./runtime-key.mjs";
const sha = (b) => createHash("sha256").update(b).digest("hex");
const { key } = runtimeIdentity();
const dir = join(bdkHome(), "runtimes", key);
const fail = (m) => { console.error(`runtime-resolve: ${m}`); process.exit(1); };
if (!existsSync(join(dir, "manifest.json"))) fail(`no runtime for key ${key} in ${bdkHome()}/runtimes. Fetch a published runtime for this pin, or run a source build (patches/GN args differ from any published runtime).`);
const manifest = JSON.parse(readFileSync(join(dir, "manifest.json"), "utf8"));
if (manifest.key !== key) fail(`manifest key ${manifest.key} != expected ${key}`);
for (const [f, digest] of Object.entries(manifest.files)) if (sha(readFileSync(join(dir, f))) !== digest) fail(`integrity failure for ${f} in ${dir}`);
mkdirSync(".bdk", { recursive: true });
rmSync(".bdk/runtime", { force: true });
symlinkSync(dir, ".bdk/runtime");
writeFileSync(".bdk/runtime.json", JSON.stringify({ key, manifestDigest: sha(readFileSync(join(dir, "manifest.json"))) }) + "\n");
console.log(`runtime-resolve: ${key} -> ${dir}`);
