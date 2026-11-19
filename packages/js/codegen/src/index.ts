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
export { renderCommandTable, TABLE_PATH } from "./table.js";
export { macroFragment, parsePolicy, validate } from "./validate.js";

export const packageName = "@bdk/codegen";
