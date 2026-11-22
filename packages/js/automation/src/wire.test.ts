import { describe, expect, it } from "vitest";
import {
  encodeFrame,
  FrameDecoder,
  MAX_FRAME_BYTES,
  PROTOCOL_VERSION,
  parseFrame,
} from "./wire.js";

const call = { type: "call", id: 1, command: "contents.list", args: {} } as const;

describe("framing", () => {
  it("has the protocol constants of the contract", () => {
    expect(PROTOCOL_VERSION).toBe(1);
    expect(MAX_FRAME_BYTES).toBe(33554432);
  });

  it("round-trips a frame: 4-byte big-endian length, then UTF-8 JSON", () => {
    const frame = { type: "log", level: "info", message: "héllo ✓" } as const;
    const bytes = encodeFrame(frame);
    const length = new DataView(bytes.buffer).getUint32(0, false);
    expect(length).toBe(bytes.byteLength - 4);
    expect(new FrameDecoder().push(bytes)).toEqual([frame]);
  });

  it("decodes several frames from one chunk", () => {
    const a = encodeFrame(call);
    const b = encodeFrame({ type: "cancel", id: 1 });
    const both = new Uint8Array(a.byteLength + b.byteLength);
    both.set(a);
    both.set(b, a.byteLength);
    expect(new FrameDecoder().push(both)).toEqual([call, { type: "cancel", id: 1 }]);
  });

  it("reassembles a frame split at every byte", () => {
    const decoder = new FrameDecoder();
    const bytes = encodeFrame(call);
    const out = [];
    for (const byte of bytes) out.push(...decoder.push(Uint8Array.of(byte)));
    expect(out).toEqual([call]);
  });

  it("keeps the rest of a following frame for the next chunk", () => {
    const decoder = new FrameDecoder();
    const a = encodeFrame(call);
    const b = encodeFrame({ type: "unsubscribe", id: 2 });
    const first = new Uint8Array(a.byteLength + 3);
    first.set(a);
    first.set(b.subarray(0, 3), a.byteLength);
    expect(decoder.push(first)).toEqual([call]);
    expect(decoder.push(b.subarray(3))).toEqual([{ type: "unsubscribe", id: 2 }]);
  });

  it("throws on a frame above the limit, before buffering it", () => {
    const header = new Uint8Array(4);
    new DataView(header.buffer).setUint32(0, MAX_FRAME_BYTES + 1, false);
    expect(() => new FrameDecoder().push(header)).toThrow(RangeError);
  });

  it("accepts a frame of exactly the limit's header value", () => {
    const header = new Uint8Array(4);
    new DataView(header.buffer).setUint32(0, MAX_FRAME_BYTES, false);
    expect(new FrameDecoder().push(header)).toEqual([]);
  });

  it("throws on JSON that is not a frame", () => {
    expect(() => parseFrame("[1]")).toThrow(TypeError);
    expect(() => parseFrame('{"id":1}')).toThrow(TypeError);
    expect(() => parseFrame("nope")).toThrow(SyntaxError);
  });
});
