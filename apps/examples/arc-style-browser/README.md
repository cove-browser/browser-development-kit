# arc-style-browser

The Arc-style reference application of milestone M8 (tasks T-ARC-02 and T-ARC-03): a sidebar, pinned pages,
two spaces with a profile each, split view of up to three pages, pages moved between panes, spaces and
windows, hidden spaces, application-owned session restoration, extension popups and side panels in the
application's own containers, a bundled settings page and an automation script over two spaces. It is a
test application, not a product. Everything it does goes through the public Swift SDK: BDK has no space,
split, tab or sidebar type and gains none from it ([amendments.md](../../../docs/planning/amendments.md)
sections 2.1 and 2.18). To BDK, each of these is page views entering and leaving windows.

## Run it

```sh
pnpm dev:arc                                  # build, then launch (nx run arc-style-browser:dev)
pnpm exec nx run arc-style-browser:build      # resolve bdk.toml, swift build, assemble .build/ArcStyleBrowser.app
pnpm exec nx run arc-style-browser:test       # the layout model and the M8 scenarios, on the stub, in process
pnpm exec nx run arc-style-browser:test-e2e   # launches the built bundle with --do actions
pnpm exec nx run arc-style-browser:lint
```

Which runtime it runs on is decided by `Package.swift`, as for the minimal browser: the packaged runtime at
`.bdk/runtime` (`pnpm chromium:runtime`), or a local engine build's frameworks directory in
`BDK_RUNTIME_FRAMEWORKS`, or else the in-memory stub (always with `BDK_RUNTIME=stub`). On the stub nothing
renders (each page shows its URL and title), the settings page does not load and the automation script does
not run; each window's subtitle and `scripts/bundle.sh` say so. To run on a local engine build (it must be built
from this branch, since the runtime refuses a framework with another ABI hash):

```sh
BDK_RUNTIME_FRAMEWORKS=~/.bdk/chromium/managed/src/out/release/Chromium.app/Contents/Frameworks \
  pnpm exec nx run arc-style-browser:build
open apps/examples/arc-style-browser/.build/ArcStyleBrowser.app
```

`scripts/bundle.sh` copies (an APFS clone) that framework into the bundle and re-signs it as this application,
so the bundle keeps working after the build directory changes. The signature is ad hoc unless `BDK_SIGN_IDENTITY`
names a signing identity (`security find-identity -v -p codesigning`; `BDK_SIGN_TEAM` is its team). An ad hoc
build is a new code identity each time, so the Keychain asks again for Chromium's storage key after every rebuild
and Always Allow lasts only until the next one; with a real identity it lasts. A request the Keychain refuses
leaves Chromium without that key, and pages then stay at `about:blank`. The session and, on the real runtime, the
engine's data are in `~/Library/Application Support/dev.bdk.examples.arc-style-browser` (`--support-dir`
moves them).

## What it shows

