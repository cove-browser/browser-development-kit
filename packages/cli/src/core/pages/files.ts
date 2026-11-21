import { copyFile, mkdir, readdir, readFile, rm, stat } from "node:fs/promises";
import { dirname, join } from "node:path";
import { cspIssues } from "@bdk/vite/tooling";
import type { Diagnostic } from "../diagnostics.js";

/** The largest file a bundled page may hold. The runtime's data source refuses anything bigger. */
export const MAX_PAGE_FILE_BYTES = 16 * 1024 * 1024;

export interface PageFile {
  /** `/`-separated path inside the page directory. */
  relative: string;
  absolute: string;
  size: number;
}

/** Every file under `dir`, links followed, in a stable order. Dot-files and dot-directories are not pages' files. */
export async function collectFiles(dir: string, prefix = ""): Promise<PageFile[]> {
  const files: PageFile[] = [];
  const entries = await readdir(dir);
  for (const name of entries.sort()) {
    if (name.startsWith(".")) continue;
    const absolute = join(dir, name);
    const info = await stat(absolute);
    if (info.isDirectory()) files.push(...(await collectFiles(absolute, `${prefix}${name}/`)));
    else if (info.isFile()) files.push({ relative: `${prefix}${name}`, absolute, size: info.size });
  }
  return files;
}

const HTML_FILE = /\.html?$/i;

/**
 * What stops a page from shipping: a file over `MAX_PAGE_FILE_BYTES`, and any construct in an HTML file
 * that the content security policy blocks. `shown` is the path to print for a file (usually the source).
 */
export async function checkPageFiles(
  files: readonly PageFile[],
  shown: (file: PageFile) => string,
): Promise<Diagnostic[]> {
  const diagnostics: Diagnostic[] = [];
  for (const file of files) {
    const path = shown(file);
    if (file.size > MAX_PAGE_FILE_BYTES) {
      diagnostics.push({
        code: "pages.too_large",
        severity: "error",
        message: `${path} is ${(file.size / 1024 / 1024).toFixed(1)} MiB; a bundled page file may be at most ${MAX_PAGE_FILE_BYTES / 1024 / 1024} MiB.`,
        file: path,
        fix: "Compress or split the file, load it from the application with an app. command, or remove it.",
      });
    }
    if (!HTML_FILE.test(file.relative)) continue;
    for (const issue of cspIssues(await readFile(file.absolute, "utf8"), path)) {
      diagnostics.push({
        code: "pages.csp",
        severity: "error",
        message: issue.what,
        file: path,
        line: issue.line,
        fix: issue.fix,
      });
    }
  }
  return diagnostics;
}

/** Replaces `destination` with `files`, so nothing from an earlier build is left behind. */
export async function writeFiles(destination: string, files: readonly PageFile[]): Promise<void> {
  await rm(destination, { recursive: true, force: true });
  await mkdir(destination, { recursive: true });
  for (const file of files) {
    const target = join(destination, file.relative);
    await mkdir(dirname(target), { recursive: true });
    await copyFile(file.absolute, target);
  }
}
