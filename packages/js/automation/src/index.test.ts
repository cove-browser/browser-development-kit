import { describe, expect, it } from "vitest";
import { packageName } from "./index.js";

describe("@bdk/automation", () => {
  it("exports its package name", () => {
    expect(packageName).toBe("@bdk/automation");
  });
});
