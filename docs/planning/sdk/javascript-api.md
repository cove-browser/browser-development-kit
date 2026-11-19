# BDK JavaScript and TypeScript APIs

Status: proposal, 2026-11-15. Based on the spec's section 6 and [../architecture.md](../architecture.md) sections 8 and 9. Module, function and option names are placeholders, except the package names, which are the spec's. Nothing here has been built. Status labels are in [README.md](README.md) section 2. Tasks: T-JS-01 to T-JS-04, T-WUI-03, T-WUI-04, T-AUT-01 to T-AUT-05, T-CLI-04.

JavaScript is optional. An application written only in Swift uses none of this, and Swift stays the source of truth for application commands.

## 1. Two SDKs, two trust domains

| | `@bdk/internal/*` | `@bdk/automation`, `@bdk/commands`, `@bdk/events` |
|---|---|---|
| Runs in | A packaged internal page, in a sandboxed renderer with WebUI bindings | The automation host: a separate process with no page |
| Authority | The capabilities granted to that logical page in `bdk.toml` | The capability set and scope of the session Swift created |
| Reaches the broker by | A Mojo interface bound to that page | The host's channel, or the authenticated local socket |
| Can drive other pages | No | Yes, within scope |
| Built with | Vite, or nothing | Rolldown |
| Milestone | M4 | M7 |

The two cannot call each other. They meet only at the broker. An import path grants nothing: importing `@bdk/internal/settings` in a page with no `settings.read` grant gives functions whose calls are refused.

Ordinary web pages and extensions get neither SDK.

## 2. Internal pages

### 2.1 Layout

```text
BrowserApp/
├── bdk.toml
└── Pages/
    ├── Settings/                # <scheme>://settings
    │   ├── index.html
    │   ├── main.ts
    │   └── style.css
    ├── NewTab/                  # <scheme>://new-tab
    │   └── index.html
    ├── Agents/                  # <scheme>://agents — an application route
    │   └── index.html
    ├── package.json             # optional: only if pages use npm packages or Vite
    ├── vite.config.ts           # optional
    └── tsconfig.json            # optional
```

| Rule | Detail |
|---|---|
| A page | A directory directly under `Pages/` with an `index.html` |
| Route name | The directory name, split on case boundaries, lower-cased and hyphenated: `NewTab` becomes `new-tab` |
| Sub-paths | `<scheme>://settings/privacy` serves `Pages/Settings/index.html` when no file matches, so client-side routing works. Files are served by path otherwise. |
| Shared code | Any directory under `Pages/` with no `index.html` is not a page and can hold shared modules, for example `Pages/shared/` |
| No build needed | A page of plain HTML, CSS and ES modules is packaged as it is |
| With Vite | If `Pages/vite.config.*` exists, the build runs Vite with BDK's preset (section 8) and packages its output |

### 2.2 What a page is at runtime

| Property | Value |
|---|---|
| Address the user sees and types | `<scheme>://settings` |
| `location.origin` inside the page, and in DevTools | `chrome://bdk-settings`. This is the alias design; see `architecture.md` 8.3 and decision D3. |
| Process | A renderer locked to that origin, with WebUI bindings |
| Served from | The signed application bundle, by a BDK data source |
| Network | The page may use `fetch` for `https:` resources only as far as its content security policy allows; see below |
| Storage | `localStorage` and IndexedDB work and are per page origin, per profile |
| Links between internal pages | Use the branded form: `<a href="cove://history">`. BDK also exposes `page.url(route)` so a page does not hard-code the scheme. |

Content security policy, Chromium's standard for WebUI. The page cannot relax it:

| Allowed | Blocked |
|---|---|
| Scripts and styles from the page's own bundle | Inline `<script>`, `eval`, `new Function` |
| Images from the bundle, `data:` and the favicon service | Scripts from any remote origin |
| `fetch` to the page's own origin | Remote frames |
| Shared BDK resources under `/@bdk/` | Loading another internal page's files |
| DOM writes through a Trusted Types policy the preset installed | Assigning a plain string to `innerHTML` and similar sinks |

