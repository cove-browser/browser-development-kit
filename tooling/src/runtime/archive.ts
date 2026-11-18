import { spawn, spawnSync } from "node:child_process";
import { createReadStream, createWriteStream } from "node:fs";
import { mkdir, rm, stat, writeFile } from "node:fs/promises";
import { dirname } from "node:path";
import { sha256File } from "./canonical.js";
import type { ArchiveFormat } from "./pin.js";
import { walkTree } from "./tree.js";

/** Modification time every file in a runtime is set to, so archives do not depend on build time. */
export const FIXED_MTIME = new Date("2020-01-01T00:00:00Z");

export function hasZstd(): boolean {
  return spawnSync("zstd", ["--version"], { stdio: "ignore" }).status === 0;
}

/** zstd when the binary is present, gzip otherwise. */
export function defaultFormat(): ArchiveFormat {
  return hasZstd() ? "tar.zst" : "tar.gz";
}

export function archiveExtension(format: ArchiveFormat): string {
  return format;
}

/** The tar binary: `BDK_TAR` (tests use it to run GNU tar on macOS), else `tar` on PATH. */
function tarBin(): string {
  return process.env.BDK_TAR ?? "tar";
}

const gnuTarCache = new Map<string, boolean>();

/** Whether `tarBin()` is GNU tar (Linux) rather than bsdtar (macOS). Their create flags differ. */
export function tarIsGnu(): boolean {
  const bin = tarBin();
  let gnu = gnuTarCache.get(bin);
  if (gnu === undefined) {
    const r = spawnSync(bin, ["--version"], { encoding: "utf8" });
    gnu = /GNU tar/.test(r.stdout ?? "");
    gnuTarCache.set(bin, gnu);
  }
  return gnu;
}

/** bsdtar: GNU format, owner 0:0 with no names, no extended attributes, ACLs or AppleDouble files. */
const BSD_TAR_CREATE = [
  "-c",
  "--format",
  "gnutar",
  "--no-xattrs",
  "--no-mac-metadata",
  "--no-acls",
  "--uid",
  "0",
  "--gid",
  "0",
  "--uname",
  "",
  "--gname",
  "",
  "--numeric-owner",
  "-n",
];

/**
 * The same for GNU tar, whose names differ: `-n` there means --seek, so --no-recursion; and
 * --verbatim-files-from so that a listed name starting with "-" is not read as an option.
 */
const GNU_TAR_CREATE = [
  "-c",
  "--format=gnu",
  "--no-xattrs",
  "--no-acls",
  "--owner=0",
  "--group=0",
  "--numeric-owner",
  "--no-recursion",
  "--verbatim-files-from",
];

function compressor(format: ArchiveFormat): readonly [string, string[]] {
  return format === "tar.zst"
    ? ["zstd", ["-q", "-T1", "--no-check", "-10", "-c"]]
    : ["gzip", ["-n", "-9", "-c"]];
}

function decompressor(format: ArchiveFormat): readonly [string, string[]] {
  return format === "tar.zst" ? ["zstd", ["-q", "-d", "-c"]] : ["gzip", ["-d", "-c"]];
}

interface Stage {
  readonly cmd: string;
  readonly args: string[];
}

/** Runs `stages` as a pipeline, reading `input` into the first and writing the last to `output`. */
function runPipeline(stages: Stage[], input: string | null, output: string | null): Promise<void> {
  return new Promise((resolve, reject) => {
    const procs = stages.map((s, i) =>
      spawn(s.cmd, s.args, {
        stdio: [
          i === 0 && input === null ? "ignore" : "pipe",
          i === stages.length - 1 && output === null ? "ignore" : "pipe",
          "pipe",
        ],
      }),
    );
    let pending = procs.length + (output === null ? 0 : 1);
    let failed = false;
    const errors: string[] = [];
    const settle = (error?: Error): void => {
      if (error && !failed) {
        failed = true;
        for (const p of procs) p.kill();
        reject(error);
      }
      if (--pending === 0 && !failed) resolve();
    };
    procs.forEach((p, i) => {
      const stage = stages[i] as Stage;
      p.stdin?.on("error", () => {});
      p.stderr?.on("data", (d: Buffer) => errors.push(d.toString()));
      p.on("error", (e) => settle(new Error(`${stage.cmd}: ${e.message}`)));
      p.on("close", (code) =>
        settle(
          code === 0
            ? undefined
            : new Error(`${stage.cmd} exited ${code}: ${errors.join("").trim()}`),
        ),
      );
      const next = procs[i + 1];
      if (next?.stdin && p.stdout) p.stdout.pipe(next.stdin);
    });
    const first = procs[0];
    if (input !== null && first?.stdin) {
      const src = createReadStream(input);
      src.on("error", (e) => settle(e));
      src.pipe(first.stdin);
    }
    const last = procs[procs.length - 1];
    if (output !== null && last?.stdout) {
      const sink = createWriteStream(output);
      sink.on("error", (e) => settle(e));
      sink.on("close", () => settle());
      last.stdout.pipe(sink);
    }
  });
}

export interface ArchiveResult {
  readonly sha256: string;
  readonly size: number;
}

/**
 * Archives the contents of `srcDir` (no wrapping directory) into `outFile`, deterministically: entries in
 * byte order, owner 0:0 with no names, no extended attributes or ACLs, GNU format, with bsdtar or
 * GNU tar. The bytes differ between the two, so compare archives made with the same one. File times
 * come from the tree, which packaging sets to `FIXED_MTIME`.
 */
export async function createArchive(
  srcDir: string,
  outFile: string,
  format: ArchiveFormat,
): Promise<ArchiveResult> {
  const entries = await walkTree(srcDir);
  for (const e of entries) {
    if (e.rel.includes("\n"))
      throw new Error(`Cannot archive a path containing a newline: ${JSON.stringify(e.rel)}`);
  }
  await mkdir(dirname(outFile), { recursive: true });
  const listFile = `${outFile}.list`;
  await writeFile(listFile, `${entries.map((e) => e.rel).join("\n")}\n`);
  const [zc, zargs] = compressor(format);
  try {
    await runPipeline(
      [
        {
          cmd: tarBin(),
          args: [
            ...(tarIsGnu() ? GNU_TAR_CREATE : BSD_TAR_CREATE),
            "-C",
            srcDir,
            "-T",
            listFile,
            "-f",
            "-",
          ],
        },
        { cmd: zc, args: zargs },
      ],
      null,
      outFile,
    );
  } finally {
    await rm(listFile, { force: true });
  }
  return { sha256: await sha256File(outFile), size: (await stat(outFile)).size };
}

/** Extracts `archive` into the existing directory `destDir`. */
export async function extractArchive(
  archive: string,
  destDir: string,
  format: ArchiveFormat,
): Promise<void> {
  const [dc, dargs] = decompressor(format);
  await mkdir(destDir, { recursive: true });
  await runPipeline(
    [
      { cmd: dc, args: dargs },
      { cmd: tarBin(), args: ["-x", "-f", "-", "-C", destDir] },
    ],
    archive,
    null,
  );
}

export function archiveFileName(sourceKey: string, format: ArchiveFormat): string {
  return `bdk-runtime-${sourceKey}.${archiveExtension(format)}`;
}
