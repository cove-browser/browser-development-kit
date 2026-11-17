import { spawn } from "node:child_process";
import { randomUUID } from "node:crypto";
import { mkdirSync, readFileSync, rmSync } from "node:fs";
import { join } from "node:path";
import { type BdkPaths, currentBdkPaths } from "./paths.js";

/** `managed` is `$BDK_HOME/chromium/managed`; an experiment is `$BDK_HOME/chromium/exp/<id>`. */
export type CheckoutId = "managed" | { experiment: string };

export interface LockOwner {
  worktree: string;
  pid: number;
  command: string;
  started: string;
}

/** Exit code that both `lockf -t 0` (EX_TEMPFAIL) and `flock -n -E 75` use when the lock is held. */
const BUSY_EXIT = 75;

export class LockBusyError extends Error {
  override readonly name = "LockBusyError";
  constructor(
    readonly checkoutLabel: string,
    readonly lockPath: string,
    readonly owner?: LockOwner,
  ) {
    super(
      owner
        ? `Checkout "${checkoutLabel}" is locked by pid ${owner.pid} (${owner.command}), started ${owner.started} from worktree ${owner.worktree}. Wait for it to finish; the lock is released when that process exits.`
        : `Checkout "${checkoutLabel}" is locked (${lockPath}) and has no owner record. It is held by a process that did not write one, for example a manual bootstrap. Wait for it to finish.`,
    );
  }
}

export function checkoutLabel(checkout: CheckoutId): string {
  return checkout === "managed" ? "managed" : `exp-${checkout.experiment}`;
}

/** `$BDK_HOME/chromium/managed` or `$BDK_HOME/chromium/exp/<id>`. */
export function checkoutRoot(checkout: CheckoutId, paths: BdkPaths = currentBdkPaths()): string {
  return checkout === "managed"
    ? paths.chromiumManaged
    : paths.chromiumExperiment(checkout.experiment);
}

/** `$BDK_HOME/chromium/locks/managed.lock` or `exp-<id>.lock`. */
export function lockFilePath(checkout: CheckoutId, paths: BdkPaths = currentBdkPaths()): string {
  if (checkout !== "managed") paths.chromiumExperiment(checkout.experiment); // validates the id
  return join(paths.chromiumLocks, `${checkoutLabel(checkout)}.lock`);
}

export function ownerFilePath(checkout: CheckoutId, paths: BdkPaths = currentBdkPaths()): string {
  return join(paths.chromiumLocks, `${checkoutLabel(checkout)}.owner.json`);
}

interface OwnerFile extends LockOwner {
  token?: string;
}

function readOwnerFile(path: string): OwnerFile | undefined {
  try {
    const parsed = JSON.parse(readFileSync(path, "utf8")) as Partial<OwnerFile>;
    if (typeof parsed.pid !== "number" || typeof parsed.worktree !== "string") return undefined;
    return {
      worktree: parsed.worktree,
      pid: parsed.pid,
      command: String(parsed.command ?? ""),
      started: String(parsed.started ?? ""),
      ...(typeof parsed.token === "string" ? { token: parsed.token } : {}),
    };
  } catch {
    return undefined;
  }
}

function alive(pid: number): boolean {
  try {
    process.kill(pid, 0);
    return true;
  } catch (e) {
    return (e as NodeJS.ErrnoException).code === "EPERM";
  }
}

/** The owner record if it names a live process. A record left by a crashed run is ignored. */
export function readLockOwner(
  checkout: CheckoutId,
  paths: BdkPaths = currentBdkPaths(),
): LockOwner | undefined {
  const owner = readOwnerFile(ownerFilePath(checkout, paths));
  if (!owner || !alive(owner.pid)) return undefined;
  const { token: _token, ...plain } = owner;
  return plain;
}

/**
 * Runs `argv` while holding the checkout's exclusive OS advisory lock: `lockf -t 0` on macOS and
 * `flock -n` elsewhere. Fails at once with LockBusyError (naming the owner from the owner record)
 * if the lock is held. The kernel releases the lock when the process exits, so a crash leaves no
 * stale lock. Resolves with the child's exit code.
 */
export async function runUnderCheckoutLock(
  checkout: CheckoutId,
  argv: string[],
  opts: { cwd?: string; env?: NodeJS.ProcessEnv; paths?: BdkPaths } = {},
): Promise<number> {
  const paths = opts.paths ?? currentBdkPaths();
  const lockPath = lockFilePath(checkout, paths);
  const ownerPath = ownerFilePath(checkout, paths);
  if (argv.length === 0) throw new Error("runUnderCheckoutLock: argv is empty");
  mkdirSync(paths.chromiumLocks, { recursive: true });

  const token = randomUUID();
  const locker =
    process.platform === "darwin"
      ? ["lockf", "-k", "-t", "0", lockPath]
      : ["flock", "-n", "-E", String(BUSY_EXIT), lockPath];
  // Runs inside the lock: write the owner record, then replace the shell with the command, so the
  // recorded pid is the command's own pid.
  const script =
    'printf \'{"worktree":%s,"pid":%s,"command":%s,"started":%s,"token":%s}\\n\' "$1" "$$" "$2" "$3" "$4" > "$5"; shift 5; exec "$@"';
  const args = [
    ...locker.slice(1),
    "sh",
    "-c",
    script,
    "sh",
    JSON.stringify(opts.cwd ?? process.cwd()),
    JSON.stringify(argv.join(" ")),
    JSON.stringify(new Date().toISOString()),
    JSON.stringify(token),
    ownerPath,
    ...argv,
  ];

  const code = await new Promise<number>((resolve, reject) => {
    const child = spawn(locker[0] as string, args, {
      cwd: opts.cwd,
      env: opts.env ?? process.env,
      stdio: "inherit",
    });
    child.on("error", reject);
    child.on("close", (c, signal) => resolve(c ?? (signal ? 128 : 1)));
  });

  if (code === BUSY_EXIT) {
    const owner = readOwnerFile(ownerPath);
    // The child can itself exit 75; its record carries our token.
    if (owner?.token !== token) {
      const live = owner && alive(owner.pid) ? owner : undefined;
      if (live) {
        const { token: _t, ...plain } = live;
        throw new LockBusyError(checkoutLabel(checkout), lockPath, plain);
      }
      throw new LockBusyError(checkoutLabel(checkout), lockPath);
    }
  }
  if (readOwnerFile(ownerPath)?.token === token) rmSync(ownerPath, { force: true });
  return code;
}

/** Non-blocking probe: is the checkout lock held right now? (Takes and drops the lock if free.) */
export async function probeCheckoutLock(
  checkout: CheckoutId,
  paths: BdkPaths = currentBdkPaths(),
): Promise<{ locked: false } | { locked: true; owner?: LockOwner }> {
  try {
    await runUnderCheckoutLock(checkout, ["true"], { paths });
    return { locked: false };
  } catch (e) {
    if (e instanceof LockBusyError)
      return e.owner ? { locked: true, owner: e.owner } : { locked: true };
    throw e;
  }
}
