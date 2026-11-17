import { homedir } from "node:os";
import { posix, win32 } from "node:path";

/** Thrown when BDK_HOME cannot be used. The message says what to change. */
export class BdkHomeError extends Error {
  override readonly name = "BdkHomeError";
}

export interface ResolveOptions {
  /** Environment to read; defaults to `process.env`. */
  readonly env?: Readonly<Record<string, string | undefined>>;
  /** Platform to resolve for; defaults to `process.platform`. */
  readonly platform?: NodeJS.Platform;
  /** Home directory used for the default on macOS and Linux. */
  readonly home?: string;
}

/**
 * Resolves BDK_HOME: `$BDK_HOME` if set, otherwise `~/.bdk` on macOS and Linux
 * and `%LOCALAPPDATA%\bdk` on Windows. The result is absolute and contains no
 * spaces, because Chromium's build does not tolerate them reliably.
 */
export function resolveBdkHome(options: ResolveOptions = {}): string {
  const env = options.env ?? process.env;
  const platform = options.platform ?? process.platform;
  const p = platform === "win32" ? win32 : posix;

  let home: string;
  const explicit = env.BDK_HOME;
  if (explicit !== undefined && explicit !== "") {
    home = explicit;
  } else if (platform === "win32") {
    const localAppData = env.LOCALAPPDATA;
    if (localAppData === undefined || localAppData === "") {
      throw new BdkHomeError("BDK_HOME is not set and LOCALAPPDATA is empty. Set BDK_HOME.");
    }
    home = p.join(localAppData, "bdk");
  } else {
    home = p.join(options.home ?? homedir(), ".bdk");
  }

  if (!p.isAbsolute(home)) {
    throw new BdkHomeError(`BDK_HOME must be an absolute path, got "${home}".`);
  }
  if (/\s/.test(home)) {
    throw new BdkHomeError(
      `BDK_HOME must not contain spaces (the Chromium build does not tolerate them), got "${home}". ` +
        "Set BDK_HOME to a path without spaces.",
    );
  }
  return p.normalize(home);
}

export interface BdkPaths {
  readonly home: string;
  /** Content-addressed runtime store root. */
  readonly runtimes: string;
  /** `runtimes/<source-key>` */
  runtime(sourceKey: string): string;
  /** depot_tools bare mirrors (`GIT_CACHE_PATH`). */
  readonly gitCache: string;
  /** The one managed checkout. */
  readonly chromiumManaged: string;
  /** `chromium/exp/<id>` */
  chromiumExperiment(id: string): string;
  readonly chromiumLocks: string;
  readonly ccache: string;
}

const SEGMENT = /^[A-Za-z0-9][A-Za-z0-9._-]*$/;

function checkSegment(kind: string, value: string): string {
  if (!SEGMENT.test(value) || value.includes("..")) {
    throw new BdkHomeError(
      `Invalid ${kind} "${value}": use letters, digits, ".", "_" and "-" only.`,
    );
  }
  return value;
}

/** Builds every well-known subpath under `home` (see repository-and-tooling.md section 7). */
export function bdkPaths(home: string, platform: NodeJS.Platform = process.platform): BdkPaths {
  const p = platform === "win32" ? win32 : posix;
  return {
    home,
    runtimes: p.join(home, "runtimes"),
    runtime: (sourceKey) => p.join(home, "runtimes", checkSegment("runtime key", sourceKey)),
    gitCache: p.join(home, "git-cache"),
    chromiumManaged: p.join(home, "chromium", "managed"),
    chromiumExperiment: (id) => p.join(home, "chromium", "exp", checkSegment("experiment id", id)),
    chromiumLocks: p.join(home, "chromium", "locks"),
    ccache: p.join(home, "ccache"),
  };
}

/** Convenience: `bdkPaths(resolveBdkHome())`. */
export function currentBdkPaths(options: ResolveOptions = {}): BdkPaths {
  return bdkPaths(resolveBdkHome(options), options.platform ?? process.platform);
}
