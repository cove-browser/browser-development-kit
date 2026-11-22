import type { CallOptions, Connection } from "./connection.js";
import type { Services } from "./services.js";

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
 * What a service namespace is typed as: the interface generated from the definitions (`services.ts`,
 * written by `js-codegen:codegen`), with one method per command a session may call.
 */
export type Namespace<N extends NamespaceName = NamespaceName> = Services[N];

/**
 * Builds a call on `path` that also opens a group: `autofill.addresses` is a group whose `list` calls
 * `autofill.addresses.list`.
 */
function member(path: string, connection: () => Connection): unknown {
  return new Proxy(() => undefined, {
    get(_target, key) {
      // `then` must stay undefined so that awaiting a namespace does not treat it as a thenable.
      if (typeof key !== "string" || key === "then") return undefined;
      return member(`${path}.${key}`, connection);
    },
    async apply(
      _target,
      _this,
      [args = {}, options = {}]: [Record<string, unknown>?, CallOptions?],
    ) {
      return (await connection().call(path, args, options)).value;
    },
  });
}

/**
 * Builds a namespace over a connection that is resolved on first use. Every method invokes
 * `<namespace>.<method>` with one arguments object and resolves with the JSON result.
 */
export function namespace<N extends NamespaceName>(
  name: N,
  connection: () => Connection,
): Namespace<N> {
  return new Proxy({} as Namespace<N>, {
    get(_target, key) {
      if (typeof key !== "string" || key === "then") return undefined;
      return member(`${name}.${key}`, connection);
    },
  });
}