Trusted Types matters for the choice of framework. Chromium's WebUI policy enforces it and allows no policy by default, and several frameworks create their templates by assigning strings to `innerHTML`. How BDK handles that is an open implementation choice ([README.md](README.md) section 7), and it is why `bdk init` offers only frameworks with a passing fixture ([cli.md](cli.md) 4.3).

Whether a page may `fetch` remote `https:` data at all is a per-page decision the plan has not made. The proposal: no, by default; a page that needs remote data asks Swift through an `app.` command, so that the network access is in audited native code. This is stricter than Chromium's own pages and can be relaxed later.

### 2.3 Importing the SDK

| Page kind | Import |
|---|---|
| Built with Vite | `import { settings } from "@bdk/internal/settings"`. The preset resolves it. |
| No build step | `import { settings } from "/@bdk/internal/settings.js"`. The data source serves the SDK at a reserved path under every bundled page. |

Both give the same module. Types come from the generated declarations (section 7).

## 3. `@bdk/internal/*`

Plan, M4, task T-JS-03. One module per service namespace. Each function is a thin typed wrapper over one broker command in [services.md](services.md) section 5, with the same name after the dot.

| Module | Wraps | Capabilities used |
|---|---|---|
| `@bdk/internal/page` | `runtime.caller`, `runtime.info` | none, `runtime.read` |
| `@bdk/internal/settings` | `settings.*` | `settings.read`, `settings.write` |
| `@bdk/internal/profiles` | `profiles.*` | `profiles.read`, `profiles.manage` |
| `@bdk/internal/history` | `history.*` | `history.read`, `history.write` |
| `@bdk/internal/bookmarks` | `bookmarks.*` | `bookmarks.read`, `bookmarks.write` |
| `@bdk/internal/downloads` | `downloads.*` | `downloads.read`, `downloads.manage` |
| `@bdk/internal/permissions` | `permissions.*` | `permissions.read`, `permissions.manage` |
| `@bdk/internal/privacy` | `cookies.*` without values, `privacy.*` | `cookies.read`, `cookies.manage`, `privacy.clear` |
| `@bdk/internal/passwords` | `passwords.*` | `passwords.read`, `passwords.manage`, `passwords.reveal` |
| `@bdk/internal/autofill` | `autofill.*` | `autofill.read`, `autofill.manage` |
| `@bdk/internal/extensions` | `extensions.*` | `extensions.list`, `extensions.manage` |
| `@bdk/internal/translation` | Preferences and models from `translation.*` | `translation.read`, `translation.manage` |
| `@bdk/internal/search` | `search.*` | `search.read`, `search.manage` |
| `@bdk/internal/favicons` | `favicons.get` | `favicons.read` |
| `@bdk/internal/top-sites` | `topSites.*` | `top-sites.read` |
| `@bdk/internal/system` | `system.defaultBrowser.*` | `system.default-browser` |
| `@bdk/internal/commands` | `app.*` commands | The command's own name |
| `@bdk/internal/events` | `app.*` events | The event's own name |

There is no `@bdk/internal/contents`, `debug` or `automation`. Pages cannot hold those capabilities ([services.md](services.md) section 4).

### 3.1 Shape

