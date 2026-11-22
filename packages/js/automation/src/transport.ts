import type { Frame } from "./wire.js";

/** A bidirectional frame channel: the host's message port or an external socket. */
export interface Transport {
  /** Sends one frame. Throws if the channel is closed. */
  send(frame: Frame): void;
  /** Registers a listener for every frame received. */
  onFrame(listener: (frame: Frame) => void): void;
  /** Registers a listener called once when the channel ends. */
  onClose(listener: () => void): void;
  /** Closes the channel. */
  close(): void;
}
