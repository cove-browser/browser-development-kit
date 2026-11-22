import type { CallOptions, Connection, EventListener, Subscription } from "./connection.js";

/** Typed declarations come from code generation (task T-JS-02); until then results are loose. */
type Json = unknown;

export interface FrameOptions {
  /** A frame id from `Page.frames()`; the main frame when omitted. */
  frame?: string;
}

export interface ContentsInfo {
  id: string;
  url: string;
  title: string;
  profile: string;
  visible: boolean;
  loading: boolean;
  crashed: boolean;
}

/** The result of `contents.get`. */
export interface PageState extends ContentsInfo {
  progress: number;
  security: "unknown" | "none" | "secure" | "broken";
  audible: boolean;
  muted: boolean;
  focused: boolean;
  zoom: number;
  canGoBack: boolean;
  canGoForward: boolean;
}

export type LoadCondition = "committed" | "finished";

export interface NavigateOptions {
  waitUntil?: LoadCondition;
}

/** Commit of a navigation, the result of `contents.navigate`. */
export interface Commit {
  url: string;
  errorPage: boolean;
}

export interface WaitCondition extends FrameOptions {
  load?: LoadCondition;
  url?: string;
  selector?: string;
  script?: string;
  networkIdle?: boolean;
}

export interface WaitOptions {
  timeoutMs?: number;
  signal?: AbortSignal;
}

export interface EvaluateOptions extends FrameOptions {
  args?: unknown[];
  /** Defaults to the isolated world. */
  world?: "isolated" | "page";
  userGesture?: boolean;
}

export interface Rect {
  x: number;
  y: number;
  width: number;
  height: number;
}

export interface ElementInfo {
  tag: string;
  attributes: Json;
  text: string;
  box?: Rect;
}

export interface Snapshot {
  kind: "dom" | "accessibility" | "text";
  root: Json;
}

export interface FrameInfo {
  id: string;
  parent?: string;
  url: string;
  origin: string;
  name: string;
}

/** A CSS selector, or a point in the page; `frame` picks a frame. */
export type Target =
  | string
  | ({ selector: string } & FrameOptions)
  | ({ x: number; y: number } & FrameOptions);

export interface ClickOptions {
  button?: "left" | "middle" | "right";
  clickCount?: number;
}

export interface ScreenshotOptions {
  area?: "visible" | "fullPage" | "rect";
  rect?: Rect;
  format?: "png" | "jpeg";
  quality?: number;
  /** Write the file instead of returning bytes (the returned array is then empty). */
  path?: string;
}

export interface PdfOptions {
  landscape?: boolean;
  printBackground?: boolean;
  scale?: number;
  paperWidth?: number;
  paperHeight?: number;
  marginTop?: number;
  marginBottom?: number;
  marginLeft?: number;
  marginRight?: number;
  pageRanges?: string;
  path?: string;
}

export type DebugListener = (method: string, params: Json) => void;

/** A DevTools protocol session on one page (`debug.attach`). */
export interface DebugSession {
  readonly id: string;
  send(method: string, params?: Json): Promise<Json>;
  /** Listens to this session's protocol events. */
  on(listener: DebugListener): { unsubscribe(): void };
  detach(): Promise<void>;
}

/** `contents.list`, `get` and `create`, each returning a Page. */
export interface Contents {
  list(options?: { profile?: string }): Promise<Page[]>;
  get(id: string): Promise<Page>;
  create(options?: { url?: string; profile?: string }): Promise<Page>;
}

/** Builds the expression `(fn)(...args)` evaluated for a function. */
export function toExpression(
  fn: string | ((...args: never[]) => unknown),
  args: unknown[],
): string {
  return typeof fn === "string" ? fn : `(${fn})(...${JSON.stringify(args)})`;
}

function targetArgs(target: Target): Record<string, unknown> {
  return typeof target === "string" ? { selector: target } : { ...target };
}

/** One page of the browser, addressed by `contents.*` commands. */
export class Page {
  readonly id: string;
  readonly #connection: () => Connection;

  constructor(id: string, connection: () => Connection) {
    this.id = id;
    this.#connection = connection;
  }

