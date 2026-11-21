/**
 * Whether a list of grants covers a capability: an exact grant, or an `app.<prefix>.*` pattern. The pattern
 * `app.x.*` covers `app.x.y` and `app.x.y.z` but not `app.x`; no other wildcard exists. This is the broker's rule
 * (conventions.md, "The broker").
 */
export function holdsCapability(grants: readonly string[], capability: string): boolean {
  return grants.some((grant) => {
    if (grant === capability) return true;
    return (
      grant.startsWith("app.") && grant.endsWith(".*") && capability.startsWith(grant.slice(0, -1))
    );
  });
}
