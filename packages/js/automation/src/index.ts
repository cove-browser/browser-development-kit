/**
 * `@bdk/automation`: the API of a script that runs in the BDK automation host. The host provides
 * `globalThis.__bdkHost`; outside it, importing is harmless and the host-bound APIs throw. For an
 * external tool use `@bdk/automation/client`.
 */
import { createApi } from "./api.js";
import { hostConnection } from "./host.js";

export type { CallOptions, SubscribeOptions } from "./connection.js";
export { BDKError } from "./connection.js";
export type { Task } from "./host.js";
export { session, task } from "./host.js";
export type { Namespace } from "./namespaces.js";
export type * from "./page.js";
export { Page } from "./page.js";
export type { UserScript } from "./scripts.js";

const api = createApi(hostConnection);

export const contents = api.contents;
export const scripts = api.scripts;
export const profiles = api.profiles;
export const history = api.history;
export const bookmarks = api.bookmarks;
export const downloads = api.downloads;
export const settings = api.settings;
export const permissions = api.permissions;
export const cookies = api.cookies;
export const privacy = api.privacy;
export const extensions = api.extensions;
export const search = api.search;
export const passwords = api.passwords;
export const autofill = api.autofill;
