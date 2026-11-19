/**
 * Lint for the configuration assets: the schema must itself be a valid strict draft 2020-12 schema,
 * and every fixture TOML must be well-formed, except those whose expected diagnostic is a syntax
 * error. Run by `cli:lint`.
 */
import { readdirSync, readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { parse } from "smol-toml";
import { assertSchemaValid } from "../src/core/config/schema-check.js";

const root = fileURLToPath(new URL("../../../", import.meta.url));
const fixtures = `${root}tests/config`;
const problems: string[] = [];

try {
  assertSchemaValid(
    JSON.parse(readFileSync(new URL("../assets/bdk.schema.json", import.meta.url), "utf8")),
  );
} catch (error) {
  problems.push(`bdk.schema.json is not a valid schema: ${(error as Error).message}`);
}

function expectsSyntaxError(tomlPath: string): boolean {
  try {
    const sidecar = JSON.parse(readFileSync(tomlPath.replace(/\.toml$/, ".expected.json"), "utf8"));
    return sidecar.diagnostics.some((d: { code: string }) => d.code === "config.syntax");
  } catch {
    return false;
  }
}

function checkToml(path: string, mayBeBroken: boolean): void {
  try {
    parse(readFileSync(path, "utf8"));
    if (mayBeBroken) problems.push(`${path} is valid TOML but its sidecar expects config.syntax`);
  } catch (error) {
    if (!mayBeBroken) problems.push(`${path} is not well-formed TOML: ${(error as Error).message}`);
  }
}

for (const dir of ["valid", "invalid"]) {
  for (const name of readdirSync(`${fixtures}/${dir}`).filter((n) => n.endsWith(".toml"))) {
    const path = `${fixtures}/${dir}/${name}`;
    checkToml(path, dir === "invalid" && expectsSyntaxError(path));
  }
}
for (const name of readdirSync(`${fixtures}/resolve`)) {
  const path = `${fixtures}/resolve/${name}/bdk.toml`;
  try {
    readFileSync(path);
  } catch {
    continue; // a case may deliberately have no bdk.toml
  }
  checkToml(path, false);
}

if (problems.length > 0) {
  console.error(problems.join("\n"));
  process.exit(1);
}
console.log("config assets ok");
