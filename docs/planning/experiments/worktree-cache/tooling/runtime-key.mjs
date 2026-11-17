// Computes the BDK runtime identity key from every input that can change the engine binary.
import { createHash } from "node:crypto";
import { existsSync, readFileSync, readdirSync, statSync } from "node:fs";
import { execFileSync } from "node:child_process";
import { join } from "node:path";
import os from "node:os";
const sha = (b) => createHash("sha256").update(b).digest("hex");
const walk = (d) => readdirSync(d).sort().flatMap((n) => { const p = join(d, n); return statSync(p).isDirectory() ? walk(p) : [p]; });
export const bdkHome = () => process.env.BDK_HOME ?? join(os.homedir(), "Library/Caches/bdk");
export function runtimeIdentity() {
  const pin = JSON.parse(readFileSync("chromium/pin.json", "utf8"));
  const files = [...walk("chromium/patches"), ...walk("chromium/overlay"), pin.gn_args].map((f) => [f, sha(readFileSync(f))]);
  const toolchain = execFileSync("sh", ["tooling/toolchain-id.sh"], { encoding: "utf8" }).trim();
  const components = { chromium_tag: pin.chromium_tag, chromium_commit: pin.chromium_commit, bdk_abi: pin.bdk_abi, inputs: files, target: `${os.platform()}-${os.arch()}`, toolchain };
  return { key: sha(JSON.stringify(components)).slice(0, 16), components };
}
if (process.argv[1].endsWith("runtime-key.mjs")) {
  const id = runtimeIdentity();
  if (process.argv.includes("--probe")) {
    // Probe external state too: is the artifact present in the shared store, and what is its manifest digest?
    const m = join(bdkHome(), "runtimes", id.key, "manifest.json");
    console.log(`${id.key} ${existsSync(m) ? sha(readFileSync(m)) : "absent"}`);
  } else console.log(JSON.stringify(id, null, 2));
}
