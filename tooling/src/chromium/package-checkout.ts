import { loadSeriesWithApp } from "./common.js";
import { seriesState } from "./patch-apply.js";

export interface VerifyCheckoutOptions {
  /** Chromium `src`. */
  readonly srcDir: string;
  /** `chromium/patches` of the BDK repository. */
  readonly patchesDir: string;
  /** An application's series (source mode), which must follow BDK's in `src`. */
  readonly appPatchesDir?: string;
  /** `commit` from `chromium/pin.json`. */
  readonly pinCommit: string;
}

/**
 * A built checkout is the pin plus the BDK patch series applied with `git am`, so its HEAD is never
 * the pin itself. Packaging is allowed only when `src` is exactly the pin plus exactly the current
 * series (and the application's, when given) and has no uncommitted tracked changes. Returns the pin
 * commit (the packager's `checkoutBase`).
 */
export async function verifyCheckout(opts: VerifyCheckoutOptions): Promise<string> {
  const { srcDir, patchesDir, pinCommit } = opts;
  const { patches } = loadSeriesWithApp(patchesDir, opts.appPatchesDir);
  const state = await seriesState(srcDir, pinCommit, patches);
  const remedy = "Run pnpm chromium:sync and pnpm chromium:build before packaging.";
  if (state.dirty.length > 0) {
    throw new Error(
      `The checkout ${srcDir} has uncommitted changes to tracked files:\n  ${state.dirty.slice(0, 10).join("\n  ")}\n${remedy}`,
    );
  }
  if (!state.applied) {
    const series =
      opts.appPatchesDir === undefined
        ? "current chromium/patches series"
        : `current chromium/patches series followed by the application's series in ${opts.appPatchesDir}`;
    throw new Error(
      `The checkout ${srcDir} is at ${state.head}, which is not chromium/pin.json's ${pinCommit} plus exactly the ` +
        `${series} (${state.atOrAbovePin ? `${state.extraCommits} commit(s) above the pin` : "not a descendant of the pin"}). ${remedy}`,
    );
  }
  return pinCommit;
}