```ts
// @bdk/internal/page
export const page: {
  readonly route: string;                 // "settings"
  readonly capabilities: readonly string[];
  can(capability: string): boolean;       // true for an exact grant or a covering app.* wildcard
  url(route: string, path?: string): string;   // "cove://history"
  info(): Promise<RuntimeInfo>;           // needs runtime.read
};

// @bdk/internal/settings
export const settings: {
  get<K extends SettingKey>(key: K): Promise<SettingValue<K>>;
  getMany<K extends SettingKey>(keys: K[]): Promise<{ [P in K]: SettingValue<P> }>;
  describe<K extends SettingKey>(key: K): Promise<SettingDescription<K>>;
  set<K extends SettingKey>(key: K, value: SettingValue<K>): Promise<void>;
  reset(key: SettingKey): Promise<void>;
  onChanged(listener: (change: SettingChange) => void, options?: { keys?: SettingKey[] }): Subscription;
};

// @bdk/internal/commands — generated from the application's Swift declarations
export const commands: {
  execute<N extends AppCommandName>(name: N, input: AppCommandInput<N>, options?: CallOptions): Promise<AppCommandOutput<N>>;
};

// Shared
export interface CallOptions { signal?: AbortSignal; onProgress?: (p: Progress) => void; }
export interface Subscription { unsubscribe(): void; }
export class BdkError extends Error { domain: string; code: string; details?: unknown; }
```

| Rule | Detail |
|---|---|
| Typing | Setting keys, command names, inputs and outputs are literal types generated from the interface definitions and from the application's Swift declarations. A misspelt key or command is a compile error. |
| Errors | Every rejection is a `BdkError` with the same domain and code Swift sees |
| Cancellation | `AbortSignal` |
| Lists | `AsyncIterable` for paginated commands: `for await (const item of history.search({ text }))` |
| Events | `on…(listener)` returns a `Subscription`. Subscriptions end with the page. |
| Gestures | A command marked G in `services.md` must be called from a user-gesture handler in the page, such as a click. Called from a timer, it rejects with `capability.gestureRequired`. |
| Feature detection | `page.can("history.write")` lets one page serve several products with different grants |

### 3.2 A settings page

`bdk.toml`:

```toml
[pages.capabilities.settings]
allow = ["settings.read", "settings.write", "search.read", "search.manage", "app.appearance.*"]
```

`Pages/Settings/main.ts`:

```ts
import { page } from "@bdk/internal/page";
import { settings } from "@bdk/internal/settings";
import { search } from "@bdk/internal/search";
import { commands } from "@bdk/internal/commands";

const cookies = document.querySelector<HTMLSelectElement>("#third-party-cookies")!;
cookies.value = await settings.get("privacy.cookies.thirdParty");
cookies.addEventListener("change", () => settings.set("privacy.cookies.thirdParty", cookies.value as CookiePolicy));

settings.onChanged(change => { cookies.value = change.value as string; }, { keys: ["privacy.cookies.thirdParty"] });

for (const engine of await search.engines()) addEngineRow(engine);

document.querySelector("#accent")!.addEventListener("click", () =>
  commands.execute("app.appearance.setAccent", { color: "#3366ff" }));

if (!page.can("search.manage")) document.querySelector("#engines-edit")!.remove();
```

The value Swift reads with `profile.settings.get(.privacyThirdPartyCookies)` is the one this page wrote. There is one settings store.

### 3.3 A new-tab page

```toml
[pages.capabilities.new-tab]
allow = ["top-sites.read", "favicons.read", "app.tabs.open"]
```

```ts
import { topSites } from "@bdk/internal/top-sites";
import { favicons } from "@bdk/internal/favicons";
import { commands } from "@bdk/internal/commands";

for (const site of await topSites.list({ limit: 8 })) {
  const tile = renderTile(site.title, await favicons.url(site.url, 32));
  tile.addEventListener("click", () => commands.execute("app.tabs.open", { url: site.url }));
}
```

A new-tab page with no table in `bdk.toml` has no capability. It is still an isolated internal page; it just cannot ask the browser for anything.

## 4. What a page cannot do

| Attempt | Result |
|---|---|
| Call a command it has no grant for | Rejects with `capability.denied` |
| Call from a subframe | The interface is never bound to subframes |
| Navigate itself to an `https:` page and keep calling | Bindings are dropped on navigation |
| Load a remote script | Blocked by the content security policy |
| Open another internal page in a frame to borrow its grants | Blocked; and capabilities follow the top-level page identity |
| Ask for the broker interface from a page with no entry in the resolved table | The renderer is terminated, as for any unregistered WebUI interface |
| Execute Swift | Only typed `app.` commands the application registered |
| Answer a permission prompt or any UI request | UI requests go to Swift only |

