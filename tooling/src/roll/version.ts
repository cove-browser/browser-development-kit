// Stable-version detection for the Chromium roll bot (repository and tooling plan, section 6.1, step 1).

export const VERSION_HISTORY_URL =
  "https://versionhistory.googleapis.com/v1/chrome/platforms/mac/channels/stable/versions?orderBy=version%20desc&pageSize=10";
export const CHROMIUM_DASH_URL =
  "https://chromiumdash.appspot.com/fetch_releases?channel=Stable&platform=Mac&num=50";

export type FetchLike = (url: string) => Promise<Response>;

/** One stable release. `time` is the release date in epoch milliseconds when the source gives one. */
export interface Release {
  readonly version: string;
  readonly time: number | null;
}

export interface StableInfo {
  /** The highest stable version for Mac. */
  readonly version: string;
  /** Known releases with dates (Chromium Dash), used to work out how long the pin has been behind. */
  readonly releases: readonly Release[];
  readonly source: "versionhistory" | "chromiumdash";
}

export class VersionError extends Error {
  override readonly name = "VersionError";
}

/** Splits `a.b.c.d` into numbers. Throws on anything that is not dotted decimal. */
export function parseVersion(v: string): number[] {
  if (!/^\d+(\.\d+)*$/.test(v)) throw new VersionError(`"${v}" is not a dotted numeric version.`);
  return v.split(".").map(Number);
}

/** Numeric, component-wise comparison: negative when a < b, 0 when equal, positive when a > b. */
export function compareVersions(a: string, b: string): number {
  const x = parseVersion(a);
  const y = parseVersion(b);
  for (let i = 0; i < Math.max(x.length, y.length); i++) {
    const d = (x[i] ?? 0) - (y[i] ?? 0);
    if (d !== 0) return d;
  }
  return 0;
}

/** The first dotted component: the Chromium milestone. */
export function milestone(v: string): number {
  return parseVersion(v)[0] as number;
}

function highest(versions: readonly string[]): string {
  const valid = versions.filter((v) => /^\d+(\.\d+)*$/.test(v));
  const top = valid.reduce<string | undefined>(
    (best, v) => (best === undefined || compareVersions(v, best) > 0 ? v : best),
    undefined,
  );
  if (top === undefined) throw new VersionError("the response lists no versions");
  return top;
}

async function getJson(f: FetchLike, url: string): Promise<unknown> {
  const res = await f(url);
  if (!res.ok) throw new VersionError(`${url} answered ${res.status}`);
  return res.json();
}

/** VersionHistory: `{ versions: [{ name, version }] }`, newest first. Carries no release dates. */
export function parseVersionHistory(raw: unknown): string {
  const list = (raw as { versions?: unknown } | null)?.versions;
  if (!Array.isArray(list)) throw new VersionError("VersionHistory: no `versions` array");
  return highest(
    list.flatMap((e) => {
      const v = (e as { version?: unknown } | null)?.version;
      return typeof v === "string" ? [v] : [];
    }),
  );
}

/** Chromium Dash: `[{ version, time (epoch ms), milestone, hashes, ... }]`. */
export function parseDash(raw: unknown): Release[] {
  if (!Array.isArray(raw)) throw new VersionError("Chromium Dash: expected an array");
  return raw.flatMap((e) => {
    const o = e as { version?: unknown; time?: unknown } | null;
    if (typeof o?.version !== "string" || !/^\d+(\.\d+)*$/.test(o.version)) return [];
    return [{ version: o.version, time: typeof o.time === "number" ? o.time : null }];
  });
}

/**
 * The current stable version for Mac. VersionHistory is asked first; Chromium Dash is the fallback and
 * is also read, best effort, for release dates.
 */
export async function detectStable(
  f: FetchLike,
  log: (line: string) => void = () => {},
): Promise<StableInfo> {
  let dash: Release[] | null = null;
  let dashError: unknown;
  try {
    dash = parseDash(await getJson(f, CHROMIUM_DASH_URL));
  } catch (e) {
    dashError = e;
  }
  try {
    const version = parseVersionHistory(await getJson(f, VERSION_HISTORY_URL));
    return { version, releases: dash ?? [], source: "versionhistory" };
  } catch (e) {
    log(`roll: VersionHistory failed (${(e as Error).message}); falling back to Chromium Dash`);
  }
  if (dash === null || dash.length === 0) {
    throw new VersionError(
      `could not detect the stable version: VersionHistory failed and Chromium Dash ${
        dashError === undefined ? "listed no releases" : `failed (${(dashError as Error).message})`
      }`,
    );
  }
  return { version: highest(dash.map((r) => r.version)), releases: dash, source: "chromiumdash" };
}

/**
 * Whole days the pin has been behind stable: now minus the release date of the oldest stable release
 * newer than the pin. Null when no dated release newer than the pin is known.
 */
export function daysBehind(pinTag: string, releases: readonly Release[], now: Date): number | null {
  const times = releases
    .filter((r) => r.time !== null && compareVersions(r.version, pinTag) > 0)
    .map((r) => r.time as number);
  if (times.length === 0) return null;
  return Math.max(0, Math.floor((now.getTime() - Math.min(...times)) / 86_400_000));
}
