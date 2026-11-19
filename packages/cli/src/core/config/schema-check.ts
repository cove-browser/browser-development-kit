/**
 * Validates a parsed bdk.toml against the JSON Schema and turns each validator error into a
 * diagnostic with a stable code and a human message. Raw validator messages never reach users.
 */
import Ajv2020Module, { type ErrorObject, type ValidateFunction } from "ajv/dist/2020.js";
import type { KeyPath } from "./locator.js";
import { dotted, isRecord, type Reporter, typeName } from "./reporter.js";

// Ajv is CommonJS; its constructor is both the module and its `default` export.
const Ajv2020 = Ajv2020Module.default;

/** Surfaces proposed in S3 and not yet accepted: reported as ordinary unknown keys. */
const UNACCEPTED_SURFACES = new Set([
  "client_certificates",
  "autofill_suggestions",
  "exclusive_access",
  "device_choosers",
  "screen_capture",
  "webauthn",
  "external_protocols",
  "page_health",
]);

const SURFACE_MODES = new Set(["default", "custom", "disabled"]);

const compiled = new WeakMap<object, ValidateFunction>();

function compile(schema: object): ValidateFunction {
  let validate = compiled.get(schema);
  if (!validate) {
    const ajv = new Ajv2020({
      allErrors: true,
      verbose: true,
      strict: true,
      allowUnionTypes: true,
      validateFormats: false,
    });
    validate = ajv.compile(schema);
    compiled.set(schema, validate);
  }
  return validate;
}

/** Compiles the schema as a strict draft 2020-12 schema. Throws when the schema itself is invalid. */
export function assertSchemaValid(schema: object): void {
  compile(schema);
}

function pointerToPath(pointer: string): string[] {
  return pointer === ""
    ? []
    : pointer
        .slice(1)
        .split("/")
        .map((part) => part.replace(/~1/g, "/").replace(/~0/g, "~"));
}

function levenshtein(a: string, b: string): number {
  let previous = Array.from({ length: b.length + 1 }, (_, n) => n);
  for (let i = 1; i <= a.length; i++) {
    const row = [i];
    for (let j = 1; j <= b.length; j++) {
      row[j] = Math.min(
        (previous[j] ?? 0) + 1,
        (row[j - 1] ?? 0) + 1,
        (previous[j - 1] ?? 0) + (a[i - 1] === b[j - 1] ? 0 : 1),
      );
    }
    previous = row;
  }
  return previous[b.length] ?? 0;
}

/** The closest of `candidates` to `name`, if it is close enough to be a likely typo. */
export function nearestKey(name: string, candidates: readonly string[]): string | undefined {
  let best: string | undefined;
  let bestDistance = Number.POSITIVE_INFINITY;
  for (const candidate of candidates) {
    const distance = levenshtein(name, candidate);
    if (distance < bestDistance) {
      best = candidate;
      bestDistance = distance;
    }
  }
  return best !== undefined && bestDistance <= Math.max(2, Math.floor(name.length / 3))
    ? best
    : undefined;
}

function describeSchema(schema: unknown): string {
  if (!isRecord(schema)) return "a valid value";
  if ("const" in schema) return JSON.stringify(schema.const);
  if (Array.isArray(schema.enum))
    return `one of ${schema.enum.map((v) => JSON.stringify(v)).join(", ")}`;
  if (Array.isArray(schema.anyOf)) return schema.anyOf.map(describeSchema).join(" or ");
  const type = schema.type;
  const types = Array.isArray(type) ? type.map(String) : typeof type === "string" ? [type] : [];
  const article = (word: string) => (/^[aeiou]/.test(word) ? `an ${word}` : `a ${word}`);
  return types
    .map((type) => {
      const word = type === "object" ? "table" : type;
      const items = isRecord(schema.items) ? schema.items.type : undefined;
      return word === "array" && typeof items === "string"
        ? `an array of ${items}s`
        : article(word);
    })
    .join(" or ");
}

function surfaceKeys(error: ErrorObject): string[] {
  return Object.keys(isRecord(error.parentSchema?.properties) ? error.parentSchema.properties : {});
}