## 5. The automation host

Plan, M7. The runtime choice is a spike (task T-AUT-01): V8 in a sandboxed utility process, or an external runtime on the local channel. The API below is the same either way. If the first is chosen, scripts have no Node APIs; that is risk R21.

### 5.1 Layout and build

```text
BrowserApp/
└── Automation/
    ├── summarise.ts          # a script named "summarise"
    ├── nightly-export.ts
    └── lib/                  # shared modules; not scripts
```

Each top-level file is one script. `bdk build automation` bundles each with Rolldown into the application bundle and records its digest in the resolved file. Only bundled scripts can run in the in-process host; a session cannot be handed source text at runtime unless the application registers an `app.` command that does it.

### 5.2 A script

```ts
import { contents, session, task } from "@bdk/automation";
import { commands } from "@bdk/commands";
import { events } from "@bdk/events";

export default async function main(args: { url: string }) {
  const page = await contents.create({ url: args.url });        // in scope because the session created it
  await page.waitFor({ load: "finished" });

  const title = await page.evaluate(() => document.title);      // isolated world by default
  const text = await page.text();
  task.progress(0.5, "read page");

  const off = events.on("contents.navigated", e => task.log(`navigated: ${e.url}`), { contents: page.id });

  await commands.execute("app.notes.create", { title, body: text.slice(0, 2000) });
  off.unsubscribe();
  await page.close();
  return { title };
}
```

Swift starts it with `session.run("summarise", arguments: […])` ([swift-api.md](swift-api.md) section 13).

### 5.3 `@bdk/automation`

| Export | Wraps | Notes |
|---|---|---|
| `session` | `runtime.caller` | Name, capabilities, scope. `session.can(capability)`. |
| `task` | The running task | `progress`, `log`, `signal` (aborted when Swift cancels or the session is revoked) |
| `contents` | `contents.*` | `list`, `get`, `create`; each returns a `Page` |
| `Page` | `contents.*`, `translation.*`, `debug.*` for one page | Methods below |
| `profiles`, `history`, `bookmarks`, `downloads`, `settings`, `permissions`, `cookies`, `privacy`, `extensions`, `search`, `passwords`, `autofill` | The service namespaces | Same shapes as the `@bdk/internal` modules, so code can be shared |
| `scripts` | `scripts.*` | Persistent user scripts |

```ts
interface Page {
  readonly id: string;
  url(): Promise<string>; title(): Promise<string>; state(): Promise<PageState>;
  navigate(url: string, options?: NavigateOptions): Promise<Commit>;
  reload(): Promise<void>; back(): Promise<void>; forward(): Promise<void>; close(): Promise<void>;
  waitFor(condition: WaitCondition, options?: { timeoutMs?: number; signal?: AbortSignal }): Promise<void>;

  evaluate<T>(fn: string | ((...a: any[]) => T), options?: { args?: unknown[]; world?: "isolated" | "page"; frame?: FrameRef }): Promise<T>;
  query(selector: string, options?: { frame?: FrameRef }): Promise<ElementInfo[]>;
  text(options?: { frame?: FrameRef }): Promise<string>;
  snapshot(kind: "dom" | "accessibility"): Promise<Snapshot>;
  frames(): Promise<FrameInfo[]>;
  insertCSS(css: string): Promise<{ remove(): Promise<void> }>;

  click(target: Target): Promise<void>; type(target: Target, text: string): Promise<void>;
  press(key: string): Promise<void>; scroll(target: Target | { dx: number; dy: number }): Promise<void>;

  screenshot(options?: ScreenshotOptions): Promise<Uint8Array>;
  pdf(options?: PdfOptions): Promise<Uint8Array>;

  translate(to: string): Promise<void>; revertTranslation(): Promise<void>;
  debug(): Promise<DebugSession>;          // needs contents.debug
  requestAttention(): Promise<void>;       // ask the application to show the page
}
```

