# extensions-suite

The extension compatibility suite (task T-EXT-07): the probe and fixture extensions, the matrix that says what each
extension API and surface is expected to do and which engine tests prove it, the generator that turns the matrix and
the engine's results into the published page, and the list of real third-party extensions to run by hand.

Published output: [docs/extensions/compatibility-matrix.md](../../docs/extensions/compatibility-matrix.md). Its state
today is "not run": the engine tests that prove the rows need a runtime rebuilt on a developer's Mac
([amendments.md](../../docs/planning/amendments.md) 2.7).

## Layout

| Path | What |
|---|---|
| `probe/` | The MV3 probe: loaded unpacked for the T-EXT-01 assertions. Based on `docs/planning/experiments/mv3-probe`. |
| `fixtures/api-surface/` | Asks for every desktop API permission in the matrix and reports which namespaces exist. |
| `fixtures/incognito-spanning/`, `fixtures/incognito-split/` | One manifest per `incognito` mode, with the same worker. extensions.md 10 requires both before the matrix claims support. |
| `fixtures/blocker/`, `fixtures/passwords/`, `fixtures/devtool/` | One small extension for each class the M5 exit criteria name: a content blocker, a password manager, a developer tool. |
| `fixtures/install-*`, `fixtures/ui-*`, `fixtures/panel-*` | Owned by the engine tests of the same areas. The suite's own checks skip them. |
| `matrix.json` | The source of truth for every row of the matrix. |
| `expected-engine-tests.txt` | Generated: every engine test name the matrix expects. |
| `src/` | The generator, the engine runner and their unit tests. |
| `real/` | The list of real extensions and the script that fetches them. |
| `results/` | Engine output, gitignored. |

## Targets

Run with `pnpm exec nx run extensions-suite:<target>`.

| Target | Does |
|---|---|
| `test` | Unit tests of the generator, the result parser and the runner; validation of `matrix.json` (reasons, evidence names, statuses, every API of extensions.md section 7, the ten T-EXT-01 assertions, both incognito modes); validation of every fixture manifest (valid JSON, `manifest_version` 3, every referenced file and page script exists). Needs no Chromium. |
| `lint`, `format` | Biome, and `tsc --noEmit`. |
| `matrix` | Regenerates `docs/extensions/compatibility-matrix.md` and `expected-engine-tests.txt`. With `BDK_EXT_RESULTS=<file>` (or `--results`) the observed column comes from that engine output; with none it is "not run". |
| `matrix-check` | Fails when the committed generated files differ from what `matrix.json` produces with no results. CI runs it on every pull request. |
| `test-extensions` | Runs the engine conformance app's `engine_ext_*` tests through `packages/core/scripts/test-engine.sh`, saves the output to `results/engine-extensions.txt` and regenerates the matrix from it. With no runtime it prints "runtime rebuild needed ... Skipped." and exits 0, like the integration job. `pnpm test:release` runs it. |

Compare the matrix with the engine's own list of tests:

```sh
BDK_TEST_APP=packages/core/build-engine/BDKEngineConformance.app/Contents/MacOS/BDKEngineConformance
"$BDK_TEST_APP" --list > packages/core/build-engine/tests.txt
pnpm exec tsx tests/extensions/src/cli.ts coverage packages/core/build-engine/tests.txt
```

It names every expected test the engine lacks and every `engine_ext_*` test no row uses.

## The matrix

`matrix.json` holds one row per extension API or surface in extensions.md section 7 and per T-EXT-01 assertion, and
rows for the UI surfaces (section 4), the installation sources (5), private profiles (10), host mapping (8) and the
representative extension classes. A row has:

| Field | Meaning |
|---|---|
| `id` | `<group>.<name>`, unique. Groups: `assertion`, `api`, `surface`, `install`, `incognito`, `layout`, `class`. |
| `subject` | What the row is about. For `api` rows, the API as written in extensions.md section 7. |
| `expected` | `supported` behaves as in Chrome. `degraded` exists and differs. `absent` is missing from the extension's `chrome` object, by design. `gap` is a known missing or unverified capability. |
| `reason` | Why. Required. Rows that are not `supported` become the "Known differences from Chrome" section. |
| `evidence` | The engine test names that prove it. At least one. |
| `source` | Where the row comes from. |

Observed result per row: `passed` when every listed test passed, `failed` when any failed, `not run` when a test was
skipped or never printed. Expectations are the plan, not results; a failed row means the engine differs from the plan
and either the engine or the row is wrong.

### Evidence naming

Evidence test names are `engine_ext_<area>_<what>`: lower case words joined by underscores, the area one of

| Area | Written in | Covers |
|---|---|---|
| `install` | `packages/core/tests/tests_engine_extensions.c` | Sources, policy, inspection, prompts, uninstall, private profiles, real extensions |
| `ui` | `tests_engine_ext_ui.c` | Actions, popups, context menus, options pages, commands, host-backed APIs |
| `panel` | `tests_engine_ext_panels.c` | Side panels and DevTools extension panels |
| `probe` | any of them | Observations made with the probe and the fixtures |

The engine conformance app prints one line per test, `[ ok ] <name>` or `[FAIL] <name>` (with indented failure
lines), and a test that declines to run may print `[skip] <name>`. The suite selects its tests with
`--run 'engine_ext_*'`. A row may list several tests; an engine test may serve several rows. Adding an engine test for
a new behaviour means adding or extending a row; the generator lists `engine_ext_*` tests that no row uses.

