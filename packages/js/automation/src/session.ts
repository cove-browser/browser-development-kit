import type { SessionInfo } from "./wire.js";

/** The session a script or an external connection runs as. */
export class Session {
  readonly name: string;
  readonly capabilities: readonly string[];

  constructor(info: SessionInfo) {
    this.name = info.name;
    this.capabilities = [...info.capabilities];
  }

  /**
   * Whether the session was granted a capability. A grant `app.<prefix>.*` covers every name under
   * `app.<prefix>.`; there is no other wildcard.
   */
  can(capability: string): boolean {
    return this.capabilities.some((granted) =>
      granted.endsWith(".*")
        ? capability.startsWith(granted.slice(0, -1)) && granted.startsWith("app.")
        : granted === capability,
    );
  }
}
