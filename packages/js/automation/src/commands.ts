import type { CallOptions, Connection } from "./connection.js";
import { hostConnection } from "./host.js";
import type { CommandInput, CommandMap, CommandOutput } from "./services.js";

/**
 * The application's commands: name to `{ input, output }`. `bdk build codegen` fills it by declaration
 * merging (`@bdk/automation/app.d.ts`); with nothing declared, any `app.` name is accepted.
 */
// biome-ignore lint/suspicious/noEmptyInterface: filled by declaration merging
export interface AppCommands {}

export type AppCommandName = keyof AppCommands extends never
  ? `app.${string}`
  : keyof AppCommands & string;

/** Every command `commands.execute` takes: the browser's, which a session may call, and the application's. */
export type CommandName = keyof CommandMap | AppCommandName;

export type ArgumentsOf<N extends CommandName> = N extends keyof CommandMap
  ? CommandInput<N>
  : N extends keyof AppCommands
    ? AppCommands[N] extends { input: infer I }
      ? I
      : never
    : Readonly<Record<string, unknown>>;

export type ResultOf<N extends CommandName> = N extends keyof CommandMap
  ? CommandOutput<N>
  : N extends keyof AppCommands
    ? AppCommands[N] extends { output: infer O }
      ? O
      : never
    : unknown;

/** The arguments may be left out when every one of them is optional. */
type ExecuteArguments<N extends CommandName> =
  Record<string, never> extends ArgumentsOf<N>
    ? [args?: ArgumentsOf<N>, options?: CallOptions]
    : [args: ArgumentsOf<N>, options?: CallOptions];

/** Executes commands, including the application's `app.` commands. */
export interface Commands {
  /** Invokes a command and resolves with its JSON result. */
  execute<N extends CommandName>(name: N, ...rest: ExecuteArguments<N>): Promise<ResultOf<N>>;
}

/** Builds `commands` over a connection that is resolved on first use. */
export function createCommands(connection: () => Connection): Commands {
  return {
    async execute<N extends CommandName>(name: N, ...[args, options = {}]: ExecuteArguments<N>) {
      const result = await connection().call(
        name,
        (args ?? {}) as Record<string, unknown>,
        options,
      );
      return result.value as ResultOf<N>;
    },
  };
}

/** Commands through the automation host. Scripts import it as `@bdk/commands`. */
export const commands: Commands = createCommands(hostConnection);
