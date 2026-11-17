import { abiVersion, commandNames, type Commands } from "./generated/commands.js";
export type CommandName = keyof Commands;
export function describe(): string {
  return `abi=${abiVersion} commands=${commandNames.join(",")}`;
}
