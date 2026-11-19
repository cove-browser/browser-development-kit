import type { Diagnostic } from "../diagnostics.js";

/**
 * Entitlements an application must not carry (configuration.md section 6, architecture.md section 16).
 * Each weakens the hardened runtime that the runtime's helpers are signed under, or contradicts the
 * process model (the App Sandbox would stop the helpers launching their own sandbox).
 */
export const DENIED_APP_ENTITLEMENTS: Readonly<Record<string, string>> = {
  "com.apple.security.app-sandbox":
    "the App Sandbox cannot be combined with the runtime's own sandbox and helper processes",
  "com.apple.security.cs.disable-library-validation":
    "it lets any library load into the process, which defeats library validation",
  "com.apple.security.cs.allow-unsigned-executable-memory":
    "it allows writable executable memory without the JIT entitlement's scope",
  "com.apple.security.cs.disable-executable-page-protection":
    "it turns off executable page protection",
  "com.apple.security.cs.allow-dyld-environment-variables":
    "it lets environment variables inject libraries",
};

/** One error per denied entitlement that is set (to anything but `false`) in the application's file. */
export function checkAppEntitlements(
  entitlements: Record<string, unknown> | undefined,
  file?: string,
): Diagnostic[] {
  if (entitlements === undefined) return [];
  const found: Diagnostic[] = [];
  for (const [key, reason] of Object.entries(DENIED_APP_ENTITLEMENTS)) {
    const value = entitlements[key];
    if (value === undefined || value === false) continue;
    found.push({
      code: "sign.entitlement_denied",
      severity: "error",
      message: `The application entitlement ${key} is not allowed: ${reason}.`,
      ...(file === undefined ? {} : { file }),
      fix: `Remove ${key} from the entitlements file.`,
    });
  }
  return found;
}

/** What the renderer and GPU helpers get, as in upstream's helper-renderer and helper-gpu plists. */
export const JIT_ENTITLEMENTS: Readonly<Record<string, boolean>> = {
  "com.apple.security.cs.allow-jit": true,
};
