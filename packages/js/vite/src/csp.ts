/**
 * Finds what the content security policy of a bundled internal page blocks in an HTML file
 * (architecture.md section 8.4, javascript-api.md section 2.2).
 *
 * The scanner is a small tokenizer, not an HTML parser. It reads comments, tags, quoted and unquoted
 * attributes, and the raw text of `script`, `style` and `textarea`. It does not build a tree, decode
 * character references (so `&#106;avascript:` is not seen) or run template syntax; a page that hides a
 * blocked construct behind either is still stopped at run time, by the policy itself. This check exists
 * so the developer hears about the usual mistakes at build time, with the file and the fix.
 */

export type CspRule =
  | "inline-script"
  | "inline-event-handler"
  | "javascript-url"
  | "remote-script"
  | "remote-stylesheet"
  | "remote-modulepreload"
  | "frame"
  | "inline-style-element"
  | "inline-style-attribute"
  | "base-elsewhere";

export interface CspIssue {
  file: string;
  /** 1-based line of the tag that holds the problem. */
  line: number;
  rule: CspRule;
  /** What the policy blocks. */
  what: string;
  /** What to change. */
  fix: string;
}

interface Attribute {
  name: string;
  value: string;
}

interface Tag {
  name: string;
  attributes: Attribute[];
  line: number;
  /** The raw text up to the closing tag, for `script`, `style` and `textarea`. */
  content: string;
}

const RAW_TEXT = new Set(["script", "style", "textarea", "title"]);

/** A URL the page may load: relative, or on `chrome://resources/`. Anything else is another origin. */
export function isRemoteUrl(url: string): boolean {
  const value = url.trim();
  if (value.toLowerCase().startsWith("chrome://resources/")) return false;
  return value.startsWith("//") || value.startsWith("\\\\") || /^[a-z][a-z0-9+.-]*:/i.test(value);
}

function lineAt(text: string, offset: number): number {
  let line = 1;
  for (let i = 0; i < offset; i++) if (text.charCodeAt(i) === 10) line++;
  return line;
}

const SPACE = /[\t\n\f\r ]/;

function readAttributes(html: string, start: number): { attributes: Attribute[]; end: number } {
  const attributes: Attribute[] = [];
  let i = start;
  for (;;) {
    while (i < html.length && (SPACE.test(html.charAt(i)) || html.charAt(i) === "/")) i++;
    if (i >= html.length) return { attributes, end: i };
    if (html.charAt(i) === ">") return { attributes, end: i + 1 };
    let nameEnd = i;
    while (nameEnd < html.length && !/[\t\n\f\r />=]/.test(html.charAt(nameEnd))) nameEnd++;
    // A stray "=" starts a name, as in the HTML specification; this keeps the loop moving.
    if (nameEnd === i) nameEnd = i + 1;
    const name = html.slice(i, nameEnd).toLowerCase();
    i = nameEnd;
    while (i < html.length && SPACE.test(html.charAt(i))) i++;
    let value = "";
    if (html.charAt(i) === "=") {
      i++;
      while (i < html.length && SPACE.test(html.charAt(i))) i++;
      const quote = html.charAt(i);
      if (quote === '"' || quote === "'") {
        const close = html.indexOf(quote, i + 1);
        const end = close === -1 ? html.length : close;
        value = html.slice(i + 1, end);
        i = close === -1 ? html.length : close + 1;
      } else {
        let end = i;
        while (end < html.length && !SPACE.test(html.charAt(end)) && html.charAt(end) !== ">")
          end++;
        value = html.slice(i, end);
        i = end;
      }
    }
    attributes.push({ name, value });
  }
}

function* tags(html: string): Generator<Tag> {
  let i = 0;
  while (i < html.length) {
    const open = html.indexOf("<", i);
    if (open === -1) return;
    if (html.startsWith("<!--", open)) {
      const close = html.indexOf("-->", open + 4);
      i = close === -1 ? html.length : close + 3;
      continue;
    }
    const next = html.charAt(open + 1);
    if (!/[a-z]/i.test(next)) {
      // `</x>`, `<!doctype>`, `<?x?>` and a bare "<" are not start tags.
      const close = html.indexOf(">", open + 1);
      i = close === -1 ? html.length : close + 1;
      continue;
    }
    let nameEnd = open + 1;
    while (nameEnd < html.length && !/[\t\n\f\r />]/.test(html.charAt(nameEnd))) nameEnd++;
    const name = html.slice(open + 1, nameEnd).toLowerCase();
    const { attributes, end } = readAttributes(html, nameEnd);
    let content = "";
    i = end;
    if (RAW_TEXT.has(name)) {
      const closing = new RegExp(`</${name}[\\t\\n\\f\\r />]`, "i");
      const rest = html.slice(end);
      const found = closing.exec(rest);
      content = found ? rest.slice(0, found.index) : rest;
      i = found ? end + found.index : html.length;
    }
    yield { name, attributes, line: lineAt(html, open), content };
  }
}

