import { describe, expect, it } from "vitest";
import { BdkError, isBdkError, SdkErrors } from "./index.js";

describe("BdkError", () => {
  it("carries domain, code, message and details", () => {
    const error = new BdkError({
      domain: "capability",
      code: "denied",
      message: "no",
      details: { capability: "history.read" },
    });
    expect(error).toBeInstanceOf(Error);
    expect(error.name).toBe("BdkError");
    expect(error.message).toBe("no");
    expect(error.domain).toBe("capability");
    expect(error.code).toBe("denied");
    expect(error.details).toEqual({ capability: "history.read" });
  });

  it("has no details property when there are none", () => {
    expect("details" in new BdkError({ domain: "a", code: "b", message: "c" })).toBe(false);
  });
});

describe("isBdkError", () => {
  it("accepts a BdkError", () => {
    expect(isBdkError(new BdkError({ domain: "a", code: "b", message: "c" }))).toBe(true);
  });

  it("accepts the same shape from another copy of the class", () => {
    const foreign = Object.assign(new Error("m"), { name: "BdkError", domain: "a", code: "b" });
    expect(isBdkError(foreign)).toBe(true);
  });

  it("rejects everything else", () => {
    expect(isBdkError(new Error("m"))).toBe(false);
    expect(isBdkError({ domain: "a", code: "b", message: "c" })).toBe(false);
    expect(isBdkError(Object.assign(new Error("m"), { name: "BdkError", domain: 1 }))).toBe(false);
    expect(isBdkError(undefined)).toBe(false);
  });
});

describe("SdkErrors", () => {
  it("names the errors the SDK raises", () => {
    expect(SdkErrors.cancelled).toEqual({ domain: "broker", code: "cancelled" });
    expect(SdkErrors.transportUnavailable).toEqual({
      domain: "broker",
      code: "transportUnavailable",
    });
    expect(SdkErrors.capabilityDenied).toEqual({ domain: "capability", code: "denied" });
  });
});
