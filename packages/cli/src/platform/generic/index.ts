import type { Platform } from "../platform.js";

/** Used on hosts that have no platform module yet. It has no IDE build and no bundle facts. */
export const genericPlatform: Platform = {
  name: "generic",
  isIdeBuild: () => false,
  bundleFacts: () => Promise.resolve(undefined),
};
