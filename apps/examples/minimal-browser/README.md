# minimal-browser

A small AppKit browser built on the Swift SDK: one window, two pages side by side, each with back, forward,
reload and an address field. Task T-SWF-05; the code follows [swift-api.md section 18](../../../docs/planning/sdk/swift-api.md).

## It runs against the stub runtime

There is no real runtime yet (the engine tasks wait on M0). The example starts the in-memory stub runtime from
`BDKTestSupport`. It renders nothing and runs no JavaScript: each page shows its URL and title in the
placeholder `BDKWebView` draws when the runtime has no native view. The window says so at the top, and the
status file reports `"runtime": "stub"`.

The stub is used in one file, `Sources/MinimalBrowser/StubRuntime.swift`. It also scripts the titles of the
sites the example loads, because the stub navigates only what it is told about. Switching to the real runtime is
replacing that file's `start` with `try BDKRuntime.start(configuration)` and dropping the `BDKTestSupport`
dependency in `Package.swift`; its header lists the steps.

## What it shows

* The runtime starts from the bundled `bdk.resolved.json`, produced by the `bdk` CLI from this directory's
  `bdk.toml`. `applicationShouldTerminate` shuts the runtime down.
* A profile is opened and two pages are created in it, each shown by its `BDKWebView`.
* The window title follows the focused page's title.
* On quit, each page's `sessionState()` is saved to `session.json` in Application Support
  (`~/Library/Application Support/dev.bdk.examples.minimal-browser`). On launch, `restore` rebuilds the pages from it.

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
`bdk.resolved.json` in `Resources`. The bundle is signed ad hoc. It holds no runtime, so `bdk build sign`
(which re-signs a nested runtime) has nothing to do yet.

It is a SwiftPM package that depends on the repository's root package by relative path. It builds with
`swift build` in this directory, without Xcode.

## Layout

| Path | Contents |
|---|---|
| `Sources/MinimalBrowser` | The application: `main.swift`, `AppDelegate`, `BrowserWindow`, `BrowserPane`, and `StubRuntime` |
| `Sources/MinimalBrowserCore` | Logic with no runtime: launch options, the status report, address parsing |
| `Sources/MinimalBrowserKit` | The session store |
| `Tests/MinimalBrowserTests` | Unit tests (the stub runs in process) |
| `Tests/MinimalBrowserE2ETests` | Launches the built `.app` |

## Test flags

The tests steer the app with launch arguments, because a person launching it passes none:
`--support-dir <dir>` (where the session goes), `--status-file <file>` (a JSON report of the window and each
page's URL, title, entries and visibility, rewritten on every change), `--navigate <0|1>=<url>`,
`--snapshot-file <png>` (a render of the window's contents by AppKit) and `--quit-after-report`.

## What the end-to-end tests prove

They need a window server session (a logged-in Mac). `quitGracefully` sends the quit Apple event, so the quit
goes through `applicationShouldTerminate`.

| Test | Proves |
|---|---|
| Launch smoke | The built bundle starts, loads `bdk.resolved.json`, and reports one visible window with two visible pages, each with a navigation entry. A graceful quit exits with status 0 and the runtime shut down. |
| Relaunch | After navigating both pages, a quit saves the session, and a second launch restores the same entries, current index, URL and title for both pages. |
| Renderer-crash recovery | Skipped, recorded as not testable. The stub has no renderer to crash, and the C ABI has no process-gone event: the `BDK_CONTENTS_EVENT_*` kinds in `packages/core/include/bdk/bdk_ids.h` stop at closed. T-ABI-03 and the engine add it. The M2 exit criterion about surviving a renderer crash is therefore open. |

They do not prove that anything renders, that JavaScript runs, that Chromium restores a real session (the stub's
state is a text format of URLs and titles), or that a real runtime starts. A screenshot of the real window needs
screen-recording permission, so `--snapshot-file` draws the content view in process instead; it has no title
bar.
