import { genericPlatform } from "./generic/index.js";
import type { Platform } from "./platform.js";
import { xcodePlatform } from "./xcode/index.js";

/** The only place that chooses a platform module. `src/main.ts` calls it; the core receives the result. */
export function selectPlatform(host: NodeJS.Platform = process.platform): Platform {
  switch (host) {
    case "darwin":
      return xcodePlatform;
    default:
      return genericPlatform;
  }
}
