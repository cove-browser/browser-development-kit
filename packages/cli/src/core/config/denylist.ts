/**
 * The Chromium switch deny list (configuration.md 3.4.1). Each rule protects a security invariant
 * of architecture.md section 16. The list is an initial set, to be kept in step with the
 * Google-removal audit list (T-PRV-07).
 */

interface DenyRule {
  /** Matched against the name with case, hyphens and underscores removed. */
  pattern: RegExp;
  /** What the rule protects, in words for the message. */
  reason: string;
}

const RULES: readonly DenyRule[] = [
  {
    pattern: /^(no|disable|without)\w*(sandbox|siteisolation|processlock)/,
    reason: "it would disable the sandbox, site isolation or process locks (invariant 1)",
  },
  {
    pattern: /^(siteperprocess|isolateorigins|strictoriginisolation)$/,
    reason: "it would change site isolation (invariant 1)",
  },
  {
    pattern: /remotedebugging/,
    reason: "remote debugging on a network port or a pipe is not allowed (invariant 7)",
  },
  {
    pattern:
      /^(disablewebsecurity|ignorecertificateerrors\w*|allowrunninginsecurecontent|disablecors\w*|disablesitecertchecks)$/,
    reason: "it would disable web security, certificate checks or CORS (invariants 1 and 6)",
  },
  {
    pattern: /^(google(apikey|defaultclient\w*|oauth\w*)|useofficialgoogleapikeys)$/,
    reason: "Google API keys do not belong in configuration (invariant 9)",
  },
  {
    pattern:
      /^(enable|use)?(syncservice|sync|signin|metricsreporting|metricsupload|crashreporting|crashupload|variationsfetch|variationsseed|variationsservice)$/,
    reason:
      "it would re-enable sign-in, sync, metrics upload, crash upload or variations fetches (architecture.md section 12)",
  },
  {
    pattern: /^(autogrant|autoaccept|autoselect|autoinstall|usefakeui)\w*/,
    reason: "it would auto-grant permissions, media capture or extension installs (invariant 6)",
  },
];

function squash(name: string): string {
  return name.replace(/^-+/, "").replace(/[-_]/g, "").toLowerCase();
}

/** Why this GN argument, feature or switch is denied, or `undefined` when it is allowed. */
export function denyReason(name: string): string | undefined {
  const squashed = squash(name);
  return RULES.find((rule) => rule.pattern.test(squashed))?.reason;
}
