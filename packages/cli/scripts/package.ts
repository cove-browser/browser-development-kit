/**
 * Builds `packages/cli/dist/bdk`: bundle `src/main.ts` with Rolldown, then `node --build-sea` with every file
 * under `assets/` embedded, then (macOS) sign ad hoc and lay out the Swift package artifact bundle.
 * Run through `pnpm exec nx run cli:package`. Docs: packages/cli/README.md.
 */
import { execFileSync } from "node:child_process";
import { copyFileSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { rolldown } from "rolldown";
import { artifactBundleInfo, listAssets, seaConfig } from "./package-lib.js";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const dist = join(root, "dist");
const work = join(dist, "sea");
const exe = join(dist, "bdk");

function readVersion(): string {
  const parsed: unknown = JSON.parse(readFileSync(join(root, "package.json"), "utf8"));
  if (typeof parsed === "object" && parsed !== null && "version" in parsed) {
    if (typeof parsed.version === "string") return parsed.version;
  }
  throw new Error("packages/cli/package.json has no version.");
}

async function bundle(version: string, file: string): Promise<void> {
  const build = await rolldown({
    input: join(root, "src", "main.ts"),
    platform: "node",
    // The bundle is evaluated by the executable's CommonJS loader.
    transform: { define: { __BDK_CLI_VERSION__: JSON.stringify(version) } },
    logLevel: "warn",
  });
  await build.write({ file, format: "cjs", minify: false, sourcemap: false });
  await build.close();
}

function artifactTriple(): string {
  return `${process.arch === "arm64" ? "arm64" : "x86_64"}-apple-macosx`;
}

async function main(): Promise<void> {
  const version = readVersion();
  // dist/ also holds the tsc output of `cli:build`, so remove only what this script makes.
  for (const path of [work, exe, join(dist, "bdk.artifactbundle")]) {
    rmSync(path, { recursive: true, force: true });
  }
  mkdirSync(work, { recursive: true });

  const bundleFile = join(work, "bdk.cjs");
  await bundle(version, bundleFile);

  const assets = listAssets(join(root, "assets"));
  const configFile = join(work, "sea-config.json");
  writeFileSync(
    configFile,
    JSON.stringify(
      seaConfig({ main: bundleFile, output: exe, assets, useCodeCache: true }),
      null,
      2,
    ),
  );
  // The executable is a copy of the Node that runs this script, which is the pinned release.
  execFileSync(process.execPath, ["--build-sea", configFile], { stdio: "inherit" });

  if (process.platform === "darwin") {
    execFileSync("codesign", ["--sign", "-", "--force", exe], { stdio: "inherit" });
    const variantDir = `bdk-${version}-macosx/bin`;
    const binDir = join(dist, "bdk.artifactbundle", variantDir);
    mkdirSync(binDir, { recursive: true });
    copyFileSync(exe, join(binDir, "bdk"));
    writeFileSync(
      join(dist, "bdk.artifactbundle", "info.json"),
      `${JSON.stringify(artifactBundleInfo({ version, variantDir, triple: artifactTriple() }), null, 2)}\n`,
    );
  }
  const names = Object.keys(assets);
  console.log(`Packaged ${exe} (${names.length} embedded asset${names.length === 1 ? "" : "s"}).`);
}

main().catch((error: unknown) => {
  console.error(error instanceof Error ? error.message : error);
  process.exitCode = 1;
});