## The probe protocol

Every fixture worker answers the same way, so the engine can collect results in one shape. A request is
`{"probe": "<name>", "args": {...}}`. The answer is

```json
{ "schema": 1, "probe": "<name>", "ok": true, "result": <any> }
{ "schema": 1, "probe": "<name>", "ok": false, "error": "<message>" }
```

`ok: false` means the probe itself failed (unknown probe, an exception). An API call the probe wants to observe
failing reports `ok: true` with `{"succeeded": false, "error": "..."}` in `result`, so a test can assert on the error.
A worker can be reached three ways:

1. `chrome.runtime.sendMessage` from any page of the extension (the popup, the options page, the side panel).
2. `chrome.runtime.sendMessage(<extension id>, request)` from a page on `http://127.0.0.1` or `http://localhost`
   (`externally_connectable`), which is how a harness drives an installed extension without extension-page access.
3. `bdkProbe(name, args)` evaluated in the worker (DevTools or the DevTools protocol).

MV3 workers stop when idle. The probe keeps its state in `chrome.storage.session` and registers every listener at the
top level, so an answer does not depend on the worker having stayed alive.

`probe/` probes, by T-EXT-01 assertion:

| Assertion | Probes |
|---|---|
| 1 worker starts, content scripts acknowledge | `acks` (each content script reports `{tabId, frameId, url}`); the page attribute `data-bdk-probe="ack:<tabId>"` |
| 2 active tab and current window | `snapshot`: `activeCurrent`, `currentWindowId`, `all`, `windows` |
| 3 focus fires `tabs.onActivated` | `events` (`tabs.onActivated`), `clearEvents` |
| 4 move keeps the id, detach then attach | `move`, then `events` (`tabs.onDetached`, `tabs.onAttached`) |
| 5 action grants `activeTab` | `inject` `{tabId}`: succeeds only where a grant exists |
| 6 popup anchored, same active tab, closes on blur | `reports`: the popup, side panel and options page each report what they believe the active tab is, and `{"closed": true}` on `pagehide`; `openPopup` |
| 7 messages reach both pages | `pingContent` `{tabId}`, `pingAll` |
| 8 context-menu item and click | `menu` (creation result), `events` (`contextMenus.onClicked` with the tab) |
| 9 `sidePanel.open` | `openSidePanel` `{tabId}` outside a gesture, and the `sidePanel.open.result` event recorded when the context-menu click (a gesture) opens it |
| 10 parking context | `snapshot.windows`, listing windows of every type with `incognito` and `type` |

The probe has no host permissions on purpose: `inject` must fail on a page until the action grants `activeTab`.
Its content script matches `http` and `https` pages only, so run it against a local server, not `file:` or `data:`.
Its manifest differs from `docs/planning/experiments/mv3-probe` where that fixture would not have worked: the worker
kept events in memory, which MV3 discards when it stops; the popup and side panel asked the worker for the "current
window", which is the worker's and not the popup's; `action.onClicked` never fires with a `default_popup`; and
`sidePanel.open` can only succeed inside a user gesture.

Other fixtures: `api-surface` answers `apiPresence` (namespaces and members, for the `absent` and `degraded` rows),
`recentlyClosed` and `commands`; the incognito fixtures answer `context` (`inIncognitoContext`,
`allowedIncognitoAccess`, `starts`, `windows`) and `hellos`; `blocker` answers `rules`; `passwords` answers
`credential` and `fill`; `devtool` answers `panelEvents` (the panel's `inspectedWindow.eval` result).

## Real extensions

`real/extensions.json` lists real extensions of the three classes: uBlock Origin Lite (content blocker), Bitwarden
(password manager) and React Developer Tools (developer tool), each with its upstream project, licence, version, the
release asset URL from the project's own GitHub releases (not the Chrome Web Store), and a SHA-256.

They are not vendored. Their licences differ from the repository's, nobody here reviews or updates their code, and a
stale copy would make the suite pass against an extension that no longer exists. They are fetched on demand:

```sh
pnpm exec tsx tests/extensions/real/fetch.ts [id ...]
```

`fetch.ts` downloads into `$BDK_HOME/extensions-cache/<id>/<version>/` (BDK_HOME resolved as in
`tooling/src/paths.ts`), checks the SHA-256 when one is recorded and refuses a mismatch, and unzips with the system
`unzip` into `unpacked/`. With no digest recorded it prints the one it saw; record it in `extensions.json` in the
same change. The file was written without network access: where a version or asset URL could not be confirmed it
is `null` and `fetch.ts` refuses the entry, so resolve it from the project's releases first. The engine tests for
real extensions (`engine_ext_install_real_*`) skip when the cache has no copy.

## What CI can and cannot run

CI runs `test`, `lint` and `matrix-check`: everything above that needs no Chromium. It cannot run `test-extensions`
(no runtime; the integration job reports "runtime rebuild needed" and skips when `chromium/pin.json` records none) or
`real/fetch.ts`, so the published matrix stays "not run" until a developer builds the runtime, runs
`pnpm test:release` and commits the regenerated page with the runtime they publish.
