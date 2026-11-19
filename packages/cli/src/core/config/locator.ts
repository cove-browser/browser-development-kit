/**
 * Maps a key path in a TOML document to the line and column where the key is written.
 *
 * TOML parsers return values without positions, so diagnostics would have no line. This scanner
 * walks the text once and records the position of every key, table header, dotted-key prefix
 * and array element. It assumes the text is already known to be valid TOML (the parser runs
 * first), so it does not report errors of its own.
 */

export interface Position {
  line: number;
  column: number;
}

export type KeyPath = readonly (string | number)[];

const WHITESPACE = new Set([" ", "\t"]);
const NEWLINE = new Set(["\n", "\r"]);
const VALUE_END = new Set([",", "]", "}", "#", "\n", "\r"]);
const BARE_KEY = /[A-Za-z0-9_-]/;

function pathId(path: KeyPath): string {
  return path.map(String).join("\u0000");
}

export class KeyLocator {
  private readonly positions = new Map<string, Position>();
  private readonly lineStarts: number[] = [0];
  private readonly text: string;
  private i = 0;

  constructor(text: string) {
    this.text = text;
    for (let n = 0; n < text.length; n++) {
      if (text[n] === "\n") this.lineStarts.push(n + 1);
    }
    this.scanDocument();
  }

  /** The position of exactly this path, if the document writes it. */
  find(path: KeyPath): Position | undefined {
    return this.positions.get(pathId(path));
  }

  /** The position of this path, or of its nearest written ancestor, or the first line. */
  findNearest(path: KeyPath): Position {
    for (let n = path.length; n > 0; n--) {
      const found = this.positions.get(pathId(path.slice(0, n)));
      if (found) return found;
    }
    return { line: 1, column: 1 };
  }

  private record(path: KeyPath, index: number): void {
    const id = pathId(path);
    if (this.positions.has(id)) return;
    let low = 0;
    let high = this.lineStarts.length - 1;
    while (low < high) {
      const mid = (low + high + 1) >> 1;
      if ((this.lineStarts[mid] ?? 0) <= index) low = mid;
      else high = mid - 1;
    }
    this.positions.set(id, { line: low + 1, column: index - (this.lineStarts[low] ?? 0) + 1 });
  }

  private peek(offset = 0): string {
    return this.text[this.i + offset] ?? "";
  }

  private skipBlanks(): void {
    while (WHITESPACE.has(this.peek())) this.i++;
  }

  /** Skips whitespace, newlines and comments. */
  private skipFiller(): void {
    for (;;) {
      const ch = this.peek();
      if (WHITESPACE.has(ch) || NEWLINE.has(ch)) this.i++;
      else if (ch === "#") this.skipComment();
      else return;
    }
  }

  private skipComment(): void {
    while (this.i < this.text.length && !NEWLINE.has(this.peek())) this.i++;
  }

  private scanDocument(): void {
    let table: (string | number)[] = [];
    const arrayCounts = new Map<string, number>();
    for (;;) {
      this.skipFiller();
      if (this.i >= this.text.length) return;
      if (this.peek() === "[") {
        const isArrayTable = this.peek(1) === "[";
        this.i += isArrayTable ? 2 : 1;
        const start = this.i;
        const parts = this.readKey();
        this.i += isArrayTable ? 2 : 1;
        table = [...parts];
        if (isArrayTable) {
          const id = pathId(table);
          const count = arrayCounts.get(id) ?? 0;
          arrayCounts.set(id, count + 1);
          table.push(count);
        }
        for (let n = 1; n <= parts.length; n++) this.record(table.slice(0, n), start);
        this.skipBlanks();
        continue;
      }
      const start = this.i;
      const parts = this.readKey();
      for (let n = 1; n <= parts.length; n++) this.record([...table, ...parts.slice(0, n)], start);
      this.skipBlanks();
      if (this.peek() === "=") this.i++;
      this.scanValue([...table, ...parts]);
    }
  }

  /** Reads a possibly dotted key and leaves the cursor on the character after it. */
  private readKey(): string[] {
    const parts: string[] = [];
    for (;;) {
      this.skipBlanks();
      const ch = this.peek();
      if (ch === '"') parts.push(this.readBasicString());
      else if (ch === "'") parts.push(this.readLiteralString());
      else {
        let bare = "";
        while (BARE_KEY.test(this.peek())) bare += this.text[this.i++];
        parts.push(bare);
      }
      this.skipBlanks();
      if (this.peek() === ".") this.i++;
      else return parts;
    }
  }

  private readBasicString(): string {
    this.i++;
    let out = "";
    while (this.i < this.text.length && this.peek() !== '"') {
      if (this.peek() === "\\") {
        const marker = this.peek(1);
        this.i += 2;
        if (marker === "u" || marker === "U") {
          const length = marker === "u" ? 4 : 8;
          const code = Number.parseInt(this.text.slice(this.i, this.i + length), 16);
          this.i += length;
          out += String.fromCodePoint(code);
        } else out += { n: "\n", t: "\t", r: "\r", b: "\b", f: "\f" }[marker] ?? marker;
      } else out += this.text[this.i++];
    }
    this.i++;
    return out;
  }

  private readLiteralString(): string {
    this.i++;
    const end = this.text.indexOf("'", this.i);
    const out = this.text.slice(this.i, end);
    this.i = end + 1;
    return out;
  }

  private scanValue(path: (string | number)[]): void {
    this.skipBlanks();
    const ch = this.peek();
    if (ch === '"' && this.text.startsWith('"""', this.i)) this.skipMultiline('"""', true);
    else if (ch === "'" && this.text.startsWith("'''", this.i)) this.skipMultiline("'''", false);
    else if (ch === '"') this.readBasicString();
    else if (ch === "'") this.readLiteralString();
    else if (ch === "[") this.scanArray(path);
    else if (ch === "{") this.scanInlineTable(path);
    else while (this.i < this.text.length && !VALUE_END.has(this.peek())) this.i++;
  }

  private skipMultiline(delimiter: string, escapes: boolean): void {
    this.i += 3;
    while (this.i < this.text.length && !this.text.startsWith(delimiter, this.i)) {
      this.i += escapes && this.peek() === "\\" ? 2 : 1;
    }
    this.i += 3;
    while (this.peek() === delimiter[0]) this.i++;
  }

  private scanArray(path: (string | number)[]): void {
    this.i++;
    let index = 0;
    for (;;) {
      this.skipFiller();
      if (this.i >= this.text.length) return;
      if (this.peek() === "]") {
        this.i++;
        return;
      }
      this.record([...path, index], this.i);
      this.scanValue([...path, index]);
      index++;
      this.skipFiller();
      if (this.peek() === ",") this.i++;
    }
  }

  private scanInlineTable(path: (string | number)[]): void {
    this.i++;
    for (;;) {
      this.skipFiller();
      if (this.i >= this.text.length) return;
      if (this.peek() === "}") {
        this.i++;
        return;
      }
      const start = this.i;
      const parts = this.readKey();
      for (let n = 1; n <= parts.length; n++) this.record([...path, ...parts.slice(0, n)], start);
      this.skipBlanks();
      if (this.peek() === "=") this.i++;
      this.scanValue([...path, ...parts]);
      this.skipFiller();
      if (this.peek() === ",") this.i++;
    }
  }
}
