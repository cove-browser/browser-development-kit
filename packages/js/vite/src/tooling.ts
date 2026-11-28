/** What the `bdk` command line tool uses from this package, without loading the Vite plugin. */

export { type CspIssue, type CspRule, cspIssues, formatCspIssues, isRemoteUrl } from "./csp.js";
export { SUPPORTED_DEPENDENCIES, type VersionRange } from "./supported.js";

/**
 * Set by `bdk build pages` to the name of one page directory. The page is then built alone, with its own
 * directory as the root, so the output can be served as the whole origin of the page.
 */
export const PAGE_ENV = "BDK_PAGE";
