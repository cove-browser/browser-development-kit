import { describe, expect, it } from "vitest";
import { type CspRule, cspIssues, isRemoteUrl } from "./csp.js";

const PAGE = `<!doctype html>
<html>
  <head>
    <meta charset="utf-8">
    <title>Settings</title>
    <base href="./">
    <link rel="stylesheet" href="./style.css">
    <link rel="stylesheet" href="chrome://resources/css/text_defaults.css">
    <link rel="modulepreload" href="./assets/chunk.js">
    <script type="application/json" id="data">{"a": 1}</script>
  </head>
  <body class="page">
    <button id="save">Save</button>
    <script type="module" src="./main.js"></script>
  </body>
</html>
`;

function rules(html: string): CspRule[] {
  return cspIssues(html, "Pages/Settings/index.html").map((i) => i.rule);
}

describe("cspIssues", () => {
  it("accepts a compliant page", () => {
    expect(cspIssues(PAGE, "index.html")).toEqual([]);
  });

  it("finds an inline script, with its line, and not an empty one", () => {
    const issues = cspIssues("<p>\n<script>alert(1)</script>", "a.html");
    expect(issues).toHaveLength(1);
    expect(issues[0]).toMatchObject({ file: "a.html", line: 2, rule: "inline-script" });
    expect(issues[0]?.fix).toContain("src=");
    expect(rules('<script type="module"></script>')).toEqual([]);
  });

  it("treats an inline importmap as a script and a JSON block as data", () => {
    expect(rules('<script type="importmap">{"imports":{}}</script>')).toEqual(["inline-script"]);
    expect(rules('<script type="application/ld+json">{}</script>')).toEqual([]);
  });

  it("does not read a closing tag inside a script as markup", () => {
    expect(rules('<script src="./a.js"></script><p onclick="x()">')).toEqual([
      "inline-event-handler",
    ]);
    expect(rules("<script type=module src=./a.js></script>")).toEqual([]);
  });

  it("finds inline event handlers on any tag, quoted or not", () => {
    expect(rules('<button onclick="go()">')).toEqual(["inline-event-handler"]);
    expect(rules("<img src=./a.png ONERROR=x()>")).toEqual(["inline-event-handler"]);
  });

  it("finds javascript: URLs, whatever the case and padding", () => {
    expect(rules('<a href="javascript:void(0)">x</a>')).toEqual(["javascript-url"]);
    expect(rules("<a href=' JaVa\tScript:x()'>x</a>")).toEqual(["javascript-url"]);
    expect(rules('<form action="javascript:x()">')).toEqual(["javascript-url"]);
    expect(rules('<a href="./javascript:x">x</a>')).toEqual([]);
  });

  it("finds remote scripts, stylesheets and module preloads", () => {
    expect(rules('<script src="https://cdn.example/x.js"></script>')).toEqual(["remote-script"]);
    expect(rules('<script src="//cdn.example/x.js"></script>')).toEqual(["remote-script"]);
    expect(rules('<link rel="stylesheet" href="https://x.example/a.css">')).toEqual([
      "remote-stylesheet",
    ]);
    expect(rules('<link rel="preload stylesheet" href="http://x.example/a.css">')).toEqual([
      "remote-stylesheet",
    ]);
    expect(rules('<link rel="modulepreload" href="https://x.example/a.js">')).toEqual([
      "remote-modulepreload",
    ]);
    expect(rules('<link rel="icon" href="https://x.example/a.png">')).toEqual([]);
  });

  it("allows chrome://resources/ and relative URLs only", () => {
    expect(isRemoteUrl("chrome://resources/js/x.js")).toBe(false);
    expect(isRemoteUrl("./a.js")).toBe(false);
    expect(isRemoteUrl("/@bdk/internal/settings.js")).toBe(false);
    expect(isRemoteUrl("chrome://settings/a.js")).toBe(true);
    expect(isRemoteUrl("data:text/javascript,1")).toBe(true);
    expect(isRemoteUrl("//cdn.example/a.js")).toBe(true);
  });

  it("finds every iframe, because the policy allows no frames", () => {
    expect(rules('<iframe src="./inner.html"></iframe>')).toEqual(["frame"]);
    expect(rules('<iframe src="https://x.example/"></iframe>')).toEqual(["frame"]);
  });

  it("finds style elements and style attributes, and not an empty style element", () => {
    expect(rules("<style>p{color:red}</style>")).toEqual(["inline-style-element"]);
    expect(rules('<p style="color:red">x</p>')).toEqual(["inline-style-attribute"]);
    expect(rules("<style></style>")).toEqual([]);
  });

  it("finds a base that points at another origin", () => {
    expect(rules('<base href="https://x.example/">')).toEqual(["base-elsewhere"]);
    expect(rules('<base href="./sub/">')).toEqual([]);
  });

  it("ignores comments, and reads on after a broken tag", () => {
    expect(rules("<!-- <script>alert(1)</script> --><p>ok</p>")).toEqual([]);
    expect(rules('<p class="a><script>x</script>')).toEqual([]);
    expect(rules("<p <b onclick=x>")).toEqual(["inline-event-handler"]);
  });

  it("reports every issue in document order", () => {
    expect(rules('<p style="x"><script>1</script><a onclick="y">')).toEqual([
      "inline-style-attribute",
      "inline-script",
      "inline-event-handler",
    ]);
  });
});
