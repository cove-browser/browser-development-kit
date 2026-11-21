import type { CallOptions } from "@bdk/protocol";
import { invoke } from "./call.js";
import { BdkError, SdkErrors } from "./errors.js";

/**
 * The application's commands: name to `{ input, output }`. `bdk build codegen` fills it by declaration
 * merging (`@bdk/internal/app.d.ts`); with nothing declared, any `app.` name is accepted.
 */
// biome-ignore lint/suspicious/noEmptyInterface: filled by declaration merging
export interface AppCommands {}

export type AppCommandName = keyof AppCommands extends never
  ? `app.${string}`
  : keyof AppCommands & string;

export type AppCommandInput<N extends AppCommandName> = N extends keyof AppCommands
  ? AppCommands[N] extends { input: infer I }
    ? I
    : never
  : Readonly<Record<string, unknown>>;

export type AppCommandOutput<N extends AppCommandName> = N extends keyof AppCommands
  ? AppCommands[N] extends { output: infer O }
    ? O
    : never
  : unknown;

export const commands = {
  /** Runs an application command. Needs the capability named like it, or a covering `app.<prefix>.*`. */
  execute<N extends AppCommandName>(
    name: N,
    input: AppCommandInput<N>,
    options?: CallOptions,
  ): Promise<AppCommandOutput<N>> {
    if (!name.startsWith("app.")) {
      return Promise.reject(
        new BdkError({
          ...SdkErrors.invalidArguments,
          message: `${name} is not an application command; its name starts with "app."`,
        }),
      );
    }
    return invoke<AppCommandOutput<N>>(name, input as Readonly<Record<string, unknown>>, options);
  },
};
