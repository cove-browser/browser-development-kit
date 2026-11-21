import type { BdkError as ProtocolBdkError, SdkErrors as ProtocolSdkErrors } from "@bdk/protocol";
import { describe, expectTypeOf, it } from "vitest";
import type { BdkError, SdkErrors } from "./errors.js";

describe("the copies of @bdk/protocol this package keeps for the unbundled page", () => {
  it("BdkError is the same shape as the protocol's, both ways", () => {
    expectTypeOf<BdkError>().toExtend<ProtocolBdkError>();
    expectTypeOf<ProtocolBdkError>().toExtend<BdkError>();
  });

  it("SdkErrors names the same errors", () => {
    expectTypeOf<typeof SdkErrors>().toEqualTypeOf<typeof ProtocolSdkErrors>();
  });
});
