import { mkdir, readFile, writeFile } from "node:fs/promises";
import { join } from "node:path";

/** Where a project records the runtime it resolved (sdk/cli.md section 5, `bdk build runtime`). */
export const RUNTIME_POINTER = join(".bdk", "runtime.json");

/**
 * Where, inside a packaged runtime, the frameworks an application embeds are: `bdk build embed` copies each
 * entry of this directory to the application's `Contents/Frameworks`, as the runtime's own
 * `Chromium.app` holds them.
 */
export const EMBEDDED_DIR = "Chromium.app/Contents/Frameworks";

export interface RuntimePointer {
  source_key: string;
  manifest_sha256: string;
}

/** The project's `.bdk/runtime.json`, or undefined when it has none or it is unreadable. */
export async function readRuntimePointer(projectDir: string): Promise<RuntimePointer | undefined> {
  try {
    const parsed: unknown = JSON.parse(await readFile(join(projectDir, RUNTIME_POINTER), "utf8"));
    if (
      typeof parsed === "object" &&
      parsed !== null &&
      typeof (parsed as RuntimePointer).source_key === "string" &&
      typeof (parsed as RuntimePointer).manifest_sha256 === "string"
    ) {
      return parsed as RuntimePointer;
    }
  } catch {
    // None, or unreadable: the runtime step writes it again.
  }
  return undefined;
}

/** Writes `.bdk/runtime.json`, only when it changes, so its modification time is stable. */
export async function writeRuntimePointer(
  projectDir: string,
  pointer: RuntimePointer,
): Promise<void> {
  const file = join(projectDir, RUNTIME_POINTER);
  const text = `${JSON.stringify(
    { source_key: pointer.source_key, manifest_sha256: pointer.manifest_sha256 },
    null,
    2,
  )}\n`;
  const current = await readFile(file, "utf8").catch(() => "");
  if (current === text) return;
  await mkdir(join(projectDir, ".bdk"), { recursive: true });
  await writeFile(file, text);
}

/**
 * The manifest's files that `bdk build embed` copies, keyed by their path under `Contents/Frameworks`.
 */
export function embeddedFiles(files: Readonly<Record<string, string>>): Record<string, string> {
  const prefix = `${EMBEDDED_DIR}/`;
  return Object.fromEntries(
    Object.entries(files)
      .filter(([path]) => path.startsWith(prefix))
      .map(([path, digest]) => [path.slice(prefix.length), digest]),
  );
}
