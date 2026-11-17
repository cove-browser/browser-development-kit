import { describe, expect, it } from "vitest";
import { packageName } from "./index.js";

describe("@bdk/internal", () => {
  it("exports its package name", () => {
    expect(packageName).toBe("@bdk/internal");
  });
});
