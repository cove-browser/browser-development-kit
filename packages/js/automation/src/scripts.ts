import type { Connection } from "./connection.js";

/** A persistent user script (`scripts.register`). */
export interface UserScript {
  id: string;
  matches: string[];
  excludes?: string[];
  runAt?: "documentStart" | "documentEnd" | "documentIdle";
  world?: string;
  allFrames?: boolean;
  javascript?: string[];
  css?: string[];
}

/** Scripts are kept per profile, so every call names the profile. */
export interface ScriptOptions {
  profile: string;
}

export interface Scripts {
  register(script: UserScript, options: ScriptOptions): Promise<void>;
  unregister(id: string, options: ScriptOptions): Promise<void>;
  list(options: ScriptOptions): Promise<UserScript[]>;
}

/** Builds `scripts` over a connection that is resolved on first use. */
export function createScripts(connection: () => Connection): Scripts {
  return {
    async register(script, options) {
      await connection().call("scripts.register", { script }, options);
    },
    async unregister(id, options) {
      await connection().call("scripts.unregister", { id }, options);
    },
    async list(options) {
      const { value } = await connection().call("scripts.list", {}, options);
      return (value as { scripts: UserScript[] }).scripts;
    },
  };
}
