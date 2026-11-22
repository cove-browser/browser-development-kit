import type { CallerInfo, CallOptions } from "@bdk/protocol";
import { invoke } from "./call.js";
import { holdsCapability } from "./capabilities.js";
import { BdkError, SdkErrors } from "./errors.js";
import type { RuntimeInfo } from "./types.js";

/** The page this module runs in (javascript-api.md section 3.1). */
export interface PageContext {
  /** The route name, such as `settings`. */
  readonly route: string;
  /** The capabilities `bdk.toml` grants the page. */
  readonly capabilities: readonly string[];
  /** True for an exact grant or a covering `app.<prefix>.*` pattern. */
  can(capability: string): boolean;
  /** The address of a route as the user sees it, `<scheme>://<route><path>`. */
  url(route: string, path?: string): string;
  /** Versions and build information. Needs `runtime.read`. */
  info(): Promise<RuntimeInfo>;
}

/** How `createPage` makes a call; `invoke` in a page, a fake in a test. */
export type CallFunction = (
  name: string,
  args?: Readonly<Record<string, unknown>>,
  options?: CallOptions,
) => Promise<unknown>;

/** Builds the page from what `runtime.caller` answered. Pure, so a test needs no transport. */
export function createPage(caller: CallerInfo, call: CallFunction = invoke): PageContext {
  const capabilities = Object.freeze([...caller.capabilities]);
  return {
    route: caller.name,
    capabilities,
    can: (capability) => holdsCapability(capabilities, capability),
    url(route, path = "") {
      if (caller.internalScheme === undefined) {
        throw new BdkError({
          ...SdkErrors.invalidResponse,
          message:
            "runtime.caller did not report the internal scheme, so page.url cannot build an address",
        });
      }
      const separator = path === "" || /^[/?#]/.test(path) ? "" : "/";
      return `${caller.internalScheme}://${route}${separator}${path}`;
    },
    info: () => call("runtime.info", {}) as Promise<RuntimeInfo>,
  };
}
