import { describe, expect, it } from "vitest";
import { type BdkHost, HostRuntime, requireHost } from "./host.js";
import { Session } from "./session.js";
import type { ServerFrame } from "./wire.js";

function fakeHost() {
  const sent: unknown[] = [];
  const host: BdkHost = { send: (text) => void sent.push(JSON.parse(text)), onmessage: null };
  const deliver = (frame: ServerFrame) => host.onmessage?.(JSON.stringify(frame));
  return { host, sent, deliver };
}

const start: ServerFrame = {
  type: "start",
  protocol: 1,
  task: "task-1",
  script: "summarise",
  arguments: { url: "https://a.test" },
  session: { name: "tester", capabilities: ["contents.read", "app.notes.*"] },
};

describe("HostRuntime", () => {
  it("installs onmessage, keeps the start frame's session and task", () => {
    const { host, deliver } = fakeHost();
    const runtime = new HostRuntime(host);
    expect(host.onmessage).toBeTypeOf("function");
    expect(() => runtime.session).toThrow(/start frame/);
    deliver(start);
    expect(runtime.session.name).toBe("tester");
    expect(runtime.task.id).toBe("task-1");
    expect(runtime.task.script).toBe("summarise");
    expect(runtime.arguments).toEqual({ url: "https://a.test" });
  });

  it("sends progress and log frames, and never done or failed", () => {
    const { host, sent, deliver } = fakeHost();
    const runtime = new HostRuntime(host);
    deliver(start);
    runtime.task.progress(0.5, "half");
    runtime.task.log("hello");
    runtime.task.log("careful", "warn");
    expect(sent).toEqual([
      { type: "progress", fraction: 0.5, message: "half" },
      { type: "log", level: "info", message: "hello" },
      { type: "log", level: "warn", message: "careful" },
    ]);
  });

  it("aborts task.signal when an abort frame arrives", () => {
    const { host, deliver } = fakeHost();
    const runtime = new HostRuntime(host);
    deliver(start);
    expect(runtime.task.signal.aborted).toBe(false);
    deliver({
      type: "abort",
      error: { domain: "broker", code: "cancelled", message: "cancelled" },
    });
    expect(runtime.task.signal.aborted).toBe(true);
    expect(runtime.task.signal.reason).toMatchObject({ code: "cancelled" });
  });

  it("answers calls through the host's send and onmessage", async () => {
    const { host, sent, deliver } = fakeHost();
    const runtime = new HostRuntime(host);
    deliver(start);
    const pending = runtime.connection.call("contents.list");
    expect(sent).toEqual([{ type: "call", id: 1, command: "contents.list", args: {} }]);
    deliver({ type: "result", id: 1, value: { items: [] } });
    expect((await pending).value).toEqual({ items: [] });
  });
});

describe("outside the host", () => {
  it("imports without throwing and throws a clear error when a host API is used", () => {
    expect(() => requireHost()).toThrow(/automation host/);
  });
});

describe("Session.can", () => {
  const session = new Session({
    name: "s",
    capabilities: ["contents.read", "app.notes.*", "contents.*"],
  });

  it("matches exact capabilities", () => {
    expect(session.can("contents.read")).toBe(true);
    expect(session.can("contents.control")).toBe(false);
  });

  it("lets app.<prefix>.* cover names under the prefix only", () => {
    expect(session.can("app.notes.create")).toBe(true);
    expect(session.can("app.notes.deep.name")).toBe(true);
    expect(session.can("app.notesx.create")).toBe(false);
    expect(session.can("app.other.create")).toBe(false);
  });

  it("gives no wildcard outside app.", () => {
    expect(session.can("contents.control")).toBe(false);
  });
});
