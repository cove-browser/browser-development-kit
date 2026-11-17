import { chmod, lstat, readdir, readlink } from "node:fs/promises";
import { join, posix } from "node:path";

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
