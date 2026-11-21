export type Outcome = "pass" | "fail" | "skip";

export interface Results {
  readonly tests: ReadonlyMap<string, Outcome>;
}

const LINE = /^\[\s*(ok|pass|fail|skip)\s*\]\s+(\S+)\s*$/i;

/**
 * Reads what the engine conformance app prints, one line per test: `[ ok ] name` or `[FAIL] name`
 * (packages/core/tests/test_support.c). Failure details are indented and ignored. `[skip] name` is accepted
 * for a test that declines to run, for example a real extension that is not in the cache. A name that appears
 * twice keeps its worst outcome.
 */
export function parseResults(text: string): Results {
  const rank: Record<Outcome, number> = { pass: 0, skip: 1, fail: 2 };
  const tests = new Map<string, Outcome>();
  for (const line of text.split(/\r?\n/)) {
    const match = LINE.exec(line);
    if (!match) continue;
    const tag = (match[1] ?? "").toLowerCase();
    const outcome: Outcome = tag === "ok" || tag === "pass" ? "pass" : (tag as "fail" | "skip");
    const name = match[2] ?? "";
    const earlier = tests.get(name);
    if (earlier === undefined || rank[outcome] > rank[earlier]) tests.set(name, outcome);
  }
  return { tests };
}
