import { describe, expect, it } from "vitest";
import { CliError } from "../exit-codes.js";
import { checkAppEntitlements, DENIED_APP_ENTITLEMENTS } from "./entitlements.js";
import {
  type BundleLayout,
  helperIdentifier,
  helperKind,
  type PlanAction,
  planSigning,
} from "./plan.js";

const V = "/App.app/Contents/Frameworks/F.framework/Versions/1";
const layout: BundleLayout = {
  appPath: "/App.app",
  appBundleId: "com.acme.App",
  frameworkPath: "/App.app/Contents/Frameworks/F.framework",
  frameworkPlist: `${V}/Resources/Info.plist`,
  libraries: [`${V}/Libraries/libvulkan.dylib`, `${V}/Libraries/libaperitif.dylib`],
  executables: [
    `${V}/Helpers/web_app_shortcut_copier`,
    `${V}/Helpers/chrome_crashpad_handler`,
    `${V}/Helpers/app_mode_loader`,
  ],
  helpers: [
    "Helper (Renderer)",
    "Helper (GPU)",
    "Helper",
    "Helper (Alerts)",
    "Helper (Aperitif Renderer)",
    "Helper (Aperitif GPU)",
    "Helper (Aperitif Alerts)",
    "Helper (Aperitif)",
  ].map((n) => `${V}/Helpers/Fake ${n}.app`),
};

type Sign = Extract<PlanAction, { kind: "sign" }>;
const signs = (plan: PlanAction[]): Sign[] => plan.filter((a): a is Sign => a.kind === "sign");
const find = (plan: PlanAction[], name: string): Sign => {
  const hit = signs(plan).find((a) => a.path.endsWith(name));
  if (hit === undefined) throw new Error(`no sign action for ${name}`);
  return hit;
};

describe("helperKind", () => {
  it.each([
    ["Chromium Helper.app", "plain"],
    ["Chromium Helper (Aperitif).app", "plain"],
    ["Chromium Helper (Renderer).app", "renderer"],
    ["Chromium Helper (Aperitif Renderer).app", "renderer"],
    ["Chromium Helper (GPU).app", "gpu"],
    ["Chromium Helper (Aperitif GPU).app", "gpu"],
    ["Chromium Helper (Alerts).app", "alerts"],
    ["Chromium Helper (Aperitif Alerts).app", "alerts"],
    ["Cove Helper (Renderer).app", "renderer"],
  ])("%s is %s", (name, kind) => {
    expect(helperKind(name)).toBe(kind);
  });

  it.each(["Something.app", "Chromium Helper (Mystery).app", "Helper.app"])(
    "does not know %s",
    (name) => {
      expect(helperKind(name)).toBeUndefined();
    },
  );
});

describe("helperIdentifier follows Chromium's suffixes", () => {
  it.each([
    ["plain", "com.acme.App.helper"],
    ["gpu", "com.acme.App.helper"],
    ["renderer", "com.acme.App.helper.renderer"],
    ["alerts", "com.acme.App.framework.AlertNotificationService"],
  ] as const)("%s", (kind, id) => {
    expect(helperIdentifier(kind, "com.acme.App")).toBe(id);
  });
});

