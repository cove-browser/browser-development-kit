import { existsSync } from "node:fs";
import { describe, expect, it } from "vitest";
import { tmp } from "./chromium/test-support.js";
import {
  checkoutRoot,
  LockBusyError,
  lockFilePath,
  ownerFilePath,
  probeCheckoutLock,
  readLockOwner,
  runUnderCheckoutLock,
} from "./lock.js";
import { bdkPaths } from "./paths.js";

const haveLocker = process.platform === "darwin" || process.platform === "linux";

describe("checkout paths", () => {
  const paths = bdkPaths("/h");
  it("maps checkouts to directories and lock files", () => {
    expect(checkoutRoot("managed", paths)).toBe("/h/chromium/managed");
    expect(checkoutRoot({ experiment: "a1" }, paths)).toBe("/h/chromium/exp/a1");
    expect(lockFilePath("managed", paths)).toBe("/h/chromium/locks/managed.lock");
    expect(lockFilePath({ experiment: "a1" }, paths)).toBe("/h/chromium/locks/exp-a1.lock");
    expect(ownerFilePath("managed", paths)).toBe("/h/chromium/locks/managed.owner.json");
  });
  it("rejects bad experiment ids", () => {
    expect(() => lockFilePath({ experiment: "../x" }, paths)).toThrow(/Invalid experiment id/);
  });
});

describe.skipIf(!haveLocker)("runUnderCheckoutLock", () => {
  it("returns the child's exit code and releases the lock", async () => {
    const paths = bdkPaths(tmp("lock"));
    expect(await runUnderCheckoutLock("managed", ["sh", "-c", "exit 3"], { paths })).toBe(3);
    expect(await runUnderCheckoutLock("managed", ["true"], { paths })).toBe(0);
    expect(existsSync(ownerFilePath("managed", paths))).toBe(false);
  });

  it("fails at once while held, naming the owner from the record", async () => {
    const paths = bdkPaths(tmp("lock"));
    const holder = runUnderCheckoutLock("managed", ["sleep", "2"], { paths, cwd: "/" });
    await new Promise((r) => setTimeout(r, 500));
    const owner = readLockOwner("managed", paths);
    expect(owner?.command).toBe("sleep 2");
    const started = Date.now();
    const err = await runUnderCheckoutLock("managed", ["true"], { paths }).catch((e: unknown) => e);
    expect(Date.now() - started).toBeLessThan(1000);
    expect(err).toBeInstanceOf(LockBusyError);
    expect((err as LockBusyError).owner).toMatchObject({
      command: "sleep 2",
      worktree: "/",
      pid: owner?.pid,
    });
    expect((err as LockBusyError).message).toContain(String(owner?.pid));
    expect(await probeCheckoutLock("managed", paths)).toMatchObject({ locked: true });
    expect(await holder).toBe(0);
    expect(await probeCheckoutLock("managed", paths)).toEqual({ locked: false });
  });

  it("does not confuse a child exiting 75 with a busy lock", async () => {
    const paths = bdkPaths(tmp("lock"));
    expect(await runUnderCheckoutLock("managed", ["sh", "-c", "exit 75"], { paths })).toBe(75);
  });

  it("locks different checkouts independently", async () => {
    const paths = bdkPaths(tmp("lock"));
    const a = runUnderCheckoutLock("managed", ["sleep", "1"], { paths });
    await new Promise((r) => setTimeout(r, 300));
    expect(await runUnderCheckoutLock({ experiment: "e1" }, ["true"], { paths })).toBe(0);
    await a;
  });

  it("reports a lock held by a process that wrote no owner record", async () => {
    const paths = bdkPaths(tmp("lock"));
    const { spawn } = await import("node:child_process");
    const { mkdirSync } = await import("node:fs");
    mkdirSync(paths.chromiumLocks, { recursive: true });
    const lockfile = lockFilePath("managed", paths);
    const cmd =
      process.platform === "darwin" ? ["lockf", "-t", "0", lockfile] : ["flock", "-n", lockfile];
    const bootstrap = spawn(cmd[0] as string, [...cmd.slice(1), "sleep", "2"], { stdio: "ignore" });
    await new Promise((r) => setTimeout(r, 500));
    const err = await runUnderCheckoutLock("managed", ["true"], { paths }).catch((e: unknown) => e);
    expect(err).toBeInstanceOf(LockBusyError);
    expect((err as LockBusyError).owner).toBeUndefined();
    expect((err as LockBusyError).message).toMatch(/no owner record/);
    await new Promise((r) => bootstrap.on("close", r));
  });
});
