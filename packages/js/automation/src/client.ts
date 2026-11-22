import { createConnection } from "node:net";
import { type Api, createApi } from "./api.js";
import { BDKError, Connection } from "./connection.js";
import { Session } from "./session.js";
import type { Transport } from "./transport.js";
import { encodeFrame, type Frame, FrameDecoder, PROTOCOL_VERSION } from "./wire.js";

export { BDKError } from "./connection.js";

/** Options of `connect`. */
export interface ConnectOptions {
  /** The external endpoint's Unix socket. */
  socketPath: string;
  /** The token the application recognises. */
  token: string;
  /** Free-form description of this client, passed to the application with the hello. */
  client?: Record<string, unknown>;
}

/** A connected client: the session BDK bound it to and the same API a script has. */
export type Client = Api & {
  readonly session: Session;
  close(): void;
};

/** The framed socket transport (Node). */
export function socketTransport(socketPath: string): Transport {
  const socket = createConnection(socketPath);
  const decoder = new FrameDecoder();
  const frameListeners: ((frame: Frame) => void)[] = [];
  const closeListeners: (() => void)[] = [];
  let closed = false;
  const finish = () => {
    if (closed) return;
    closed = true;
    for (const listener of closeListeners) listener();
  };
  socket.on("data", (chunk: Buffer) => {
    let frames: Frame[];
    try {
      frames = decoder.push(chunk);
    } catch {
      socket.destroy();
      return;
    }
    for (const frame of frames) for (const listener of frameListeners) listener(frame);
  });
  socket.on("error", finish);
  socket.on("close", finish);
  return {
    send: (frame) => {
      if (closed) throw new Error("the socket is closed");
      socket.write(encodeFrame(frame));
    },
    onFrame: (listener) => void frameListeners.push(listener),
    onClose: (listener) => void closeListeners.push(listener),
    close: () => void socket.destroy(),
  };
}

/** Connects to the external endpoint, presents the token and resolves once BDK accepts it. */
export async function connect(options: ConnectOptions): Promise<Client> {
  return connectOver(socketTransport(options.socketPath), options);
}

/** `connect` over any transport; the seam that tests use. */
export function connectOver(
  transport: Transport,
  options: Pick<ConnectOptions, "token" | "client">,
): Promise<Client> {
  const connection = new Connection(transport);
  return new Promise<Client>((resolve, reject) => {
    transport.onFrame((frame) => {
      if (frame.type === "welcome") {
        resolve({
          ...createApi(() => connection),
          session: new Session(frame.session),
          close: () => connection.close(),
        });
      } else if (frame.type === "closed") {
        reject(new BDKError(frame.error));
      }
    });
    transport.onClose(() =>
      reject(
        new BDKError({
          domain: "broker",
          code: "closed",
          message: "the connection closed before BDK answered the hello",
        }),
      ),
    );
    try {
      transport.send({
        type: "hello",
        protocol: PROTOCOL_VERSION,
        token: options.token,
        ...(options.client !== undefined && { client: options.client }),
      });
    } catch (error) {
      reject(error);
    }
  });
}
