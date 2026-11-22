import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { isMain, repoRootFromHere, takeValue } from "../../../tooling/src/runtime/cli.js";
import { validateMatrix } from "./matrix.js";
import { loadMatrix } from "./model.js";
import { coverage, renderExpectedTests, renderMarkdown } from "./render.js";
import { parseResults, type Results } from "./results.js";

const USAGE = `usage: cli.ts <command>
  generate [--results <file>] [--out <file>]   write the matrix (results default: $BDK_EXT_RESULTS, else none)
  check                                        fail when the committed files are stale (no results)
  coverage <engine-list-file>                  compare the expected test names with an engine --list output`;

export const MATRIX_JSON = "tests/extensions/matrix.json";
export const MATRIX_MARKDOWN = "docs/extensions/compatibility-matrix.md";
export const EXPECTED_TESTS = "tests/extensions/expected-engine-tests.txt";

/** The generated files for a matrix and optional results, by workspace-relative path. */
export function generatedFiles(root: string, results: Results | null): Map<string, string> {
  const matrix = loadMatrix(resolve(root, MATRIX_JSON));
  const issues = validateMatrix(matrix);
  if (issues.length > 0) throw new Error(`${MATRIX_JSON} is invalid:\n- ${issues.join("\n- ")}`);
  return new Map([
    [MATRIX_MARKDOWN, renderMarkdown(matrix, results)],
    [EXPECTED_TESTS, renderExpectedTests(matrix)],
  ]);
}

function readResults(path: string | undefined): Results | null {
  if (path === undefined || path === "") return null;
  if (!existsSync(path)) throw new Error(`The results file ${path} does not exist.`);
  return parseResults(readFileSync(path, "utf8"));
}

function generate(root: string, argv: string[]): number {
  let resultsPath = process.env.BDK_EXT_RESULTS;
  let out: string | undefined;
  for (let i = 0; i < argv.length; i += 2) {
    const flag = argv[i];
    if (flag === "--results") resultsPath = takeValue(argv, i, flag);
    else if (flag === "--out") out = takeValue(argv, i, flag);
    else throw new Error(`Unknown option ${flag}.`);
  }
  const results = readResults(resultsPath);
  for (const [path, text] of generatedFiles(root, results)) {
    const target =
      path === MATRIX_MARKDOWN && out !== undefined ? resolve(out) : resolve(root, path);
    mkdirSync(dirname(target), { recursive: true });
    writeFileSync(target, text);
    console.log(`wrote ${target}`);
  }
  console.log(results === null ? "state: not run (no results file)" : "state: results applied");
  return 0;
}

export function check(root: string): number {
  let stale = 0;
  for (const [path, text] of generatedFiles(root, null)) {
    const file = resolve(root, path);
    if (!existsSync(file) || readFileSync(file, "utf8") !== text) {
      console.error(`${path} is out of date`);
      stale++;
    }
  }
  if (stale > 0)
    console.error(
      "Regenerate with: pnpm exec nx run extensions-suite:matrix (with no results file)",
    );
  return stale === 0 ? 0 : 1;
}

function coverageReport(root: string, argv: string[]): number {
  const [file] = argv;
  if (file === undefined)
    throw new Error("coverage needs the file holding the engine's --list output.");
  const { missing, unlisted } = coverage(
    loadMatrix(resolve(root, MATRIX_JSON)),
    readFileSync(file, "utf8"),
  );
  for (const name of missing) console.error(`missing: ${name}`);
  for (const name of unlisted) console.error(`not in the matrix: ${name}`);
  console.log(
    `${missing.length} expected tests missing, ${unlisted.length} engine_ext tests not in the matrix`,
  );
  return missing.length === 0 ? 0 : 1;
}

export function main(argv: string[], root: string): number {
  const [command, ...rest] = argv;
  try {
    if (command === "generate") return generate(root, rest);
    if (command === "check") return check(root);
    if (command === "coverage") return coverageReport(root, rest);
  } catch (error) {
    console.error(error instanceof Error ? error.message : String(error));
    return 1;
  }
  console.error(USAGE);
  return 2;
}

if (isMain(import.meta.url))
  process.exitCode = main(process.argv.slice(2), repoRootFromHere(import.meta.url));
