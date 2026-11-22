import { describe, expect, it } from "vitest";
import { commands } from "./commands.js";
import { events } from "./events.js";
import { contents, history, session, task } from "./index.js";

describe("@bdk/automation outside the host", () => {
  it("imports without throwing", () => {
    expect(contents).toBeDefined();
  });

  it("throws a clear error from each host-bound API", async () => {
    await expect(contents.list()).rejects.toThrow(/automation host/);
    await expect(history.search?.({})).rejects.toThrow(/automation host/);
    await expect(commands.execute("app.x.y")).rejects.toThrow(/automation host/);
    expect(() => events.on("contents.created", () => undefined)).toThrow(/automation host/);
    expect(() => session.name).toThrow(/automation host/);
    expect(() => task.progress(0, "")).toThrow(/automation host/);
  });
});
