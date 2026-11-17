import { writeFileSync } from "node:fs";
import { join } from "node:path";
import { describe, expect, it } from "vitest";
import { verifyCheckout } from "./package-checkout.js";
import { applySeries } from "./patch-apply.js";
import { exportSeries } from "./patch-export.js";
import { commitBdkHook, gitSh, makeSrc, makeUpstream, tmp } from "./test-support.js";

/** A pin, a patches dir exporting one BDK commit, and a src with that series applied by `git am`. */
async function built() {
  const up = await makeUpstream();
  const first = await makeSrc(up, "1.0.0");
  await commitBdkHook(first);
  const patchesDir = join(tmp("patches"), "patches");
  await exportSeries({ srcDir: first, pinCommit: up.commit1, patchesDir });
  const src = await makeSrc(up, "1.0.0");
  await applySeries({ srcDir: src, pinCommit: up.commit1, patchesDir });
  return { up, src, patchesDir, opts: { srcDir: src, patchesDir, pinCommit: up.commit1 } };
}

describe("verifyCheckout (what chromium:package requires of src)", () => {
  it("accepts the pin plus exactly the series, whose HEAD is not the pin", async () => {
    const { up, src, opts } = await built();
    expect((await gitSh(src, ["rev-parse", "HEAD"])).trim()).not.toBe(up.commit1);
    await expect(verifyCheckout(opts)).resolves.toBe(up.commit1);
  });

  it("refuses a checkout still at the bare pin when the series is not empty", async () => {
    const { up, patchesDir } = await built();
    const bare = await makeSrc(up, "1.0.0");
    await expect(
      verifyCheckout({ srcDir: bare, patchesDir, pinCommit: up.commit1 }),
    ).rejects.toThrow(/chromium:sync/);
  });

  it("refuses uncommitted tracked changes", async () => {
    const { src, opts } = await built();
    writeFileSync(join(src, "chrome", "app", "framework.exports"), "_Changed\n");
    await expect(verifyCheckout(opts)).rejects.toThrow(/uncommitted changes/);
  });

  it("refuses an extra local commit beyond the series", async () => {
    const { src, opts } = await built();
    writeFileSync(join(src, "extra.txt"), "x\n");
    await gitSh(src, ["add", "extra.txt"]);
    await gitSh(src, ["commit", "-q", "-m", "extra"]);
    await expect(verifyCheckout(opts)).rejects.toThrow(/1 commit\(s\)|2 commit\(s\)|plus exactly/);
  });
});
