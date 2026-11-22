export {
  APP_DECLARATIONS_PATH,
  APP_ENVIRONMENT_PATH,
  type ApplicationInputs,
  COMMANDS_FILE,
  digestOf,
  generateApplication,
  isStale,
  RESOLVED_FILE,
  readDigest,
} from "./application.js";
export {
  build,
  check,
  generate,
  HEADER_PATH,
  type Inputs,
  type Outputs,
  readInputs,
} from "./generate.js";
export { renderHeader } from "./header.js";
export { formatIssue, InterfaceError, type Issue } from "./issue.js";
export type * from "./model.js";
export { parseRegistry, type Registry, reconcile, serializeRegistry } from "./registry.js";
export { NATIVE_NAMESPACES, renderSwiftServices, SWIFT_PATH } from "./swift.js";
export { renderCommandTable, TABLE_PATH } from "./table.js";
export {
  renderTypescriptModules,
  TYPESCRIPT_DIR,
  TYPESCRIPT_REPORT_DIR,
} from "./typescript.js";
export { macroFragment, parsePolicy, validate } from "./validate.js";

export const packageName = "@bdk/codegen";
