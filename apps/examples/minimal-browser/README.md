# minimal-browser

A small AppKit browser built on the Swift SDK: one window, two pages side by side, each with back, forward,
reload and an address field. Task T-SWF-05; the code follows [swift-api.md section 18](../../../docs/planning/sdk/swift-api.md).

## Which runtime it runs on

`Package.swift` decides. When a packaged runtime is resolved at `.bdk/runtime` (`pnpm chromium:package`, then
`pnpm chromium:runtime`; `BDK_RUNTIME_MODE=source` uses the local store only), the application links that
runtime's Chromium framework, `scripts/bundle.sh` copies the framework into the bundle, and `bdk build sign`
re-signs it as this application. The pages are real Chromium pages. Otherwise, or with `BDK_RUNTIME=stub`, it
runs on the in-memory stub runtime from `BDKTestSupport`: nothing renders, each page shows its URL and title in
the placeholder `BDKWebView` draws, and the window says so at the top. The status file reports which
(`"runtime": "chromium"` or `"stub"`). `Sources/MinimalBrowser/RuntimeSource.swift` is the one file that knows.

## What it shows

* The runtime starts from the bundled `bdk.resolved.json`, produced by the `bdk` CLI from this directory's
  `bdk.toml`. `applicationShouldTerminate` shuts the runtime down.
* A profile is opened and two pages are created in it, each shown by its `BDKWebView`.
* The window title follows the focused page's title.
* On quit, each page's `sessionState()` is saved to `session.json` in Application Support
  (`~/Library/Application Support/dev.bdk.examples.minimal-browser`). On launch, `restore` rebuilds the pages from it.
  With the real runtime that is Chromium's own navigation history, so back and forward work after a relaunch.
* When a page's renderer goes away (it crashed or was killed), the page reloads; the other page is untouched.
* Quitting goes through `applicationShouldTerminate` twice: `BDKRuntime.terminate(replyingTo:)` cancels the first
  attempt, shuts the runtime down, then quits again.

## The Settings page

`Pages/Settings/` is a bundled internal page: plain HTML, CSS and ES modules, with no build step. `bdk.toml` sets
`internal_scheme = "minimal"` and grants the page `settings.read` and `settings.write`
(`[pages.capabilities.settings]`). It imports `/@bdk/internal/page.js` and `/@bdk/internal/settings.js`, and keeps to
the content security policy: no inline script or style.

Open it by typing `minimal://settings` in a page's address field (a Chromium runtime is needed, and the runtime
must serve bundled pages: the stub renders nothing). It should show:

* "This is the route "settings", at minimal://settings."
* The page's capabilities: `settings.read` and `settings.write`.
* A "Do Not Track" checkbox that shows the current value of the `privacy.doNotTrack` setting. Ticking it saves the
  value (a line says "Saved privacy.doNotTrack = true.") and the Swift side reads the same value
  (`profile.settings`). A change made elsewhere updates the box through the `settings.changed` event.

With no grant the checkbox would stay disabled and an error would show instead. `bdk build pages` (run by
`scripts/bundle.sh`) refuses the page if it ever gains an inline script or style.

The example does not declare commands, so `bdk build codegen` says so in a note and generates no `app.`
declarations. To get editor types for the page, run `bdk build config codegen --project apps/examples/minimal-browser`:
it writes the `@bdk` packages to `.bdk/js` and links them at `Pages/node_modules/@bdk`.

## Run it

```sh
pnpm dev:example                          # builds the bundle, then launches it
pnpm exec nx run minimal-browser:build    # resolve config, swift build, assemble .build/MinimalBrowser.app
pnpm exec nx run minimal-browser:test     # unit tests
pnpm exec nx run minimal-browser:test-e2e # launches the built app
pnpm exec nx run minimal-browser:lint
```

`scripts/bundle.sh` resolves the configuration with `packages/cli/dist/bdk` when it exists and with the
TypeScript sources otherwise, builds the executable, and assembles the bundle with `Info.plist`
(`NSPrincipalClass` = `BDKApplication`, bundle identifier `dev.bdk.examples.minimal-browser`) and
`bdk.resolved.json` in `Resources`. `bdk build pages` then packages `Pages/` and the SDK into the bundle's
`Resources` (`Pages/Settings`, `BDK/js/internal`). Built against the real runtime it also gets `Chromium Framework.framework` in
`Contents/Frameworks` (an APFS clone of the packaged one), and `bdk build sign --identity -` re-signs the
framework and its helpers as this application; against the stub the bundle is signed ad hoc as it is.

It is a SwiftPM package that depends on the repository's root package by relative path. It builds with
`swift build` in this directory, without Xcode.

## Layout

| Path | Contents |
|---|---|
| `Sources/MinimalBrowser` | The application: `main.swift`, `AppDelegate`, `BrowserWindow`, `BrowserPane`, and `RuntimeSource` |
| `Sources/MinimalBrowserCore` | Logic with no runtime: launch options, the status report, address parsing |
| `Sources/MinimalBrowserKit` | The session store |
| `Tests/MinimalBrowserTests` | Unit tests (the stub runs in process) |
| `Tests/MinimalBrowserE2ETests` | Launches the built `.app`; `LocalServer` serves the profile tests |

## Test flags

The tests steer the app with launch arguments, because a person launching it passes none:
`--support-dir <dir>` (where the session goes; with the real runtime, its data directory too, in `Engine`),
`--status-file <file>` (a JSON report of the window and each page's URL, title, entries, visibility, loading
state and renderer recoveries, rewritten on every change), `--navigate <0|1>=<url>` (each loads completely
before the next), `--private` (each page in its own private profile, no session kept), `--crash-page <0|1>`
(crash that page's renderer with `chrome://crash`), `--snapshot-file <png>` (a render of the window's contents
by AppKit) and `--quit-after-report`. The end-to-end tests also set `BDK_ENGINE_MOCK_KEYCHAIN=1`, so Chromium
never asks for the Keychain.

## What the end-to-end tests prove

They need a window server session (a logged-in Mac). `quitGracefully` sends the quit Apple event, so the quit
goes through `applicationShouldTerminate`. Each run keeps the application's output in the support directory.

| Test | Proves | Runtime |
|---|---|---|
| Launch smoke | The built bundle starts, loads `bdk.resolved.json`, and reports one visible window with two visible pages, each loaded with its site's title. A graceful quit exits with status 0 and the runtime shut down. | Both |
| Relaunch | After navigating both pages, a quit saves the session, and a second launch restores the same entries, current index, URL and title for both pages. | Both |
| Renderer-crash recovery | `chrome://crash` kills page 0's renderer; the application sees the renderer-gone event, reloads the page, and the page shows its site again. Page 1 is untouched, and the application quits cleanly. | Real |
| Shared profile | Two pages in one profile: a cookie, local storage and a cached image set by page 0 are seen by page 1 (the image is fetched once). The control for the next test. | Real |
| Private profiles | Two pages in two private profiles: page 1 sees no cookie and no storage from page 0, and fetches the image again (separate HTTP caches). | Real |

With the real runtime the home pages and the relaunch test load real sites (example.com, example.org,
example.net, iana.org), so they need the network; the profile tests use a server inside the test on
127.0.0.1. With the stub, the three real-runtime tests are skipped, and the others prove the application's own
behaviour and nothing about Chromium. Permissions are kept apart by construction (each private profile is its
own off-the-record profile with its own content settings) but no test grants one yet: BDK has no permission
prompt before M3 (T-UIR-02).