`@bdk/commands` exports `commands.execute` for `app.` commands. `@bdk/events` exports `events.on` for browser and `app.` events. Both are typed from the same generated declarations as their `@bdk/internal` counterparts.

### 5.4 Rules

| Rule | Detail |
|---|---|
| Authority | Every call is checked against the session's capabilities and scope. A script cannot raise either. |
| Scope | `contents.list()` returns only pages in scope. A page identifier outside scope behaves as if it did not exist. |
| Visibility | Pages a script creates are off screen until the application attaches them. `requestAttention` asks; the application decides. |
| Prompts | A script cannot answer a permission prompt, a dialog that needs a gesture, an install prompt or an interstitial. It can observe `contents.dialogOpened` and wait, or fail. |
| Internal pages | Cannot be scripted, captured through the protocol or attached to |
| Lifetime | A task ends when its function returns, when Swift cancels it, or when its session is revoked. Whether tasks survive all windows closing is the application's choice: the host lives as long as the runtime. |
| Crash | If the host process dies, every session ends with `automation.hostGone`. The application may create new sessions; BDK restarts the host on demand. |
| Audit | Every command a session invokes is reported to Swift as an event, with the session name, command and target |
| Resource limits | CPU and memory budgets for the host are open in the spec and set in task T-AUT-02 |

### 5.5 External tools

When the application opens the local endpoint, an external process speaks the same protocol the host does: connect to the socket, present a token, receive a session. The application's own CLI or gateway is the expected client. BDK publishes the wire protocol in `packages/js/protocol` and offers `@bdk/automation` as a client for Node-compatible runtimes:

```ts
import { connect } from "@bdk/automation/client";
const bdk = await connect({ socketPath: process.env.COVE_AUTOMATION_SOCKET!, token: process.env.COVE_AGENT_TOKEN! });
const pages = await bdk.contents.list();
```

BDK ships no CLI, no MCP or ACP server, no agent identity and no consent UI. Those are the application's ([../risks.md](../risks.md) section 3).

## 6. DevTools customisation

Plan, M7, task T-AUT-05. Chosen in `[devtools]` ([configuration.md](configuration.md) 3.12).

| Mode | What the developer supplies | Packaging |
|---|---|---|
| Default | Nothing | Chromium's front end, from the runtime |
| Modified | A directory with `overlay.css` and `overlay.js` | Bundled and loaded into the default front end after it starts. The script runs in the front end's context and may change its DOM. |
| Replaced | A complete front end: an `index.html` that speaks the DevTools protocol through the embedder channel | Bundled; served in place of the default |
| Disabled | Nothing | No front end. The protocol is still available to Swift and to sessions unless `protocol = false`. |

Rules: front-end resources are packaged in the signed bundle, never loaded from a URL in a release build. An overlay or replacement gets no `@bdk/internal` capability; it is DevTools, with DevTools' own powers over the inspected page and nothing more.

## 7. Code generation

Plan, M4, tasks T-JS-01 and T-JS-02. Nothing is published to npm. Everything is generated on the developer's machine during the build.

```text
packages/core/interface/*.json  ─────────────┐   shipped inside the BDK package
application Swift declarations ──► extractor ─┤   commands, events, settings
bdk.toml grants ──────────────────────────────┤
                                              ▼
                                      bdk build codegen
                                              ▼
<derived data>/bdk/js/@bdk/internal/…   .js and .d.ts
<derived data>/bdk/js/@bdk/automation/…
<derived data>/bdk/js/@bdk/commands/…   typed from the application's commands
<derived data>/bdk/js/@bdk/events/…
<derived data>/bdk/js/@bdk/env.d.ts     route names, setting keys, the internal scheme
```

