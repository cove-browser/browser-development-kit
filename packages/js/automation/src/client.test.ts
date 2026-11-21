import { mkdtempSync, rmSync } from "node:fs";
import { createServer, type Server, type Socket } from "node:net";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { afterEach, describe, expect, it } from "vitest";
import { connect } from "./client.js";
import { encodeFrame, type Frame, FrameDecoder, type ServerFrame } from "./wire.js";

let server: Server | undefined;
let directory: string | undefined;

afterEach(async () => {
  await new Promise((resolve) => (server ? server.close(resolve) : resolve(undefined)));
  server = undefined;
  if (directory) rmSync(directory, { recursive: true, force: true });
});

/** A fake BDK endpoint: `handle` answers each frame the client sends. */
async function fakeServer(handle: (frame: Frame, reply: (frame: ServerFrame) => void) => void) {
  directory = mkdtempSync(join(tmpdir(), "bdk-client-"));
  const socketPath = join(directory, "s.sock");
  const received: Frame[] = [];
  server = createServer((socket: Socket) => {
    const decoder = new FrameDecoder();
    const reply = (frame: ServerFrame) => socket.write(encodeFrame(frame));
    socket.on("data", (chunk: Buffer) => {
      for (const frame of decoder.push(chunk)) {
        received.push(frame);
        handle(frame, reply);
      }
    });
    socket.on("error", () => undefined);
  });
  await new Promise<void>((resolve) => (server as Server).listen(socketPath, resolve));
  return { socketPath, received };
}

describe("connect", () => {
  it("sends hello first, resolves after welcome and carries out a call", async () => {
    const { socketPath, received } = await fakeServer((frame, reply) => {
      if (frame.type === "hello") {
        reply({
          type: "welcome",
          protocol: 1,
          session: { name: "agent", capabilities: ["contents.read"] },
        });
      } else if (frame.type === "call") {
        reply({ type: "result", id: frame.id, value: { items: [{ id: "5" }] } });
      }
    });
    const bdk = await connect({ socketPath, token: "secret", client: { name: "test" } });
    expect(bdk.session.name).toBe("agent");
    expect(bdk.session.can("contents.read")).toBe(true);
    const pages = await bdk.contents.list();
    expect(pages.map((p) => p.id)).toEqual(["5"]);
    expect(received[0]).toEqual({
      type: "hello",
      protocol: 1,
      token: "secret",
      client: { name: "test" },
    });
    expect(received[1]).toEqual({ type: "call", id: 1, command: "contents.list", args: {} });
    bdk.close();
  });

  it("rejects with a BDKError when the hello is refused", async () => {
    const { socketPath } = await fakeServer((frame, reply) => {
      if (frame.type === "hello") {
        reply({
          type: "closed",
          error: { domain: "automation", code: "refused", message: "bad token" },
        });
      }
    });
    await expect(connect({ socketPath, token: "wrong" })).rejects.toMatchObject({
      name: "BDKError",
      domain: "automation",
      code: "refused",
      message: "bad token",
    });
  });

  it("rejects when nothing listens at the socket path", async () => {
    directory = mkdtempSync(join(tmpdir(), "bdk-client-"));
    await expect(
      connect({ socketPath: join(directory, "none.sock"), token: "t" }),
    ).rejects.toMatchObject({
      code: "closed",
    });
  });
});
