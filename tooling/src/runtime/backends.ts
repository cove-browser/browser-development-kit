import { execFile } from "node:child_process";
import { createWriteStream } from "node:fs";
import { copyFile, mkdir, rename, rm } from "node:fs/promises";
import { basename, join } from "node:path";
import { Readable } from "node:stream";
import { pipeline } from "node:stream/promises";
import { fileURLToPath, pathToFileURL } from "node:url";

export interface UploadRequest {
  readonly archivePath: string;
  readonly sourceKey: string;
}

/** A place a runtime archive can be uploaded to. Returns the URL it will be downloaded from. */
export interface PublishBackend {
  readonly name: string;
  upload(request: UploadRequest): Promise<{ readonly url: string }>;
}

/** `file:///some/dir`: copies the archive into that directory (tests, and simulating a second machine). */
export function fileBackend(dirUrl: string): PublishBackend {
  if (!dirUrl.startsWith("file:///"))
    throw new Error(`file backend needs a file:///... directory URL, got "${dirUrl}".`);
  const dir = fileURLToPath(dirUrl);
  return {
    name: "file",
    async upload({ archivePath }) {
      await mkdir(dir, { recursive: true });
      const dest = join(dir, basename(archivePath));
      const tmp = `${dest}.part`;
      await copyFile(archivePath, tmp);
      await rename(tmp, dest);
      return { url: pathToFileURL(dest).href };
    },
  };
}

export type CommandRunner = (cmd: string, args: readonly string[]) => Promise<{ stdout: string }>;

const defaultRunner: CommandRunner = (cmd, args) =>
  new Promise((resolve, reject) => {
    execFile(cmd, [...args], { maxBuffer: 16 * 1024 * 1024 }, (error, stdout, stderr) => {
      if (error)
        reject(new Error(`${cmd} ${args.join(" ")} failed: ${stderr.trim() || error.message}`));
      else resolve({ stdout });
    });
  });

/** Parses `https://github.com/o/r(.git)` and `git@github.com:o/r(.git)` into `o/r`. */
export function parseGithubRepo(remoteUrl: string): string {
  const m = /github\.com[:/]([^/\s]+\/[^/\s]+?)(?:\.git)?\/?$/.exec(remoteUrl.trim());
  if (!m?.[1])
    throw new Error(
      `Cannot find a GitHub owner/repo in remote URL "${remoteUrl}". Pass --repo owner/name.`,
    );
  return m[1];
}

export function githubReleaseTag(sourceKey: string): string {
  return `runtime-${sourceKey}`;
}

export interface GithubOptions {
  /** `owner/name`; default: parsed from the `origin` remote. */
  readonly repo?: string;
  readonly cwd?: string;
  /** Injectable for tests. Never run for real by the test suite. */
  readonly run?: CommandRunner;
}

/**
 * Uploads to a release named `runtime-<source-key>` with `gh`. The release is created if absent. An
 * asset of the same name is never overwritten (no `--clobber`).
 */
export function githubBackend(options: GithubOptions = {}): PublishBackend {
  const run = options.run ?? defaultRunner;
  return {
    name: "github",
    async upload({ archivePath, sourceKey }) {
      const repo =
        options.repo ??
        parseGithubRepo(
          (await run("git", ["-C", options.cwd ?? ".", "remote", "get-url", "origin"])).stdout,
        );
      const tag = githubReleaseTag(sourceKey);
      const exists = await run("gh", [
        "release",
        "view",
        tag,
        "--repo",
        repo,
        "--json",
        "tagName",
      ]).then(
        () => true,
        () => false,
      );
      if (!exists) {
        await run("gh", [
          "release",
          "create",
          tag,
          "--repo",
          repo,
          "--title",
          `BDK runtime ${sourceKey}`,
          "--notes",
          `Prebuilt BDK runtime for source key ${sourceKey}. Verify against chromium/pin.json.`,
          "--prerelease",
        ]);
      }
      await run("gh", ["release", "upload", tag, archivePath, "--repo", repo]);
      return {
        url: `https://github.com/${repo}/releases/download/${tag}/${basename(archivePath)}`,
      };
    },
  };
}

export type Downloader = (url: string, destFile: string) => Promise<void>;

/** Fetches `https://` or copies `file://` to `destFile`. Anything else (including http://) is refused. */
export const download: Downloader = async (url, destFile) => {
  const part = `${destFile}.part`;
  try {
    if (url.startsWith("file:///")) {
      await copyFile(fileURLToPath(url), part);
    } else if (url.startsWith("https://")) {
      const response = await fetch(url, { redirect: "follow" });
      if (!response.ok || response.body === null) {
        throw new Error(`GET ${url} failed: HTTP ${response.status}`);
      }
      await pipeline(Readable.fromWeb(response.body), createWriteStream(part));
    } else {
      throw new Error(`Unsupported runtime URL "${url}": only https:// and file:/// are allowed.`);
    }
    await rename(part, destFile);
  } catch (error) {
    await rm(part, { force: true });
    throw error;
  }
};