function check(error: ErrorObject, path: string[], reporter: Reporter): void {
  const name = dotted(path);
  const data: unknown = error.data;
  const inSurfaces = path[0] === "ui" && path[1] === "surfaces";
  const inExtensionSurfaces = path[0] === "extensions" && path[1] === "ui";

  switch (error.keyword) {
    case "additionalProperties": {
      const key = String((error.params as { additionalProperty: string }).additionalProperty);
      const keyPath = [...path, key];
      const valid = surfaceKeys(error);
      const near = nearestKey(key, valid);
      const hint = near ? ` Did you mean "${dotted([...path, near])}"?` : "";
      const surface =
        ((inSurfaces && !UNACCEPTED_SURFACES.has(key)) || inExtensionSurfaces) && path.length === 2;
      reporter.error(
        surface ? "ui.surface_unknown" : "config.unknown_key",
        surface
          ? `Unknown surface "${key}" in [${name}].${hint}`
          : `Unknown key "${dotted(keyPath)}".${hint}`,
        keyPath,
        near
          ? `Change it to "${near}".`
          : `Remove it. No key is ignored; valid keys here are ${valid.join(", ") || "none"}.`,
      );
      return;
    }
    case "required": {
      const key = String((error.params as { missingProperty: string }).missingProperty);
      reporter.error(
        "config.required",
        `Missing required key "${dotted([...path, key])}".`,
        path,
        path.length === 0 ? `Add a [${key}] table.` : `Add ${key} = ... to [${name}].`,
      );
      return;
    }
    case "type":
      reporter.error(
        "config.type",
        `"${name}" must be ${describeSchema(error.parentSchema)}, found ${typeName(data)}.`,
        path,
      );
      return;
    case "enum":
    case "const": {
      const allowed = describeSchema(error.parentSchema);
      const found = JSON.stringify(data instanceof Date ? data.toISOString() : data);
      const parent = path.slice(0, -1);
      const last = path[path.length - 1];
      if (path[0] === "services" && last === "provider") {
        reporter.error(
          "services.provider_unknown",
          `"${name}" names a provider BDK does not ship: ${found}. Valid providers are ${allowed.replace("one of ", "")}.`,
          path,
        );
      } else if (parent.join(".") === "extensions.install.stores") {
        reporter.error(
          "extensions.store_unknown",
          `"${dotted(parent)}" names a store BDK does not ship: ${found}. Valid stores are ${allowed.replace("one of ", "")}.`,
          path,
        );
      } else if (
        inSurfaces &&
        path.length === 3 &&
        typeof data === "string" &&
        SURFACE_MODES.has(data)
      ) {
        reporter.error(
          "ui.surface_mode",
          `Surface "${String(last)}" does not allow the mode "${data}". Allowed modes: ${allowed.replace("one of ", "")}.`,
          path,
        );
      } else {
        reporter.error("config.type", `"${name}" must be ${allowed}, found ${found}.`, path);
      }
      return;
    }
    case "anyOf":
      reporter.error(
        "config.type",
        `"${name}" must be ${describeSchema(error.parentSchema)}, found ${typeName(data)}.`,
        path,
      );
      return;
    case "minLength":
      reporter.error("config.type", `"${name}" must not be empty.`, path);
      return;
    case "pattern": {
      const found = JSON.stringify(data);
      if (name === "browser.internal_scheme") {
        reporter.error(
          "browser.scheme_invalid",
          `"browser.internal_scheme" is not a valid URL scheme: ${found}. Use lower-case letters, digits, "+", "-" and ".", starting with a letter.`,
          path,
        );
      } else {
        reporter.error(
          "config.type",
          `"${name}" has an invalid value: ${found}. ${patternHint(path)}`,
          path,
        );
      }
      return;
    }
    default:
      reporter.error("config.type", `"${name}" has an invalid value.`, path);
  }
}

function patternHint(path: KeyPath): string {
  const last = path[path.length - 1];
  if (last === "update_interval") return "Write a duration such as 30m, 1h or 1d.";
  if (last === "endpoint") return "Write an http or https URL.";
  if (path.includes("bundled_languages")) return "Write a language code such as en or pt-BR.";
  return "It does not match the format this key requires.";
}

/** Validates `data` and reports each problem. Returns whether the document is valid. */
export function checkSchema(schema: object, data: unknown, reporter: Reporter): boolean {
  const validate = compile(schema);
  if (validate(data)) return true;
  for (const error of validate.errors ?? []) {
    // The branches of an anyOf are summarised by the anyOf error itself.
    if (/\/anyOf\/\d+\//.test(error.schemaPath)) continue;
    check(error, pointerToPath(error.instancePath), reporter);
  }
  return false;
}
