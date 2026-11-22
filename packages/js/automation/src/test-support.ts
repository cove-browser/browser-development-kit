import { Connection } from "./connection.js";
import type { Transport } from "./transport.js";
import type { ClientFrame, Frame, ServerFrame } from "./wire.js";

/** A transport that records what is sent and lets a test deliver frames. */
export class MockTransport implements Transport {
  readonly sent: ClientFrame[] = [];
  closed = false;
  #frames: ((frame: Frame) => void)[] = [];
  #closes: (() => void)[] = [];

  send(frame: Frame): void {
    this.sent.push(frame as ClientFrame);
  }

  onFrame(listener: (frame: Frame) => void): void {
    this.#frames.push(listener);
  }

  onClose(listener: () => void): void {
    this.#closes.push(listener);
  }

  close(): void {
    this.closed = true;
    for (const listener of this.#closes) listener();
  }

  /** Delivers a frame from BDK. */
  deliver(frame: ServerFrame): void {
    for (const listener of this.#frames) listener(frame);
  }

  /** The `call` frames sent so far. */
  calls(): Extract<ClientFrame, { type: "call" }>[] {
    return this.sent.filter((f): f is Extract<ClientFrame, { type: "call" }> => f.type === "call");
  }
}

/** A connection over a mock transport whose every call is answered by `answer(command, args)`. */
export function answeringConnection(
  answer: (command: string, args: Record<string, unknown>) => unknown,
  binary?: string,
): { connection: Connection; transport: MockTransport } {
  const transport = new MockTransport();
  const connection = new Connection(transport);
  const send = transport.send.bind(transport);
  transport.send = (frame) => {
    send(frame);
    if (frame.type === "call") {
      const value = answer(frame.command, frame.args);
      queueMicrotask(() =>
        transport.deliver({
          type: "result",
          id: frame.id,
          value,
          ...(binary !== undefined && { binary }),
        }),
      );
    }
  };
  return { connection, transport };
}
