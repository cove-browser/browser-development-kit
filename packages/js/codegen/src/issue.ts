/** One problem found in an interface file. The formatted text is stable: tests and CI logs rely on it. */
export interface Issue {
  /** File name relative to the interface directory, for example `history.json`. */
  readonly file: string;
  /** JSON path inside the file, for example `$.commands[0].name`. */
  readonly path: string;
  readonly message: string;
}

export function formatIssue(issue: Issue): string {
  return `${issue.file}: ${issue.path}: ${issue.message}`;
}

/** Thrown by the loader and the generator when the interface is not valid. */
export class InterfaceError extends Error {
  readonly issues: readonly Issue[];

  constructor(issues: readonly Issue[]) {
    super(issues.map(formatIssue).join("\n"));
    this.name = "InterfaceError";
    this.issues = issues;
  }
}
