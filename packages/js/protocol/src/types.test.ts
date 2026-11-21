import { describe, expectTypeOf, it } from "vitest";
import type {
  BdkError,
  CallerInfo,
  CallOptions,
  ItemOf,
  Page,
  Paginated,
  Progress,
  Subscription,
  Transport,
} from "./index.js";

describe("the protocol types", () => {
  it("shapes a transport", () => {
    expectTypeOf<Transport["invoke"]>().toBeCallableWith("history.search", { text: "a" });
    expectTypeOf<Transport["invoke"]>().toBeCallableWith(
      "runtime.info",
      {},
      { onProgress: () => undefined },
    );
    expectTypeOf<Transport["invoke"]>().returns.toEqualTypeOf<Promise<unknown>>();
    expectTypeOf<Transport["subscribe"]>().returns.toEqualTypeOf<Subscription>();
    expectTypeOf<Parameters<Transport["subscribe"]>[3]>().toEqualTypeOf<
      ((error: BdkError) => void) | undefined
    >();
  });

  it("shapes call options and progress", () => {
    expectTypeOf<CallOptions>().toEqualTypeOf<{
      signal?: AbortSignal;
      onProgress?: (progress: Progress) => void;
    }>();
    expectTypeOf<Progress>().toEqualTypeOf<{
      readonly fraction: number;
      readonly message: string;
    }>();
  });

  it("extracts the item type of a paginated result", () => {
    expectTypeOf<ItemOf<Paginated<{ url: string }>>>().toEqualTypeOf<{ url: string }>();
    expectTypeOf<Paginated<number>["nextCursor"]>().toEqualTypeOf<string | undefined>();
  });

  it("narrows a caller to a page", () => {
    expectTypeOf<Page["kind"]>().toEqualTypeOf<"page">();
    expectTypeOf<Page>().toExtend<CallerInfo>();
    expectTypeOf<CallerInfo["internalScheme"]>().toEqualTypeOf<string | undefined>();
  });
});
