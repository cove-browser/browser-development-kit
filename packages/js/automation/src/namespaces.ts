import type { CallOptions, Connection } from "./connection.js";

/** The service namespaces a session can reach. */
export const NAMESPACES = [
  "profiles",
  "history",
  "bookmarks",
  "downloads",
  "settings",
  "permissions",
  "cookies",
  "privacy",
  "extensions",
  "search",
  "passwords",
  "autofill",
] as const;

export type NamespaceName = (typeof NAMESPACES)[number];

/**
 * A service namespace: every method invokes `<namespace>.<method>` with one arguments object and
 * resolves with the JSON result. Loosely typed on purpose: typed declarations come later from code
 * generation (task T-JS-02).
 */
export interface Namespace {
  [method: string]: (args?: Record<string, unknown>, options?: CallOptions) => Promise<unknown>;
}

/** Builds a namespace over a connection that is resolved on first use. */
export function namespace(name: string, connection: () => Connection): Namespace {
  return new Proxy({} as Namespace, {
    get(_target, method) {
      // `then` must stay undefined so that awaiting a namespace does not treat it as a thenable.
      if (typeof method !== "string" || method === "then") return undefined;
      return async (args: Record<string, unknown> = {}, options: CallOptions = {}) =>
        (await connection().call(`${name}.${method}`, args, options)).value;
    },
  });
}
