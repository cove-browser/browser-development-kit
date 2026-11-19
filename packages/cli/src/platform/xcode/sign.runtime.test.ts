/**
 * End to end, on a real packaged runtime. Opt in: set BDK_TEST_RUNTIME_APP to a runtime's `Chromium.app`
 * (for example `~/.bdk/runtimes/<key>/Chromium.app`). The runtime is cloned (`ditto`, a copy-on-write clone
 * on APFS) into a temporary directory and never modified. packages/cli/README.md describes the test.
 */
import { spawnSync } from "node:child_process";
import { rm, writeFile } from "node:fs/promises";
import { join } from "node:path";
import { describe, expect, it } from "vitest";
import { run } from "../../core/run.js";
import { tempDir, tempProject, testContext } from "../../test-support.js";
import { createXcodePlatform } from "./index.js";

const runtimeApp = process.env.BDK_TEST_RUNTIME_APP;
const enabled = process.platform === "darwin" && runtimeApp !== undefined && runtimeApp !== "";
if (!enabled) {
  console.warn(
    "runtime signing test SKIPPED: set BDK_TEST_RUNTIME_APP to a runtime's Chromium.app to run it.",
  );
}

function tool(command: string, args: string[]) {
  const r = spawnSync(command, args, { encoding: "utf8", timeout: 170_000 });
  return { status: r.status, out: `${r.stdout}${r.stderr}` };
}

/** A clone of the runtime that looks like another application: other bundle identifier and name. */
async function secondApplication(source: string): Promise<{ app: string; scratch: string }> {
  const scratch = await tempDir("bdk-sign-e2e-");
  const app = join(scratch, "Second.app");
  expect(tool("ditto", [source, app]).status).toBe(0);
  expect(tool("chmod", ["-R", "u+w", app]).status).toBe(0);
  const plist = join(app, "Contents", "Info.plist");
  for (const [key, value] of [
    ["CFBundleIdentifier", "com.second.Browser"],
    ["CFBundleName", "Second"],
    ["CFBundleDisplayName", "Second"],
  ] as const) {
    expect(tool("plutil", ["-replace", key, "-string", value, plist]).status).toBe(0);
  }
  return { app, scratch };
}

describe.skipIf(!enabled)("signing a real runtime (BDK_TEST_RUNTIME_APP)", () => {
  it("refuses a denied entitlement and leaves the bundle untouched", async () => {
    const { app, scratch } = await secondApplication(runtimeApp as string);
    const entitlements = join(scratch, "bad.entitlements");
    await writeFile(
      entitlements,
      '<?xml version="1.0" encoding="UTF-8"?><plist version="1.0"><dict><key>com.apple.security.app-sandbox</key><true/></dict></plist>',
    );
    const before = tool("codesign", ["-dv", app]).out;
    const { dir } = await tempProject();
    const ctx = testContext({ cwd: dir, platform: createXcodePlatform() });
    const code = await run(
      ["build", "sign", "--bundle", app, "--identity", "-", "--entitlements", entitlements],
      ctx,
    );
    expect(code).toBe(1);
    expect(ctx.err.join("")).toContain("[sign.entitlement_denied]");
    expect(tool("codesign", ["-dv", app]).out).toBe(before);
    await rm(scratch, { recursive: true, force: true });
  }, 180_000);

  it("re-signs ad hoc under another identity, verifies, launches and renders a page", async () => {
    const { app, scratch } = await secondApplication(runtimeApp as string);
    const { dir } = await tempProject();
    const ctx = testContext({ cwd: dir, platform: createXcodePlatform() });
    expect(await run(["build", "sign", "--bundle", app, "--identity", "-"], ctx)).toBe(0);

    expect(tool("codesign", ["--verify", "--deep", "--strict", app]).status).toBe(0);

    const helpers = join(
      app,
      "Contents/Frameworks/Chromium Framework.framework/Versions/Current/Helpers",
    );
    const identifier = (path: string) =>
      /^Identifier=(.+)$/m.exec(tool("codesign", ["-dv", path]).out)?.[1];
    expect(identifier(app)).toBe("com.second.Browser");
    expect(identifier(join(helpers, "Chromium Helper.app"))).toBe("com.second.Browser.helper");
    expect(identifier(join(helpers, "Chromium Helper (Renderer).app"))).toBe(
      "com.second.Browser.helper.renderer",
    );
    expect(
      tool("codesign", ["-d", "--entitlements", "-", join(helpers, "Chromium Helper (GPU).app")])
        .out,
    ).toContain("com.apple.security.cs.allow-jit");

    // A rendered page proves the browser, renderer and GPU processes launched after re-signing.
    const profile = join(scratch, "profile");
    const launched = tool(join(app, "Contents/MacOS/Chromium"), [
      "--headless=new",
      `--user-data-dir=${profile}`,
      "--no-first-run",
      "--dump-dom",
      "data:text/html,<p>hi</p>",
    ]);
    expect(launched.out).toContain("<body><p>hi</p></body>");

    // Ad hoc signatures are not accepted by Gatekeeper: that is the expected result.
    const gatekeeper = tool("spctl", ["--assess", "--type", "execute", "-vv", app]);
    expect(gatekeeper.status).not.toBe(0);
    console.log(`spctl --assess (ad hoc): exit ${gatekeeper.status}: ${gatekeeper.out.trim()}`);
    await rm(scratch, { recursive: true, force: true });
  }, 180_000);
});