| Scenario (roadmap M8) | Where | What to do and what to see |
|---|---|---|
| Sidebar, pinned pages, two spaces | `SidebarView`, `Layout` | The sidebar has the spaces (Personal, Work), the space's pinned pages, its other pages and an address field for a new page. Click a space to switch; each space's pages use a profile of their own (cookies, history and extensions are per space). A row's menu pins and unpins. A row's ×, its menu's Close, or File > Close Page (⌘W) closes a page; a window whose last pane closed shows the space's next page. Close Window is ⇧⌘W. The Go menu reloads the focused page (⌘R, which also brings back a page whose renderer went away) and goes back and forward in its history (⌘[ and ⌘]); View > Next Space is ⌥⌘→. The Edit menu's cut, copy, paste and select all go to the focused page (or the address field). |
| Split view | `Layout.split`, `BrowserWindow` | Option-click a page row, a row's "Open in Split View", or View > Split with Next Page (⌘D): up to three pages side by side. Every pane on screen gets its own dialogs, permission prompts, find bar and translation offer (T-ADP-06); extensions see the focused pane as the window's active tab and the others as background tabs (risk R2). |
| Move and reparent | `Layout.move`, `BrowserController.apply` | A row's "Move to Work" or "Move to New Window". The page keeps its history, its renderer (a form you typed into keeps its text), its extension tab id and an extension's `activeTab` grant, because the same view moves; BDK moves its tab. |
| Hide and restore | `Layout.switchSpace` | Switch to the other space: the first space's pages leave the window, report hidden and keep running (a timer or a media page keeps going) as background tabs of that window. Switching back shows them as they were, without a reload. On Chromium 158 the pane that had focus, and a page never shown, still report visible (T-ADP-11). |
| Session restoration | `SessionFile`, `PageRegistry` | Quit and relaunch: the spaces, pinned pages, windows, splits and each page's history come back from `session.json`, which the application writes on quit. Chromium restores nothing by itself. |
| New pages from links | `PagePlacement`, `BrowserController.adopt` | A `target="_blank"` link, "Open link in new page", `window.open` without features or an extension's options page: the new page joins the opener's space in the sidebar and takes the opener's pane (a middle click leaves it in the sidebar, Shift-click opens a window). It keeps `window.opener`. A page that closes itself (`window.close()`) leaves the sidebar and its opener comes back to the pane. A popup (`window.open` with features, as sign-in pages use) opens in BDK's own window, which closes with it. |
| Extensions in the layout | `ExtensionPlacement` | File > Install Unpacked Extension… (BDK's install prompt asks). The extension's action appears at the bottom of the sidebar. Its popup opens in a popover beside that button, and `chrome.sidePanel.open` puts its panel on the window's trailing edge with a Close button; both are BDK's secure views in containers the application chose. |
| Internal pages | `Pages/Settings`, `bdk.toml` | File > Settings (⌘,) opens `arc://settings` with the grants `settings.read` and `settings.write`. It shows its route and capabilities and a "Do Not Track" box that writes `privacy.doNotTrack`. |
| Automation | `Automation/spaces.ts`, `AutomationRunner` | Tools > Run Automation over Two Spaces runs the bundled script with a session scoped to one page of each space and the two profiles; it lists, reads and scripts just those pages. Its result is in the status report. Sign in to a site in one space only, and the script reports `signedIn` for that page alone. |

The extension fixtures in `tests/extensions/fixtures` (`ui-action-popup`, `panel-sidepanel`, `ui-action-click`)
are ready to install for the popup, side-panel and `activeTab` checks. Extensions also install from the Chrome Web Store (`stores = ["chrome-web-store"]` in `bdk.toml`, decision D4), whose button reads "Add to Arc-style Browser".

## Layout

| Path | Contents |
|---|---|
| `Sources/ArcBrowserCore` | No BDK: the layout model (`Layout`, its operations and rules), `SessionFile`, `AppOptions`, `AppStatus`, `AddressInput` (the minimal browser's rule; each example is a package of its own) |
| `Sources/ArcBrowserKit` | The application over the SDK: `BrowserController` (applies the layout to windows), `PageRegistry`, `BrowserWindow`, `SidebarView`, `ExtensionPlacement`, `AutomationRunner` |
| `Sources/ArcBrowser` | The executable: `main.swift`, `AppDelegate`, `MainMenu` and `RuntimeSource`, the one file that knows which runtime it runs on |
| `Pages/Settings` | The bundled settings page (no build step) |
| `Automation/spaces.ts` | The automation script, bundled by `bdk build automation` with `@bdk/automation` inlined and typed from the generated declarations (T-JS-06) |
| `Tests/ArcBrowserTests` | `LayoutTests` (the model) and `ScenarioTests` (each M8 scenario as the application performs it, on the stub) |
| `Tests/ArcBrowserE2ETests` | Launches the built bundle with `--do` actions and reads its status file |

## Test flags

`--support-dir <dir>`, `--status-file <file>` (an `AppStatus` JSON report: windows, each page's space, pane,
whether it is on screen, the runtime's visibility, entries, the settings URL and the automation result),
`--do <action>` (repeatable: `open=<url>`, `split-next`, `switch-space=<name>`, `move-to-space=<name>`,
`new-window=<space>`, `focus-window=<i>`, `move-to-window=<i>`, `pin`, `close-page`, `settings`, `automation`,
`install-extension=<dir>`), `--fresh` (ignore the saved session), `--quit-after-actions` and `--background`
(no activation; windows open behind the others, so a test run does not take the screen).

## What is tested where

| Scenario | In CI (stub, `arc-style-browser:test`) | On the engine (`tests_engine_arc.c`, run by hand) | End to end (`test-e2e`, by hand) |
|---|---|---|---|
| Spaces and pinned pages | `ScenarioTests.spaces`, `ScenarioTests.closePage`, `LayoutTests` | | `launchSmoke`, `closePage` |
| Split view | `ScenarioTests.splitView` | `engine_arc_every_visible_pane_is_foreground_for_dialogs_permissions_find_and_offers`, `engine_arc_extensions_see_the_focused_pane_as_the_active_tab` | `layoutActions` |
| Move and reparent | `ScenarioTests.movePages` | `engine_arc_a_page_moved_between_panes_and_windows_keeps_its_tab_grant_and_renderer` | `layoutActions` |
| Hide and restore | `ScenarioTests.hideAndRestore` | `engine_arc_a_page_in_a_hidden_space_keeps_running_and_comes_back` | `launchSmoke` |
| Session restoration | `ScenarioTests.sessionRestoration` | `engine_arc_after_a_relaunch_chromium_restores_nothing_and_the_application_does` | `relaunch` |
| Extensions in the layout | `ScenarioTests.extensionsInTheLayout` | the M5 popup and side-panel scenarios (`tests_engine_ext_ui.c`, `tests_engine_ext_panels.c`) | |
| New pages from links | `ScenarioTests.linksOpenNewPages`, `ScenarioTests.pageClosesItself` | `engine_a_defaulted_new_window_opens_in_the_default_presenter` (BDK's popup window) | |
| Internal pages | `ScenarioTests.settingsPage` | `tests_engine_pages.c` | `settings` (real runtime) |
| Automation | `ScenarioTests.automation` | `engine_arc_a_script_drives_pages_in_two_spaces_one_signed_in` | `automation` (real runtime) |

The stub cannot show what Chromium makes of a layout, so the engine scenarios repeat each one with the same
view moves in windows the harness registers. CI never builds Chromium; those run on a developer's Mac
(`packages/core/scripts/test-engine.sh`).
