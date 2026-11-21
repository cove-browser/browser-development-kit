import type { CallerInfo } from "@bdk/protocol";
import { invoke } from "./call.js";
import { createPage, type PageContext } from "./create-page.js";

export type { PageContext } from "./create-page.js";

/** The page this module runs in, made from `runtime.caller` when the module loads. */
export const page: PageContext = createPage(await invoke<CallerInfo>("runtime.caller"));
