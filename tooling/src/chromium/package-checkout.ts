import { loadSeries } from "./common.js";
import { seriesState } from "./patch-apply.js";

export interface VerifyCheckoutOptions {
  /** Chromium `src`. */
  readonly srcDir: string;
  /** `chromium/patches` of the BDK repository. */
  readonly patchesDir: string;
  /** `commit` from `chromium/pin.json`. */
  readonly pinCommit: string;
}

/**
 * A built checkout is the pin plus the BDK patch series applied with `git am`, so its HEAD is never
 * the pin itself. Packaging is allowed only when `src` is exactly the pin plus exactly the current
 * series and has no uncommitted tracked changes. Returns the pin commit (the packager's `checkoutBase`).
 */
export async function verifyCheckout(opts: VerifyCheckoutOptions): Promise<string> {
  const { srcDir, patchesDir, pinCommit } = opts;
  const state = await seriesState(srcDir, pinCommit, loadSeries(patchesDir).patches);
  const remedy = "Run pnpm chromium:sync and pnpm chromium:build before packaging.";
  if (state.dirty.length > 0) {
    throw new Error(
      `The checkout ${srcDir} has uncommitted changes to tracked files:\n  ${state.dirty.slice(0, 10).join("\n  ")}\n${remedy}`,
    );
  }
  if (!state.applied) {
    throw new Error(
      `The checkout ${srcDir} is at ${state.head}, which is not chromium/pin.json's ${pinCommit} plus exactly the ` +
        `current chromium/patches series (${state.atOrAbovePin ? `${state.extraCommits} commit(s) above the pin` : "not a descendant of the pin"}). ${remedy}`,
    );
  }
  return pinCommit;
}
