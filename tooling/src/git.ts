import { spawn } from "node:child_process";

export interface RunOptions {
  readonly cwd?: string;
  readonly env?: NodeJS.ProcessEnv;
  /** Text written to the child's stdin. */
  readonly input?: string;
  /** Pass the child's stdout and stderr through instead of capturing them. */
  readonly inherit?: boolean;
}

export interface RunResult {
  /** Exit code; 128 + signal number is not reconstructed, a signal gives `null`. */
  readonly code: number | null;
  readonly stdout: string;
  readonly stderr: string;
}

/** Runs a command to completion. Never throws on a non-zero exit; throws if it cannot be spawned. */
export function run(
  cmd: string,
  args: readonly string[],
  options: RunOptions = {},
): Promise<RunResult> {
  return new Promise((resolve, reject) => {
    const child = spawn(cmd, args, {
      cwd: options.cwd,
      env: options.env ?? process.env,
      stdio: options.inherit ? ["pipe", "inherit", "inherit"] : ["pipe", "pipe", "pipe"],
    });
    let stdout = "";
    let stderr = "";
    child.stdout?.setEncoding("utf8").on("data", (d: string) => {
      stdout += d;
    });
    child.stderr?.setEncoding("utf8").on("data", (d: string) => {
      stderr += d;
    });
    child.on("error", reject);
    child.stdin?.on("error", () => {});
    child.on("close", (code) => resolve({ code, stdout, stderr }));
    child.stdin?.end(options.input ?? "");
  });
}

export class GitError extends Error {
  override readonly name = "GitError";
  constructor(
    readonly args: readonly string[],
    readonly result: RunResult,
  ) {
    super(
      `git ${args.join(" ")} failed (exit ${result.code}): ${result.stderr.trim() || result.stdout.trim()}`,
    );
  }
}

/** Identity for commits that BDK tooling creates (`git am`). Fixed so applying a series is reproducible. */
export const BDK_COMMITTER = { name: "BDK Patches", email: "patches@bdk.invalid" } as const;

export const bdkIdentityArgs: readonly string[] = [
  "-c",
  `user.name=${BDK_COMMITTER.name}`,
  "-c",
  `user.email=${BDK_COMMITTER.email}`,
  "-c",
  "commit.gpgsign=false",
];

function gitEnv(extra?: NodeJS.ProcessEnv): NodeJS.ProcessEnv {
  return { ...process.env, GIT_TERMINAL_PROMPT: "0", GIT_EDITOR: "true", LC_ALL: "C", ...extra };
}

/** Runs git in `cwd` and returns the result, whatever the exit code. */
export function gitRaw(
  cwd: string,
  args: readonly string[],
  options: Omit<RunOptions, "cwd"> = {},
): Promise<RunResult> {
  return run("git", args, { ...options, cwd, env: gitEnv(options.env) });
}

/** Runs git in `cwd`; throws GitError on a non-zero exit. Returns stdout. */
export async function git(
  cwd: string,
  args: readonly string[],
  options: Omit<RunOptions, "cwd"> = {},
): Promise<string> {
  const result = await gitRaw(cwd, args, options);
  if (result.code !== 0) throw new GitError(args, result);
  return result.stdout;
}