| Topic | Rule |
|---|---|
| Source of truth | BDK's interface definitions for browser services; the application's Swift types for `app.` commands, events and settings |
| Extraction | The build compiles a small tool from the application's command module. It builds the same `BDKCommandRegistry` the application starts with and calls `declarations.write(to:)`, which writes `bdk.commands.json` (`{"version":1,"commands":[…],"events":[…]}`, entries in the interface-definition format of [c-abi.md](c-abi.md) section 7, sorted so that equal input gives equal bytes). The generator reads that file. Settled in T-SVC-05. |
| Where output lives | Xcode's derived data. Not in the source tree, not committed. |
| Editor support | The build keeps a git-ignored `Pages/node_modules/@bdk` and `Automation/node_modules/@bdk` link pointing at the generated output, so editors and `tsc` resolve the types. `bdk build codegen` can be run by hand to refresh it without a full build. |
| Per-page typing | Proposed: declarations are not narrowed per page. Every module type-checks in every page; the runtime check is what enforces grants. Narrowing would make shared code between pages awkward. |
| Staleness | The generated files carry the digest of their inputs. A page built against stale declarations fails the build with a message that says to run code generation. |
| No JavaScript | With no `Pages/` and no `Automation/`, nothing is generated |

## 8. Build tooling

| Tool | Used for | Supplied by BDK |
|---|---|---|
| Vite | Internal pages, optional | A preset: `import { bdk } from "@bdk/vite"` in `Pages/vite.config.ts`. It sets one entry per page directory, resolves `@bdk/internal/*`, emits no inline scripts, uses relative asset URLs, and fails the build on anything the content security policy would block. |
| Rolldown | Automation scripts | A fixed configuration run by `bdk build automation`: one bundle per script, ES module output, `@bdk/*` left external and provided by the host |
| Frameworks | Internal pages, optional | Starter templates for the frameworks `bdk init` offers ([cli.md](cli.md) 4.3): vanilla, React, Vue, Svelte, Solid, Preact and Lit as proposed, each in TypeScript or JavaScript. `@bdk/internal/*` is framework-neutral: promises and async iterators, no hooks or stores. A template wraps them in the framework's own idiom in a few lines the developer owns. |
| TypeScript | Type checking | Generated declarations and a base `tsconfig` to extend |
| Node and a package manager | Only if the application uses Vite or npm packages | Not supplied. `bdk doctor` reports their versions. An application with plain HTML pages needs neither. |

Development loop: `bdk build pages --watch` rebuilds pages into the built application's bundle while it runs, and a debug runtime reloads an internal page when its files change. Debug builds read pages from the bundle exactly as release builds do; there is no dev-server mode that loads a privileged page from `http://localhost`, because that would be remote content in a privileged origin.

## 9. Testing

| What | How |
|---|---|
| A page's logic | `@bdk/internal/testing` exports a mock transport. A unit test registers responses per command and asserts on the calls made. Runs in any JavaScript test runner with a DOM. |
| A page against real services | An application integration test launches the application, navigates to the route and drives it through the automation API or XCUITest |
| A page's grants | The mock transport can be loaded with the page's grants from `bdk.toml`, so calling an ungranted command fails in the unit test as it would at runtime |
| An automation script | The same mock, with a fake `contents` implementation |

## 10. Summary of guarantees

1. No BDK API exists in ordinary web pages or in extension contexts.
2. An internal page has exactly the capabilities `bdk.toml` gives its logical page, bound to its native identity and re-checked on every call.
3. An alias shares its target's identity and grants. Nothing else does.
4. Privileged pages load no remote code.
5. Automation runs apart from every page, with the capability set and scope Swift chose.
6. Internal pages and automation meet only at the broker.
7. Every `app.` command is a typed call into a handler the application registered. There is no path from JavaScript to arbitrary Swift.
