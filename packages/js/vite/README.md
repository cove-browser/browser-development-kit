# @bdk/vite

BDK's Vite preset for internal pages, and the content security policy check that `bdk build pages` shares with it.
Design: [javascript-api.md](../../../docs/planning/sdk/javascript-api.md) sections 2 and 8. Task: T-WUI-03 (part).

```ts
// Pages/vite.config.ts
import { bdk } from "@bdk/vite";
export default { plugins: [bdk()] };
```

## What `bdk()` does

| Setting | Value |
|---|---|
| Entries | One per directory directly under the pages root that holds an `index.html`. Dot-directories, `node_modules` and directories with no `index.html` (such as `Pages/shared/`) are not pages. |
| Output | One directory per page directory, mirroring the source: `Pages/Settings/index.html` is emitted as `Settings/index.html`. |
| `@bdk/internal/<module>` | Left external and rewritten to `/@bdk/internal/<module>.js`, where the runtime serves the SDK. A no-build page imports the same path. |
| `base` | `"./"`: relative asset URLs. |
| `build.assetsInlineLimit` | `0`: no asset becomes a `data:` URL. |
| Module preload | No polyfill, which is inline script. |
| Content security policy | The build fails, naming the file, line and fix, when an HTML file has anything the policy blocks. The source HTML is checked before Vite transforms it (Vite would turn an inline module script into a chunk) and the emitted HTML after. |

`BDK_PAGE=<directory>` builds that page alone, with its directory as the root, so its `index.html` and `assets/` are at
the top of the output and can be served as the page's whole origin. `bdk build pages` sets it and builds one page at a
time; each page is its own origin and cannot share chunks with another.

## `cspIssues(html, file)`

Returns what the policy blocks in an HTML file, as `{ file, line, rule, what, fix }`: an inline `<script>` with content,
inline event-handler attributes, `javascript:` URLs, `<script src>` and `<link rel=stylesheet|modulepreload>` that are
not relative paths or `chrome://resources/`, every `<iframe>` (the policy allows no frames), `<style>` elements and
`style=` attributes, and a `<base>` that points at another origin. Data blocks (`<script type="application/json">`) are
not scripts. The scanner is a small tokenizer, not an HTML parser; its limits are in `src/csp.ts`. It does not look at
JavaScript, so `eval` and `new Function` are not found at build time. The CLI imports it from
`@bdk/vite/tooling`, which does not load the plugin.

## Targets

`pnpm exec nx run js-vite:build`, `:test` (unit tests for each rule and a `vite.build()` integration test on a
two-page fixture), `:lint`, `:format`. `vite` is a development dependency, for types and the integration test only;
the preset imports it for types only because it is loaded by the application's own Vite.
