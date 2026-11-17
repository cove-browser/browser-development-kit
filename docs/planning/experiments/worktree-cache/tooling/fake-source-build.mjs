// Stand-in for sync+patch+gn+build+package. Must be run under an exclusive lock on the checkout.
import { createHash } from "node:crypto";
import { chmodSync, existsSync, mkdirSync, readFileSync, renameSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { bdkHome, runtimeIdentity } from "./runtime-key.mjs";
const sha = (b) => createHash("sha256").update(b).digest("hex");
const checkout = process.argv[2] ?? "managed";
const { key, components } = runtimeIdentity();
const home = bdkHome();
const out = join(home, "chromium", "out", checkout, key);
mkdirSync(out, { recursive: true });
writeFileSync(join(home, "chromium", "checkouts", checkout, ".bdk-owner.json"), JSON.stringify({ worktree: process.cwd(), pid: process.pid, key, since: new Date().toISOString() }));
console.log(`[${checkout}] build start key=${key} out=${out}`);
await new Promise((r) => setTimeout(r, Number(process.env.FAKE_BUILD_MS ?? 1500)));
writeFileSync(join(out, "BDK Framework"), `engine built from ${key} in ${process.cwd()}\n`);
const dest = join(home, "runtimes", key);
if (!existsSync(dest)) {
  const tmp = `${dest}.tmp-${process.pid}`;
  mkdirSync(tmp, { recursive: true });
  const payload = readFileSync(join(out, "BDK Framework"));
  writeFileSync(join(tmp, "BDK Framework"), payload);
  writeFileSync(join(tmp, "manifest.json"), JSON.stringify({ key, components, files: { "BDK Framework": sha(payload) }, builder: { checkout, worktree: process.cwd() } }, null, 2));
  chmodSync(join(tmp, "BDK Framework"), 0o444); chmodSync(join(tmp, "manifest.json"), 0o444);
  renameSync(tmp, dest); chmodSync(dest, 0o555);
}
console.log(`[${checkout}] build done key=${key}`);
