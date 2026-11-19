import { describe, expect, it } from "vitest";
import type { Platform } from "../../platform/platform.js";
import { tempProject, testContext } from "../../test-support.js";
import { run } from "../run.js";
import type { SignRequest } from "../signing/types.js";

function signingPlatform(requests: SignRequest[], denied = false): Platform {
  return {
    name: "fake",
    isIdeBuild: () => false,
    bundleFacts: () => Promise.resolve(undefined),
    sign: (request) => {
      requests.push(request);
      return Promise.resolve(
        denied
          ? {
              diagnostics: [
                { code: "sign.entitlement_denied", severity: "error" as const, message: "no" },
              ],
              signed: [],
            }
          : { diagnostics: [], signed: ["/x"] },
      );
    },
  };
}

describe("bdk build sign", () => {
  it("passes the bundle, identity, team and entitlements to the platform", async () => {
    const { dir } = await tempProject();
    const requests: SignRequest[] = [];
    const ctx = testContext({ cwd: dir, platform: signingPlatform(requests) });
    const code = await run(
      [
        "build",
        "sign",
        "--bundle",
        "A.app",
        "--identity",
        "-",
        "--team",
        "T",
        "--entitlements",
        "e.plist",
      ],
      ctx,
    );
    expect(code).toBe(0);
    expect(requests).toEqual([
      { appPath: `${dir}/A.app`, identity: "-", team: "T", entitlementsFile: `${dir}/e.plist` },
    ]);
  });

  it("takes everything from Xcode's build environment when no flags are given", async () => {
    const { dir } = await tempProject();
    const requests: SignRequest[] = [];
    const ctx = testContext({
      cwd: dir,
      platform: signingPlatform(requests),
      env: {
        CODESIGNING_FOLDER_PATH: "/b/A.app",
        EXPANDED_CODE_SIGN_IDENTITY: "Apple Development: x",
        DEVELOPMENT_TEAM: "TEAM",
        CODE_SIGN_ENTITLEMENTS: "App/A.entitlements",
        SRCROOT: "/src",
      },
    });
    expect(await run(["build", "sign"], ctx)).toBe(0);
    expect(requests).toEqual([
      {
        appPath: "/b/A.app",
        identity: "Apple Development: x",
        team: "TEAM",
        entitlementsFile: "/src/App/A.entitlements",
      },
    ]);
  });

  it("skips with a warning when BDK_SKIP_SIGNING=1", async () => {
    const { dir } = await tempProject();
    const requests: SignRequest[] = [];
    const ctx = testContext({
      cwd: dir,
      platform: signingPlatform(requests),
      env: { BDK_SKIP_SIGNING: "1" },
    });
    expect(await run(["build", "sign"], ctx)).toBe(0);
    expect(requests).toEqual([]);
    expect(ctx.err.join("")).toContain("sign.skipped");
  });

  it("is a usage error without a bundle or without an identity", async () => {
    const { dir } = await tempProject();
    const a = testContext({ cwd: dir });
    expect(await run(["build", "sign", "--identity", "-"], a)).toBe(2);
    const b = testContext({ cwd: dir });
    expect(await run(["build", "sign", "--bundle", "A.app"], b)).toBe(2);
  });

  it("exits 1 when the platform reports an error diagnostic", async () => {
    const { dir } = await tempProject();
    const ctx = testContext({ cwd: dir, platform: signingPlatform([], true) });
    expect(await run(["build", "sign", "--bundle", "A.app", "--identity", "-"], ctx)).toBe(1);
  });

  it("exits 3 on a host that cannot sign", async () => {
    const { dir } = await tempProject();
    const ctx = testContext({ cwd: dir });
    expect(await run(["build", "sign", "--bundle", "A.app", "--identity", "-"], ctx)).toBe(3);
    expect(ctx.err.join("")).toContain("sign.unsupported_host");
  });

  it("rejects a sign flag on another step", async () => {
    const ctx = testContext();
    expect(await run(["build", "config", "--bundle", "A.app"], ctx)).toBe(2);
    expect(ctx.err.join("")).toContain("applies to `bdk build sign`");
  });
});
