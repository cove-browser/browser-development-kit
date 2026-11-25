import { readFileSync } from "node:fs";
import { join, posix } from "node:path";

/** `chromium/runtime-files.json`: what a runtime is made of, relative to `out/<config>`. */
export interface RuntimeFiles {
  readonly schema: 1;
  readonly target: { readonly os: string; readonly min_os: string };
  /** Files or directories (copied recursively, symlinks preserved) relative to the output directory. */
  readonly paths: readonly string[];
  /**
   * The third-party notices (T-REL-02): a file of the build output copied to `to` at the top of the runtime.
   * Chromium writes its credits page to `gen/components/resources/about_credits.html`, a placeholder unless
   * `generate_about_credits = true` (chromium/gn/README.md); packaging refuses the placeholder.
   */
  readonly notices?: { readonly from: string; readonly to: string };
}

function relativePath(p: unknown): string {
  if (
    typeof p !== "string" ||
    p === "" ||
    posix.isAbsolute(p) ||
    posix.normalize(p).startsWith("..")
  ) {
    throw new Error(`chromium/runtime-files.json: invalid path ${JSON.stringify(p)}.`);
  }
  return posix.normalize(p).replace(/\/$/, "");
}

export function runtimeFilesPath(repoRoot: string): string {
  return join(repoRoot, "chromium", "runtime-files.json");
}

export function readRuntimeFiles(repoRoot: string): RuntimeFiles {
  const raw: unknown = JSON.parse(readFileSync(runtimeFilesPath(repoRoot), "utf8"));
  const o = raw as {
    schema?: unknown;
    target?: { os?: unknown; min_os?: unknown };
    paths?: unknown;
    notices?: { from?: unknown; to?: unknown };
  };
  if (o.schema !== 1) throw new Error("chromium/runtime-files.json: schema must be 1.");
  const { os, min_os: minOs } = o.target ?? {};
  if (typeof os !== "string" || typeof minOs !== "string") {
    throw new Error("chromium/runtime-files.json: target.os and target.min_os must be strings.");
  }
  if (!Array.isArray(o.paths) || o.paths.length === 0) {
    throw new Error("chromium/runtime-files.json: paths must be a non-empty array.");
  }
  const paths = o.paths.map(relativePath);
  if (o.notices === undefined) return { schema: 1, target: { os, min_os: minOs }, paths };
  const from = relativePath(o.notices.from);
  const to = relativePath(o.notices.to);
  if (to.includes("/") || to === "manifest.json" || paths.includes(to)) {
    throw new Error(
      "chromium/runtime-files.json: notices.to must be a file name at the top of the runtime, not manifest.json or a declared path.",
    );
  }
  return { schema: 1, target: { os, min_os: minOs }, paths, notices: { from, to } };
}
