import { describe, expect, it } from "vitest";
import { packageName } from "./index.js";

describe("@bdk/protocol", () => {
  it("exports its package name", () => {
    expect(packageName).toBe("@bdk/protocol");
  });
});
