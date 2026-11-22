import type { CallerInfo } from "@bdk/protocol";
import { afterEach, describe, expect, it, vi } from "vitest";
import { setTransport } from "./call.js";
import { createPage } from "./create-page.js";
import { createMockTransport, installMockTransport } from "./testing.js";

const CALLER: CallerInfo = {
  kind: "page",
  name: "settings",
  profile: "default",
  capabilities: ["settings.read", "app.appearance.*", "app.tabs.open"],
  internalScheme: "cove",
};

describe("createPage", () => {
  it("reports the route and the capabilities, which cannot be changed", () => {
    const page = createPage(CALLER);
    expect(page.route).toBe("settings");
    expect(page.capabilities).toEqual(["settings.read", "app.appearance.*", "app.tabs.open"]);
    expect(Object.isFrozen(page.capabilities)).toBe(true);
  });

  it("can(): an exact grant", () => {
    const page = createPage(CALLER);
    expect(page.can("settings.read")).toBe(true);
    expect(page.can("settings.write")).toBe(false);
    expect(page.can("app.tabs.open")).toBe(true);
    expect(page.can("app.tabs.close")).toBe(false);
  });

  it("can(): an app.<prefix>.* pattern covers the names under the prefix, not the prefix", () => {
    const page = createPage(CALLER);
    expect(page.can("app.appearance.setAccent")).toBe(true);
    expect(page.can("app.appearance.theme.set")).toBe(true);
    expect(page.can("app.appearance")).toBe(false);
    expect(page.can("app.appearancex.set")).toBe(false);
  });

  it("can(): no other wildcard exists", () => {
    const page = createPage({ ...CALLER, capabilities: ["history.*", "*", "app.*"] });
    expect(page.can("history.read")).toBe(false);
    expect(page.can("settings.read")).toBe(false);
    expect(page.can("app.tabs.open")).toBe(true);
  });

  it("url(): builds <scheme>://<route> and appends a path", () => {
    const page = createPage(CALLER);
    expect(page.url("history")).toBe("cove://history");
    expect(page.url("settings", "/privacy")).toBe("cove://settings/privacy");
    expect(page.url("settings", "privacy")).toBe("cove://settings/privacy");
    expect(page.url("history", "?q=a")).toBe("cove://history?q=a");
  });

  it("url(): fails clearly when the runtime did not report the scheme", () => {
    const { internalScheme: _, ...without } = CALLER;
    expect(() => createPage(without).url("history")).toThrowError(/internal scheme/);
  });

  it("info(): calls runtime.info", async () => {
    const info = { bdkVersion: "1" };
    const call = vi.fn(async () => info);
    await expect(createPage(CALLER, call).info()).resolves.toBe(info);
    expect(call).toHaveBeenCalledWith("runtime.info", {});
  });
});

describe("the page module", () => {
  afterEach(() => setTransport(undefined));

  it("is made from runtime.caller when it is imported", async () => {
    const mock = createMockTransport().respond("runtime.caller", CALLER);
    installMockTransport(mock);
    const { page } = await import("./page.js");
    expect(mock.calls).toEqual([{ command: "runtime.caller", args: {}, options: undefined }]);
    expect(page.route).toBe("settings");
    expect(page.can("app.appearance.setAccent")).toBe(true);
    expect(page.url("history")).toBe("cove://history");
  });
});
