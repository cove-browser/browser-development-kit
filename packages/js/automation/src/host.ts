import { Connection } from "./connection.js";
import { Session } from "./session.js";
import type { Transport } from "./transport.js";
import { type Frame, type LogLevel, parseFrame } from "./wire.js";

/** The object the V8 automation host installs as `globalThis.__bdkHost`. */
export interface BdkHost {
  /** Sends one frame (JSON text) to BDK. */
  send(frame: string): void;
  /** Set by this library: receives each frame (JSON text) from BDK. */
  onmessage: ((frame: string) => void) | null;
}

/** The running task: progress, logging and cancellation. */
export interface Task {
  /** The task's id as assigned by BDK. */
  readonly id: string;
  /** The script's name. */
  readonly script: string;
  /** Aborted when BDK cancels the task or revokes its session. */
  readonly signal: AbortSignal;
  progress(fraction: number, message: string): void;
  log(message: string, level?: LogLevel): void;
}

const NOT_IN_HOST =
  "this API needs the BDK automation host (globalThis.__bdkHost is absent); use connect() from @bdk/automation/client outside the host";
const NOT_STARTED = "the BDK host has not delivered the start frame yet";

/** The in-host transport: frames are JSON strings over `__bdkHost`. */
export class HostTransport implements Transport {
  readonly #host: BdkHost;
  readonly #frameListeners: ((frame: Frame) => void)[] = [];
  readonly #closeListeners: (() => void)[] = [];

  constructor(host: BdkHost, intercept: (frame: Frame) => boolean) {
    this.#host = host;
    host.onmessage = (text) => {
      const frame = parseFrame(text);
      if (intercept(frame)) return;
      for (const listener of this.#frameListeners) listener(frame);
    };
  }

  send(frame: Frame): void {
    this.#host.send(JSON.stringify(frame));
  }

  onFrame(listener: (frame: Frame) => void): void {
    this.#frameListeners.push(listener);
  }

  onClose(listener: () => void): void {
    this.#closeListeners.push(listener);
  }

  close(): void {
    this.#host.onmessage = null;
    for (const listener of this.#closeListeners) listener();
  }
}

/** The library's state inside the host: the `start` frame's session and task, and the connection. */
export class HostRuntime {
  readonly #abort = new AbortController();
  readonly #transport: HostTransport;
  #connection: Connection | undefined;
  #start: StartFrame | undefined;
  #session: Session | undefined;

  constructor(host: BdkHost) {
    this.#transport = new HostTransport(host, (frame) => this.#intercept(frame));
  }

  /** The connection to BDK. */
  get connection(): Connection {
    this.#connection ??= new Connection(this.#transport);
    return this.#connection;
  }

  /** The session the task runs as. */
  get session(): Session {
    this.#session ??= new Session(this.#started().session);
    return this.#session;
  }

  /** The `arguments` of the `start` frame. */
  get arguments(): unknown {
    return this.#started().arguments;
  }

  /** The running task. */
  get task(): Task {
    const runtime = this;
    return {
      get id() {
        return runtime.#started().task;
      },
      get script() {
        return runtime.#started().script;
      },
      signal: this.#abort.signal,
      progress: (fraction, message) =>
        this.#transport.send({ type: "progress", fraction, message }),
      log: (message, level = "info") => this.#transport.send({ type: "log", level, message }),
    };
  }

  #started(): StartFrame {
    if (!this.#start) throw new Error(NOT_STARTED);
    return this.#start;
  }

  #intercept(frame: Frame): boolean {
    if (frame.type === "start") {
      this.#start = frame;
      return true;
    }
    if (frame.type === "abort") {
      this.#abort.abort(Object.assign(new Error(frame.error.message), frame.error));
      return true;
    }
    return false;
  }
}

type StartFrame = Extract<Frame, { type: "start" }>;

const host = (globalThis as { __bdkHost?: BdkHost }).__bdkHost;

/**
 * The runtime bound to `globalThis.__bdkHost`, created at import time so that no frame is missed,
 * or undefined outside the host.
 */
export const hostRuntime: HostRuntime | undefined = host ? new HostRuntime(host) : undefined;

/** The runtime of the host, or an error that says what to use instead outside it. */
export function requireHost(): HostRuntime {
  if (!hostRuntime) throw new Error(NOT_IN_HOST);
  return hostRuntime;
}

/** The connection of the host; throws outside the host. */
export function hostConnection(): Connection {
  return requireHost().connection;
}

/** The session of the running task; throws outside the host. */
export const session: Pick<Session, "name" | "capabilities" | "can"> = {
  get name() {
    return requireHost().session.name;
  },
  get capabilities() {
    return requireHost().session.capabilities;
  },
  can: (capability) => requireHost().session.can(capability),
};

/** The running task; throws outside the host. */
export const task: Task = {
  get id() {
    return requireHost().task.id;
  },
  get script() {
    return requireHost().task.script;
  },
  get signal() {
    return requireHost().task.signal;
  },
  progress: (fraction, message) => requireHost().task.progress(fraction, message),
  log: (message, level) => requireHost().task.log(message, level),
};
