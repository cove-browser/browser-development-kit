/** Lexical checks on the paths bdk.toml holds (configuration.md section 2, "Paths"). */

/**
 * Why a configured path is not allowed, or `undefined` when it is relative, uses forward slashes
 * and stays inside the project directory.
 */
export function pathProblem(value: string): string | undefined {
  if (value.includes("\\")) return "Use forward slashes.";
  if (value.startsWith("/") || value.startsWith("~") || /^[A-Za-z]:/.test(value)) {
    return "It is absolute; paths are relative to the directory of bdk.toml.";
  }
  let depth = 0;
  for (const segment of value.split("/")) {
    if (segment === "" || segment === ".") continue;
    depth += segment === ".." ? -1 : 1;
    if (depth < 0) return "It leaves the directory of bdk.toml.";
  }
  return undefined;
}

/** A project-relative path normalised to forward slashes without `.` segments or a trailing slash. */
export function normaliseProjectPath(value: string): string {
  const stack: string[] = [];
  for (const segment of value.split("/")) {
    if (segment === "" || segment === ".") continue;
    if (segment === "..") stack.pop();
    else stack.push(segment);
  }
  return stack.join("/");
}
