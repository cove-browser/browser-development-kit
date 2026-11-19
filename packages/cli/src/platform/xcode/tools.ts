import { execFile } from "node:child_process";
import { promisify } from "node:util";

const execFileAsync = promisify(execFile);

export interface ToolResult {
  stdout: string;
  stderr: string;
}

/** Every `codesign` and `plutil` call goes through one of these, so tests can fake them. */
export type ToolRunner = (command: string, args: readonly string[]) => Promise<ToolResult>;

/** A tool exited non-zero, or could not be started (`code` is then `ENOENT`). */
export class ToolError extends Error {
  override readonly name = "ToolError";
  readonly command: string;
  readonly args: readonly string[];
  readonly stderr: string;
  readonly code: string | number | undefined;

  constructor(
    command: string,
    args: readonly string[],
    stderr: string,
    code: string | number | undefined,
  ) {
    super(`${command} ${args.join(" ")} failed${stderr ? `: ${stderr.trim()}` : ""}`);
    this.command = command;
    this.args = args;
    this.stderr = stderr;
    this.code = code;
  }
}

/** The real runner: runs the tool without a shell and returns its output. */
export const runTool: ToolRunner = async (command, args) => {
  try {
    const { stdout, stderr } = await execFileAsync(command, [...args], {
      maxBuffer: 64 * 1024 * 1024,
    });
    return { stdout, stderr };
  } catch (error) {
    const e = error as { stderr?: string; code?: string | number };
    throw new ToolError(command, args, e.stderr ?? "", e.code);
  }
};
