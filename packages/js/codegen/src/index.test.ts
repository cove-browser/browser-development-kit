import { describe, expect, it } from "vitest";
import { packageName } from "./index.js";

describe("@bdk/codegen", () => {
  it("exports its package name", () => {
    expect(packageName).toBe("@bdk/codegen");
  });
});
