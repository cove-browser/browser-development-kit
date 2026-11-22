import { type ChildProcess, spawn } from "node:child_process";
import { existsSync } from "node:fs";
import { createInterface } from "node:readline";
import { fileURLToPath } from "node:url";
import { afterAll, beforeAll, describe, expect, it } from "vitest";
import { BDKError, connect } from "./client.js";

/**
 * The external tool against a real runtime: the stub, with its endpoint open, run by
 * `bdk_stub_automation_server` (packages/core/tests/automation_server.c, built by `core:build`).
 * This is the "external runtime on the local channel" half of the T-AUT-01 spike: Node runs a
 * script that calls commands through the authenticated socket.
 */
const SERVER = fileURLToPath(
  new URL("../../../core/build/bdk_stub_automation_server", import.meta.url),
);

let server: ChildProcess;
let socketPath: string;

beforeAll(async () => {
  expect(existsSync(SERVER), `${SERVER} is built by nx run core:build`).toBe(true);
  server = spawn(
    SERVER,
    ["runtime.read", "contents.read", "contents.control", "contents.capture"],
    {
      stdio: ["pipe", "pipe", "inherit"],
    },
  );
  const lines = createInterface({ input: server.stdout as NodeJS.ReadableStream });
  socketPath = await new Promise<string>((resolve) => lines.once("line", resolve));
});

afterAll(() => {
  server.stdin?.end();
});

describe("@bdk/automation/client against the stub runtime", () => {
  it("is welcomed with the right token and calls commands as its session", async () => {
    const bdk = await connect({ socketPath, token: "good-token", client: { test: true } });
    try {
      expect(bdk.session.name).toBe("external-test");
      expect(bdk.session.can("contents.read")).toBe(true);
      expect(bdk.session.can("history.read")).toBe(false);
      const caller = await bdk.commands.execute("runtime.caller");
      expect(caller).toMatchObject({ kind: "session", name: "external-test" });

      const pages = await bdk.contents.list();
      expect(pages).toHaveLength(1);
      const page = pages[0];
      if (page === undefined) throw new Error("no page");
      const state = await page.state();
      expect(state.url).toBe("https://example.com/");
      const png = await page.screenshot();
      expect(Array.from(png.slice(0, 4))).toEqual([0x89, 0x50, 0x4e, 0x47]);

      await expect(bdk.history?.search?.({})).rejects.toMatchObject({
        domain: "capability",
        code: "denied",
      });
    } finally {
      bdk.close();
    }
  });

  it("is refused with a wrong token", async () => {
    const attempt = connect({ socketPath, token: "wrong" });
    await expect(attempt).rejects.toBeInstanceOf(BDKError);
    await expect(attempt).rejects.toMatchObject({ domain: "capability", code: "denied" });
  });
});
