import { describe, expect, it } from "vitest";
import { genericPlatform } from "./generic/index.js";
import { selectPlatform } from "./select.js";
import { xcodePlatform } from "./xcode/index.js";

describe("selectPlatform", () => {
  it("selects the xcode module on macOS", () => {
    expect(selectPlatform("darwin")).toBe(xcodePlatform);
  });

  it.each(["linux", "win32", "freebsd"] as const)("selects the generic module on %s", (host) => {
    expect(selectPlatform(host)).toBe(genericPlatform);
  });

  it("selects for the current host by default", () => {
    expect(selectPlatform().name).toBe(process.platform === "darwin" ? "xcode" : "generic");
  });
});

describe("genericPlatform", () => {
  it("has no IDE build and no bundle facts", async () => {
    expect(genericPlatform.isIdeBuild({ XCODE_VERSION_ACTUAL: "1" })).toBe(false);
    expect(await genericPlatform.bundleFacts({}, "/p")).toBeUndefined();
  });
});
