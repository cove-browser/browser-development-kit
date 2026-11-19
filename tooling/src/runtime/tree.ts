import { chmod, lstat, readdir, readlink, realpath } from "node:fs/promises";
import { dirname, isAbsolute, join, posix, sep } from "node:path";

export interface TreeEntry {
  /** Path relative to the root, with `/` separators. */
  readonly rel: string;
  readonly kind: "file" | "dir" | "symlink";
  /** Permission bits (lstat mode & 0o777). */
  readonly mode: number;
  /** Link target as stored, for symlinks. */
  readonly target?: string;
}

const byteOrder = (a: string, b: string): number => Buffer.compare(Buffer.from(a), Buffer.from(b));

/**
 * Lists every entry under `root` (not following symlinks), sorted by path in byte order.
 * Sockets, FIFOs and devices are an error: nothing of that kind belongs in a runtime.
 */
export async function walkTree(root: string): Promise<TreeEntry[]> {
  const out: TreeEntry[] = [];
  const visit = async (abs: string, rel: string): Promise<void> => {
    const names = await readdir(abs);
    for (const name of names) {
      const childAbs = join(abs, name);
      const childRel = rel === "" ? name : posix.join(rel, name);
      const st = await lstat(childAbs);
      const mode = st.mode & 0o777;
      if (st.isSymbolicLink()) {
        out.push({ rel: childRel, kind: "symlink", mode, target: await readlink(childAbs) });
      } else if (st.isDirectory()) {
        out.push({ rel: childRel, kind: "dir", mode });
        await visit(childAbs, childRel);
      } else if (st.isFile()) {
        out.push({ rel: childRel, kind: "file", mode });
      } else {
        throw new Error(
          `Unsupported file type at ${childAbs}: only files, directories and symlinks.`,
        );
      }
    }
  };
  await visit(root, "");
  return out.sort((a, b) => byteOrder(a.rel, b.rel));
}

/** Removes write permission everywhere (executable files stay executable). Symlinks are untouched. */
export async function makeReadOnly(root: string): Promise<void> {
  const entries = await walkTree(root);
  for (const e of entries) {
    if (e.kind === "file") await chmod(join(root, e.rel), e.mode & 0o111 ? 0o555 : 0o444);
  }
  for (const e of [...entries].reverse()) {
    if (e.kind === "dir") await chmod(join(root, e.rel), 0o555);
  }
  await chmod(root, 0o555);
}

/** Restores owner write permission on directories so a tree can be deleted or replaced. */
export async function makeWritable(root: string): Promise<void> {
  let st: Awaited<ReturnType<typeof lstat>>;
  try {
    st = await lstat(root);
  } catch {
    return;
  }
  if (!st.isDirectory()) return;
  await chmod(root, 0o755);
  const entries = await walkTree(root).catch(async () => {
    // A directory deeper down is unreadable: open the tree top-down.
    await openDown(root);
    return walkTree(root);
  });
  for (const e of entries) {
    if (e.kind === "dir") await chmod(join(root, e.rel), 0o755);
    else if (e.kind === "file") await chmod(join(root, e.rel), e.mode | 0o200);
  }
}

async function openDown(dir: string): Promise<void> {
  await chmod(dir, 0o755);
  for (const name of await readdir(dir)) {
    const p = join(dir, name);
    if ((await lstat(p)).isDirectory()) await openDown(p);
  }
}

const MAX_LINK_HOPS = 40;

/**
 * Resolves `target` (as stored in the link at `linkAbs`) through the real filesystem, one component
 * at a time, following chains of symlinks. Returns the final absolute path, or `undefined` when the
 * path leaves `rootReal` at any step, is absolute, or loops. Components that do not exist are taken
 * literally, so a dangling link is judged by where it would point.
 */
async function resolveInside(
  rootReal: string,
  linkAbs: string,
  target: string,
): Promise<string | undefined> {
  if (isAbsolute(target)) return undefined;
  const inside = (p: string): boolean => p === rootReal || p.startsWith(rootReal + sep);
  let cur = await realpath(dirname(linkAbs));
  const queue = target.split("/");
  let hops = 0;
  while (queue.length > 0) {
    const part = queue.shift() as string;
    if (part === "" || part === ".") continue;
    if (part === "..") {
      cur = dirname(cur);
    } else {
      const next = join(cur, part);
      const st = await lstat(next).catch(() => undefined);
      if (st?.isSymbolicLink()) {
        if (++hops > MAX_LINK_HOPS) return undefined;
        const t = await readlink(next);
        if (isAbsolute(t)) return undefined;
        queue.unshift(...t.split("/"));
        continue;
      }
      cur = next;
    }
    if (!inside(cur)) return undefined;
  }
  return cur;
}

/**
 * Every symlink under `root` whose target, resolved through the real filesystem (chains and `..`
 * included), is absolute or lands outside `root`. Entries look like `rel -> target`.
 */
export async function findEscapingLinks(root: string): Promise<string[]> {
  const rootReal = await realpath(root);
  const bad: string[] = [];
  for (const e of await walkTree(root)) {
    if (e.kind !== "symlink") continue;
    const resolved = await resolveInside(rootReal, join(root, e.rel), e.target ?? "");
    if (resolved === undefined) bad.push(`${e.rel} -> ${e.target ?? ""}`);
  }
  return bad;
}
