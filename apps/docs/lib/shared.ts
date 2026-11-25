export const appName = "BDK";
export const docsRoute = "/docs";

/** Where the sources of the plan live; pages link to them for the full reference. */
export const repository = "https://github.com/cove-browser/browser-development-kit";

/** The path the site is published under (next.config.mjs); client code adds it to fetched URLs. */
export const basePath = process.env.NEXT_PUBLIC_BDK_DOCS_BASE_PATH ?? "";
