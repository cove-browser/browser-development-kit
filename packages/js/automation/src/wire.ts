/**
 * The automation wire protocol, version 1: JSON frames that travel between the runtime core and a
 * script in the automation host (one string per frame) and over the external Unix socket (each
 * frame is a 4-byte big-endian length, then UTF-8 JSON).
 */

/** The protocol version carried by `hello`, `welcome` and `start`. */
export const PROTOCOL_VERSION = 1;

/** The largest frame, in bytes of JSON. A bigger frame closes the connection. */
export const MAX_FRAME_BYTES = 33554432;

/** An error as it crosses the wire. */
export interface WireError {
  domain: string;
  code: string;
  message: string;
}

/** The session a connection or task runs as. */
export interface SessionInfo {
  name: string;
  capabilities: string[];
}

export type LogLevel = "debug" | "info" | "warn" | "error";

/** Frames a client (a script or an external tool) sends to BDK. */
export type ClientFrame =
  | { type: "hello"; protocol: number; token: string; client?: Record<string, unknown> }
  | {
      type: "call";
      id: number;
      command: string;
      args: Record<string, unknown>;
      profile?: string;
      contents?: string;
    }
  | { type: "cancel"; id: number }
  | { type: "subscribe"; id: number; event: string; profile?: string; contents?: string }
  | { type: "unsubscribe"; id: number }
  | { type: "progress"; fraction: number; message: string }
  | { type: "log"; level: LogLevel; message: string }
  | { type: "done"; result: unknown }
  | { type: "failed"; error: { message: string; stack?: string } };

/** Frames BDK sends to a client. */
export type ServerFrame =
  | { type: "welcome"; protocol: number; session: SessionInfo }
  | {
      type: "start";
      protocol: number;
      task: string;
      script: string;
      arguments: unknown;
      session: SessionInfo;
    }
  | { type: "result"; id: number; value: unknown; binary?: string }
  | { type: "error"; id: number; error: WireError }
  | { type: "progress"; id: number; fraction: number; message: string }
  | { type: "event"; id: number; name: string; payload: unknown }
  | { type: "abort"; error: WireError }
  | { type: "closed"; error: WireError };

export type Frame = ClientFrame | ServerFrame;

const LENGTH_BYTES = 4;

/** Encodes a frame for the socket: 4-byte big-endian length, then UTF-8 JSON. */
export function encodeFrame(frame: Frame): Uint8Array {
  const body = new TextEncoder().encode(JSON.stringify(frame));
  if (body.byteLength > MAX_FRAME_BYTES) {
    throw new RangeError(`frame of ${body.byteLength} bytes exceeds ${MAX_FRAME_BYTES}`);
  }
  const out = new Uint8Array(LENGTH_BYTES + body.byteLength);
  new DataView(out.buffer).setUint32(0, body.byteLength, false);
  out.set(body, LENGTH_BYTES);
  return out;
}

/** Parses one frame's JSON text, checking that it is an object with a string `type`. */
export function parseFrame(text: string): Frame {
  const value: unknown = JSON.parse(text);
  if (
    typeof value !== "object" ||
    value === null ||
    typeof (value as { type?: unknown }).type !== "string"
  ) {
    throw new TypeError("frame is not an object with a string type");
  }
  return value as Frame;
}

/** Splits a byte stream into frames. Push chunks as they arrive; whole frames come back. */
export class FrameDecoder {
  #chunks: Uint8Array[] = [];
  #buffered = 0;
  #expected: number | undefined;

  /** Adds a chunk and returns every frame it completed. Throws on an oversize or malformed frame. */
  push(chunk: Uint8Array): Frame[] {
    this.#chunks.push(chunk);
    this.#buffered += chunk.byteLength;
    const frames: Frame[] = [];
    for (;;) {
      if (this.#expected === undefined) {
        if (this.#buffered < LENGTH_BYTES) break;
        const header = this.#take(LENGTH_BYTES);
        this.#expected = new DataView(header.buffer, header.byteOffset).getUint32(0, false);
        if (this.#expected > MAX_FRAME_BYTES) {
          throw new RangeError(`frame of ${this.#expected} bytes exceeds ${MAX_FRAME_BYTES}`);
        }
      }
      if (this.#buffered < this.#expected) break;
      const body = this.#take(this.#expected);
      this.#expected = undefined;
      frames.push(parseFrame(new TextDecoder().decode(body)));
    }
    return frames;
  }

  #take(count: number): Uint8Array {
    const out = new Uint8Array(count);
    let filled = 0;
    while (filled < count) {
      const head = this.#chunks[0] as Uint8Array;
      const used = Math.min(head.byteLength, count - filled);
      out.set(head.subarray(0, used), filled);
      filled += used;
      if (used === head.byteLength) this.#chunks.shift();
      else this.#chunks[0] = head.subarray(used);
    }
    this.#buffered -= count;
    return out;
  }
}