const SCRIPT_TYPES = /^(module|importmap|(application|text)\/(x-)?(java|ecma)script|)$/;

/** A `<script>` the browser runs or applies. Other types are data blocks and are not subject to the policy. */
function isExecutable(type: string | undefined): boolean {
  return SCRIPT_TYPES.test((type ?? "").trim().toLowerCase());
}

function attribute(tag: Tag, name: string): string | undefined {
  return tag.attributes.find((a) => a.name === name)?.value;
}

const URL_ATTRIBUTES = new Set([
  "href",
  "src",
  "action",
  "formaction",
  "data",
  "poster",
  "xlink:href",
]);

/** `javascript:` after the leading control characters and spaces a URL parser skips, and ignoring tabs and newlines inside. */
function isJavascriptUrl(value: string): boolean {
  // biome-ignore lint/suspicious/noControlCharactersInRegex: the URL parser strips exactly these characters.
  const cleaned = value.replace(/[\u0000- ]+/g, "").toLowerCase();
  return cleaned.startsWith("javascript:");
}

/** Every construct in `html` that a bundled page's content security policy blocks. `file` is only used in the result. */
export function cspIssues(html: string, file: string): CspIssue[] {
  const issues: CspIssue[] = [];
  const add = (tag: Tag, rule: CspRule, what: string, fix: string): void => {
    issues.push({ file, line: tag.line, rule, what, fix });
  };
  for (const tag of tags(html)) {
    for (const { name, value } of tag.attributes) {
      if (/^on[a-z]+$/.test(name)) {
        add(
          tag,
          "inline-event-handler",
          `The "${name}" attribute on <${tag.name}> is an inline event handler, which the policy blocks.`,
          `Remove the attribute and call addEventListener("${name.slice(2)}", …) from a script file.`,
        );
      }
      if (URL_ATTRIBUTES.has(name) && isJavascriptUrl(value)) {
        add(
          tag,
          "javascript-url",
          `The "${name}" attribute on <${tag.name}> is a javascript: URL, which the policy blocks.`,
          "Use a button with an event listener in a script file, or a real URL.",
        );
      }
      if (name === "style") {
        add(
          tag,
          "inline-style-attribute",
          `The style attribute on <${tag.name}> is inline style, which the policy blocks.`,
          "Move the rule into a stylesheet and use a class, or set the property from a script file with element.style.",
        );
      }
    }
    switch (tag.name) {
      case "script": {
        const src = attribute(tag, "src");
        if (!isExecutable(attribute(tag, "type"))) break;
        if (src === undefined) {
          if (tag.content.trim() !== "") {
            add(
              tag,
              "inline-script",
              "An inline <script> is blocked by the policy.",
              'Move the code to a file in the page directory and load it with <script type="module" src="./main.js"></script>.',
            );
          }
        } else if (isRemoteUrl(src)) {
          add(
            tag,
            "remote-script",
            `The script "${src}" is on another origin, which the policy blocks.`,
            "Install the package and import it from your own code, or copy the file into the page directory.",
          );
        }
        break;
      }
      case "style":
        if (tag.content.trim() !== "") {
          add(
            tag,
            "inline-style-element",
            "A <style> element is inline style, which the policy blocks.",
            'Move the rules to a .css file in the page directory and load it with <link rel="stylesheet" href="./style.css">.',
          );
        }
        break;
      case "link": {
        const href = attribute(tag, "href");
        const rel = (attribute(tag, "rel") ?? "").toLowerCase().split(/\s+/);
        if (href === undefined || !isRemoteUrl(href)) break;
        if (rel.includes("stylesheet")) {
          add(
            tag,
            "remote-stylesheet",
            `The stylesheet "${href}" is on another origin, which the policy blocks.`,
            "Copy the stylesheet into the page directory, or import it from an installed package.",
          );
        } else if (rel.includes("modulepreload")) {
          add(
            tag,
            "remote-modulepreload",
            `The module preload "${href}" is on another origin, which the policy blocks.`,
            "Remove the link, or point it at a file in the page directory.",
          );
        }
        break;
      }
      case "iframe":
        add(
          tag,
          "frame",
          "An <iframe> is blocked: the policy allows no frames in a bundled page.",
          "Remove the frame. Show the content in the page, or ask Swift for it through an app. command.",
        );
        break;
      case "base": {
        const href = attribute(tag, "href");
        if (href !== undefined && isRemoteUrl(href)) {
          add(
            tag,
            "base-elsewhere",
            `<base href="${href}"> points at another origin, which the policy blocks.`,
            "Remove the <base> element, or use a relative href.",
          );
        }
        break;
      }
    }
  }
  return issues;
}

/** One line per issue, as `file:line: what Fix: fix`, for an error message. */
export function formatCspIssues(issues: readonly CspIssue[]): string {
  return issues.map((i) => `${i.file}:${i.line}: ${i.what} ${i.fix}`).join("\n");
}
