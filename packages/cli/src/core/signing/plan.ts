import { basename } from "node:path";
import { CliError, ExitCode } from "../exit-codes.js";
import { JIT_ENTITLEMENTS } from "./entitlements.js";

/**
 * The signing plan, from chrome/installer/mac/signing/parts.py at the Chromium pin. Planning is pure:
 * it takes a description of the bundle and returns the ordered actions, so it is tested without
 * `codesign` and without a runtime.
 */

/** `codesign --options` values, in the order Chromium writes them. */
export type SigningOption = "restrict" | "library" | "runtime" | "kill";

/** `FULL_HARDENED_RUNTIME_OPTIONS` in upstream's model.py. */
export const FULL_OPTIONS: readonly SigningOption[] = ["restrict", "library", "runtime", "kill"];
/** Used where library validation is incompatible with the JIT entitlement. */
export const JIT_OPTIONS: readonly SigningOption[] = ["restrict", "runtime", "kill"];

export type SignRole = "library" | "executable" | "helper" | "framework" | "app";

export type PlanAction =
  | { kind: "set-identifier"; plist: string; identifier: string }
  | {
      kind: "sign";
      role: SignRole;
      path: string;
      options: readonly SigningOption[];
      /** Entitlements written to a file for this part. */
      entitlements?: Readonly<Record<string, boolean>>;
      /** The application's own entitlements file; only for the app. */
      entitlementsFile?: string;
      /** `codesign --identifier`; absent where codesign derives it. */
      identifier?: string;
    };

/** What was found in the application bundle. Paths are absolute and, for the runtime, real paths. */
export interface BundleLayout {
  appPath: string;
  /** The application's `CFBundleIdentifier`. */
  appBundleId: string;
  /** The framework bundle directory, for example `…/Frameworks/Chromium Framework.framework`. */
  frameworkPath: string;
  /** `Info.plist` of the framework (`Versions/<v>/Resources/Info.plist`). */
  frameworkPlist: string;
  /** Every `*.dylib` in the framework's `Libraries` directory. */
  libraries: readonly string[];
  /** Every file in `Helpers` that is not an `.app` (crash handler, app mode loader, shortcut copier). */
  executables: readonly string[];
  /** Every `*.app` in `Helpers`. */
  helpers: readonly string[];
}

export interface PlanInput {
  layout: BundleLayout;
  /** The application's entitlements file, passed to `codesign` for the app. */
  appEntitlementsFile?: string;
  /**
   * An ad hoc signature carries no team identifier, and the options below assume one. The hardened
   * runtime implies library validation (a library must come from the same team as the process), and
   * `restrict` makes dyld refuse `@executable_path` expansion; with no team the application fails to
   * start with either. Ad hoc signatures therefore get no options at all, like Xcode's local builds.
   * Signatures with a real identity keep Chromium's options.
   */
  adHoc?: boolean;
}

export type HelperKind = "plain" | "renderer" | "gpu" | "alerts";

const HELPER_NAME = /^.+ Helper(?: \((.+)\))?$/;

/** The kind of a helper application from its name, or undefined when the name is not Chromium's. */
export function helperKind(appName: string): HelperKind | undefined {
  const match = HELPER_NAME.exec(basename(appName, ".app"));
  if (match === null) return undefined;
  const variant = match[1];
  if (variant === undefined || variant === "Aperitif") return "plain";
  if (variant.endsWith("Renderer")) return "renderer";
  if (variant.endsWith("GPU")) return "gpu";
  if (variant.endsWith("Alerts")) return "alerts";
  return undefined;
}

/** `CFBundleIdentifier` for a helper: parts.py's identifiers, rooted at the application's identifier. */
export function helperIdentifier(kind: HelperKind, appBundleId: string): string {
  switch (kind) {
    case "plain":
    case "gpu":
      return `${appBundleId}.helper`;
    case "renderer":
      return `${appBundleId}.helper.renderer`;
    case "alerts":
      return `${appBundleId}.framework.AlertNotificationService`;
  }
}

const byName = (a: string, b: string): number => (basename(a) < basename(b) ? -1 : 1);

/**
 * The actions in order: identifier rewrites, then signatures inside-out: libraries, loose executables,
 * helper applications, the framework, the application.
 */
export function planSigning(input: PlanInput): PlanAction[] {
  const { layout } = input;
  const id = layout.appBundleId;
  const opts = (options: readonly SigningOption[]): readonly SigningOption[] =>
    input.adHoc === true ? [] : options;
  const actions: PlanAction[] = [];

  const helpers = [...layout.helpers].sort(byName).map((path) => {
    const kind = helperKind(path);
    if (kind === undefined) {
      throw new CliError(
        {
          code: "sign.unknown_helper",
          severity: "error",
          message: `The helper application ${basename(path)} is not one the signing step knows.`,
          file: path,
          fix: "Use a runtime built from the same Chromium pin as this bdk, or update the signing step.",
        },
        ExitCode.Environment,
      );
    }
    return { path, kind };
  });

  for (const { path, kind } of helpers) {
    actions.push({
      kind: "set-identifier",
      plist: `${path}/Contents/Info.plist`,
      identifier: helperIdentifier(kind, id),
    });
  }
  actions.push({
    kind: "set-identifier",
    plist: layout.frameworkPlist,
    identifier: `${id}.framework`,
  });

  for (const path of [...layout.libraries].sort(byName)) {
    actions.push({ kind: "sign", role: "library", path, options: [] });
  }
  for (const path of [...layout.executables].sort(byName)) {
    const copier = basename(path) === "web_app_shortcut_copier";
    actions.push({
      kind: "sign",
      role: "executable",
      path,
      options: opts(FULL_OPTIONS),
      ...(copier ? { identifier: `${id}.web_app_shortcut_copier` } : {}),
    });
  }
  for (const { path, kind } of helpers) {
    const jit = kind === "renderer" || kind === "gpu";
    actions.push({
      kind: "sign",
      role: "helper",
      path,
      options: opts(jit ? JIT_OPTIONS : FULL_OPTIONS),
      ...(jit ? { entitlements: JIT_ENTITLEMENTS } : {}),
    });
  }
  actions.push({ kind: "sign", role: "framework", path: layout.frameworkPath, options: [] });
  actions.push({
    kind: "sign",
    role: "app",
    path: layout.appPath,
    options: opts(FULL_OPTIONS),
    ...(input.appEntitlementsFile === undefined
      ? {}
      : { entitlementsFile: input.appEntitlementsFile }),
  });
  return actions;
}