describe("planSigning", () => {
  const plan = planSigning({ layout });

  it("rewrites every helper's identifier and the framework's, before any signature", () => {
    const firstSign = plan.findIndex((a) => a.kind === "sign");
    const rewrites = plan.slice(0, firstSign);
    expect(plan.slice(firstSign).every((a) => a.kind === "sign")).toBe(true);
    const byPlist = Object.fromEntries(
      rewrites.flatMap((a) => (a.kind === "set-identifier" ? [[a.plist, a.identifier]] : [])),
    );
    expect(byPlist).toEqual({
      [`${V}/Helpers/Fake Helper.app/Contents/Info.plist`]: "com.acme.App.helper",
      [`${V}/Helpers/Fake Helper (GPU).app/Contents/Info.plist`]: "com.acme.App.helper",
      [`${V}/Helpers/Fake Helper (Aperitif).app/Contents/Info.plist`]: "com.acme.App.helper",
      [`${V}/Helpers/Fake Helper (Aperitif GPU).app/Contents/Info.plist`]: "com.acme.App.helper",
      [`${V}/Helpers/Fake Helper (Renderer).app/Contents/Info.plist`]:
        "com.acme.App.helper.renderer",
      [`${V}/Helpers/Fake Helper (Aperitif Renderer).app/Contents/Info.plist`]:
        "com.acme.App.helper.renderer",
      [`${V}/Helpers/Fake Helper (Alerts).app/Contents/Info.plist`]:
        "com.acme.App.framework.AlertNotificationService",
      [`${V}/Helpers/Fake Helper (Aperitif Alerts).app/Contents/Info.plist`]:
        "com.acme.App.framework.AlertNotificationService",
      [layout.frameworkPlist]: "com.acme.App.framework",
    });
  });

  it("signs inside-out: libraries, loose executables, helpers, the framework, the app", () => {
    expect(signs(plan).map((a) => a.role)).toEqual([
      "library",
      "library",
      "executable",
      "executable",
      "executable",
      ...Array(8).fill("helper"),
      "framework",
      "app",
    ]);
    expect(signs(plan).at(-1)?.path).toBe("/App.app");
    expect(signs(plan).at(-2)?.path).toBe(layout.frameworkPath);
  });

  it("orders each group by name", () => {
    const names = (role: string) =>
      signs(plan)
        .filter((a) => a.role === role)
        .map((a) => a.path.split("/").pop());
    expect(names("library")).toEqual(["libaperitif.dylib", "libvulkan.dylib"]);
    expect(names("executable")).toEqual([
      "app_mode_loader",
      "chrome_crashpad_handler",
      "web_app_shortcut_copier",
    ]);
  });

  it("gives renderer and GPU helpers the JIT entitlement only, without library validation", () => {
    for (const name of [
      "Fake Helper (Renderer).app",
      "Fake Helper (Aperitif Renderer).app",
      "Fake Helper (GPU).app",
      "Fake Helper (Aperitif GPU).app",
    ]) {
      const a = find(plan, name);
      expect(a.entitlements).toEqual({ "com.apple.security.cs.allow-jit": true });
      expect(a.options).toEqual(["restrict", "runtime", "kill"]);
      expect(a.options).not.toContain("library");
    }
  });

  it("gives the other helpers full hardened options and no entitlements", () => {
    for (const name of [
      "Fake Helper.app",
      "Fake Helper (Alerts).app",
      "Fake Helper (Aperitif).app",
    ]) {
      const a = find(plan, name);
      expect(a.entitlements).toBeUndefined();
      expect(a.options).toEqual(["restrict", "library", "runtime", "kill"]);
    }
  });

  it("signs libraries and the framework with no options, and loose executables with full ones", () => {
    expect(find(plan, "libvulkan.dylib").options).toEqual([]);
    expect(find(plan, "F.framework").options).toEqual([]);
    expect(find(plan, "chrome_crashpad_handler").options).toEqual([
      "restrict",
      "library",
      "runtime",
      "kill",
    ]);
  });

  it("sets an explicit identifier only for the shortcut copier", () => {
    const withId = signs(plan).filter((a) => a.identifier !== undefined);
    expect(withId.map((a) => [a.path.split("/").pop(), a.identifier])).toEqual([
      ["web_app_shortcut_copier", "com.acme.App.web_app_shortcut_copier"],
    ]);
  });

  it("signs the app with full options and its own entitlements file", () => {
    const app = signs(planSigning({ layout, appEntitlementsFile: "/e.plist" })).at(-1);
    expect(app).toMatchObject({
      role: "app",
      options: ["restrict", "library", "runtime", "kill"],
      entitlementsFile: "/e.plist",
    });
    expect(signs(plan).at(-1)?.entitlementsFile).toBeUndefined();
  });

  it("gives no signing options to any part of an ad hoc signature, but keeps the JIT entitlement", () => {
    const adHoc = planSigning({ layout, adHoc: true });
    expect(signs(adHoc).every((a) => a.options.length === 0)).toBe(true);
    expect(signs(adHoc).filter((a) => a.entitlements !== undefined)).toHaveLength(4);
    expect(signs(adHoc).map((a) => a.role)).toEqual(signs(plan).map((a) => a.role));
  });

  it("refuses a helper it does not know", () => {
    const bad = { ...layout, helpers: [`${V}/Helpers/Mystery.app`] };
    expect(() => planSigning({ layout: bad })).toThrow(CliError);
  });
});

describe("checkAppEntitlements", () => {
  it("accepts upstream's application entitlements", () => {
    expect(
      checkAppEntitlements({
        "com.apple.security.device.camera": true,
        "com.apple.security.device.audio-input": true,
        "com.apple.security.personal-information.location": true,
      }),
    ).toEqual([]);
  });

  it("accepts no entitlements, and a denied one set to false", () => {
    expect(checkAppEntitlements(undefined)).toEqual([]);
    expect(checkAppEntitlements({ "com.apple.security.app-sandbox": false })).toEqual([]);
  });

  it.each(Object.keys(DENIED_APP_ENTITLEMENTS))("refuses %s", (key) => {
    const [d] = checkAppEntitlements({ [key]: true }, "/e.plist");
    expect(d).toMatchObject({
      code: "sign.entitlement_denied",
      severity: "error",
      file: "/e.plist",
    });
    expect(d?.message).toContain(key);
    expect(d?.fix).toContain(key);
  });

  it("reports every denied entitlement", () => {
    expect(
      checkAppEntitlements({
        "com.apple.security.app-sandbox": true,
        "com.apple.security.cs.disable-library-validation": true,
      }),
    ).toHaveLength(2);
  });
});