  async #value(
    command: string,
    args: Record<string, unknown> = {},
    options: CallOptions = {},
  ): Promise<unknown> {
    return (await this.#connection().call(command, args, { ...options, contents: this.id })).value;
  }

  async state(): Promise<PageState> {
    return (await this.#value("contents.get")) as PageState;
  }

  async url(): Promise<string> {
    return (await this.state()).url;
  }

  async title(): Promise<string> {
    return (await this.state()).title;
  }

  async navigate(url: string, options: NavigateOptions = {}): Promise<Commit> {
    return (await this.#value("contents.navigate", { url, ...options })) as Commit;
  }

  async reload(options: { bypassCache?: boolean } = {}): Promise<void> {
    await this.#value("contents.reload", { ...options });
  }

  async back(): Promise<void> {
    await this.#value("contents.goBack");
  }

  async forward(): Promise<void> {
    await this.#value("contents.goForward");
  }

  async close(): Promise<void> {
    await this.#value("contents.close");
  }

  async waitFor(condition: WaitCondition, options: WaitOptions = {}): Promise<void> {
    const { signal, timeoutMs } = options;
    await this.#value(
      "contents.waitFor",
      { ...condition, ...(timeoutMs !== undefined && { timeoutMs }) },
      signal ? { signal } : {},
    );
  }

  async evaluate<T = unknown>(
    fn: string | ((...args: never[]) => T),
    options: EvaluateOptions = {},
  ): Promise<T> {
    const { args = [], world, frame, userGesture } = options;
    const result = (await this.#value("contents.evaluate", {
      expression: toExpression(fn, args),
      ...(world === "page" && { world }),
      ...(frame !== undefined && { frame }),
      ...(userGesture !== undefined && { userGesture }),
    })) as { value: T };
    return result.value;
  }

  async query(
    selector: string,
    options: FrameOptions & { limit?: number } = {},
  ): Promise<ElementInfo[]> {
    const result = (await this.#value("contents.query", { selector, ...options })) as {
      elements: ElementInfo[];
    };
    return result.elements;
  }

  async text(options: FrameOptions = {}): Promise<string> {
    return ((await this.#value("contents.text", { ...options })) as { text: string }).text;
  }

  async snapshot(
    kind: "dom" | "accessibility" | "text",
    options: FrameOptions = {},
  ): Promise<Snapshot> {
    return (await this.#value("contents.snapshot", { kind, ...options })) as Snapshot;
  }

  async frames(): Promise<FrameInfo[]> {
    return ((await this.#value("contents.frames")) as { frames: FrameInfo[] }).frames;
  }

  async insertCSS(css: string, options: FrameOptions = {}): Promise<{ remove(): Promise<void> }> {
    const { id } = (await this.#value("contents.insertCSS", { css, ...options })) as { id: string };
    return {
      remove: async () => {
        await this.#value("contents.removeCSS", { id });
      },
    };
  }

  async click(target: Target, options: ClickOptions = {}): Promise<void> {
    await this.#value("contents.click", { ...targetArgs(target), ...options });
  }

  async hover(target: Target): Promise<void> {
    await this.#value("contents.hover", targetArgs(target));
  }

  async type(target: Target, text: string): Promise<void> {
    await this.#value("contents.type", { ...targetArgs(target), text });
  }

  async press(key: string, options: { modifiers?: string[] } = {}): Promise<void> {
    await this.#value("contents.press", { key, ...options });
  }

  async scroll(target: Target | { dx: number; dy: number; frame?: string }): Promise<void> {
    await this.#value(
      "contents.scroll",
      typeof target === "string" ? targetArgs(target) : { ...target },
    );
  }

  async screenshot(options: ScreenshotOptions = {}): Promise<Uint8Array> {
    return this.#bytes("contents.screenshot", { ...options });
  }

  async pdf(
    options: PdfOptions = {},
    callOptions: Pick<CallOptions, "onProgress" | "signal"> = {},
  ): Promise<Uint8Array> {
    return this.#bytes("contents.pdf", { ...options }, callOptions);
  }

  async translate(to: string): Promise<void> {
    await this.#value("translation.translate", { to });
  }

  async revertTranslation(): Promise<void> {
    await this.#value("translation.revert");
  }

  /** Attaches a DevTools protocol session; needs `contents.debug`. */
  async debug(): Promise<DebugSession> {
    const listeners = new Set<DebugListener>();
    let sessionId: string | undefined;
    // Events that arrive before the attach result is processed are held until the id is known.
    let early: { session: string; method: string; params: Json }[] | undefined = [];
    const dispatch = (event: { session: string; method: string; params: Json }) => {
      if (event.session !== sessionId) return;
      for (const listener of listeners) listener(event.method, event.params);
    };
    const onEvent: EventListener = (payload) => {
      const event = payload as { session: string; method: string; params: Json };
      if (early) early.push(event);
      else dispatch(event);
    };
    const subscription: Subscription = this.#connection().subscribe("debug.event", onEvent, {
      contents: this.id,
    });
    try {
      await subscription.ready;
      sessionId = ((await this.#value("debug.attach")) as { session: string }).session;
    } catch (error) {
      subscription.unsubscribe();
      throw error;
    }
    const held = early;
    early = undefined;
    for (const event of held) dispatch(event);
    const id = sessionId;
    return {
      id,
      send: async (method, params) => {
        const args = { session: id, method, ...(params !== undefined && { params }) };
        return ((await this.#value("debug.send", args)) as { result: Json }).result;
      },
      on: (listener) => {
        listeners.add(listener);
        return { unsubscribe: () => void listeners.delete(listener) };
      },
      detach: async () => {
        subscription.unsubscribe();
        listeners.clear();
        await this.#value("debug.detach", { session: id });
      },
    };
  }

  /** Asks the application to show the page; the application decides. */
  async requestAttention(): Promise<void> {
    await this.#value("contents.requestAttention");
  }

  async #bytes(
    command: string,
    args: Record<string, unknown>,
    options: CallOptions = {},
  ): Promise<Uint8Array> {
    const result = await this.#connection().call(command, args, { ...options, contents: this.id });
    return result.binary ?? new Uint8Array();
  }
}

/** Builds `contents` over a connection that is resolved on first use. */
export function createContents(connection: () => Connection): Contents {
  return {
    async list(options = {}) {
      const { value } = await connection().call("contents.list", {}, options);
      return (value as { items: ContentsInfo[] }).items.map((i) => new Page(i.id, connection));
    },
    async get(id) {
      await connection().call("contents.get", {}, { contents: id });
      return new Page(id, connection);
    },
    async create(options = {}) {
      const { url, profile } = options;
      const { value } = await connection().call(
        "contents.create",
        url !== undefined ? { url } : {},
        profile !== undefined ? { profile } : {},
      );
      return new Page((value as ContentsInfo).id, connection);
    },
  };
}
