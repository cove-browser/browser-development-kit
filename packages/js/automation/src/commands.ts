import type { CallOptions, Connection } from "./connection.js";
import { hostConnection } from "./host.js";

/** Executes commands, including the application's `app.` commands. */
export interface Commands {
  /** Invokes a command and resolves with its JSON result. */
  execute(name: string, args?: Record<string, unknown>, options?: CallOptions): Promise<unknown>;
}

/** Builds `commands` over a connection that is resolved on first use. */
export function createCommands(connection: () => Connection): Commands {
  return {
    async execute(name, args = {}, options = {}) {
      return (await connection().call(name, args, options)).value;
    },
  };
}

/** Commands through the automation host. Scripts import it as `@bdk/commands`. */
export const commands: Commands = createCommands(hostConnection);
