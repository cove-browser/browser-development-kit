# BDK service commands

Status: proposal, 2026-11-15. The broker is defined in [../architecture.md](../architecture.md) section 9. This document lists how a call behaves, every capability, every command and event, and the settings catalogue. Names are placeholders; `settings.get`, `settings.set` and the capability names in the spec are kept. Status labels are in [README.md](README.md) section 2. Tasks: T-ABI-03, T-SVC-01 to T-SVC-06.

The catalogue is the planned content of `packages/core/interface/`. The M3 namespaces are there (2026-11-19): where this document and the definitions differ, the definitions are what is built, and this file should be fixed. The Swift wrappers, the TypeScript declarations, the stub runtime and the capability table are all generated from it, so a command exists in every surface or in none.

## 1. One definition, three callers

| Caller | Reaches the broker through | Checked against |
|---|---|---|
| Swift | The C ABI, in process | Nothing. Full authority. |
| Internal page | A Mojo interface bound to that page | The capabilities granted to its logical page in `bdk.toml` |
| Automation session | The automation host, or the authenticated local channel | The session's capability set and scope |

Extensions and ordinary web pages are not callers. They never receive a broker interface (invariants 2 and 5).

## 2. Call semantics

| Topic | Rule |
|---|---|
| Request | A command name, one arguments object, and a call identifier. The profile is implied by the caller (section 3) unless the caller is Swift. |
| Result | One value, or an error with domain, code, message and details |
| Check order | Caller identity, then capability, then scope, then gesture or authentication requirement, then argument validation, then the service. A caller without the capability learns nothing about whether the arguments were valid. Implemented in T-SVC-01; the details (the `capability` error codes, `authenticateWhen`, what a page's gesture is) are in `packages/core/docs/conventions.md`, "The broker". |
| Unknown command | `broker.unknownCommand`. Never silently ignored. |
| Cancellation | Every call can be cancelled. A cancelled call returns `broker.cancelled` and the service stops work where it can. |
| Progress | A command marked "progress" reports fraction and message while it runs |
| Streams | A command marked "stream" delivers a sequence of results until it completes or is cancelled |
| Pagination | List commands take `limit` and `cursor` and return `items` and `nextCursor`. A cursor is opaque and valid only for the same arguments. |
| Limits | Arguments and results are each capped at 4 MiB (4194304 bytes) of JSON, set in T-SVC-01; more is `broker.payloadTooLarge`. Larger data, such as a screenshot, a PDF or an export, is a binary result outside the JSON value, or a file the caller names. |
| Ordering | Calls from one caller start in the order sent. Results arrive when ready. |
| Timeouts | None imposed by the broker. The caller cancels. |
| Teardown | When a page navigates or closes, or a session is revoked, its outstanding calls are cancelled and its subscriptions end. |
| Re-check | A page's committed origin is checked on every call, not only at bind time. |
| Renderer misbehaviour | A page that asks for the broker interface without being entitled to it is terminated, as Chromium does for any unregistered interface. A page that holds the interface and calls a command it lacks the capability for gets `capability.denied`; that is a programming error, not an attack. |

Marks used in the command tables:

| Mark | Meaning |
|---|---|
| G | From a page, the call must come from a user gesture in that page. From automation, the session must hold the capability; there is no gesture. |
| A | Needs operating-system authentication (Touch ID or the login password) at the time of the call, whoever calls. |
| U | The command raises a UI request ([ui-requests.md](ui-requests.md)), which Swift or the default presenter answers. |
| P | Reports progress. |
| S | Streams. |
| N | Not available to internal pages, whatever their grants. |

## 3. Callers, profiles and scope

| Caller | Profile | Scope |
|---|---|---|
| Swift | Named in the call | Everything |
| Internal page | Always the profile the page is in. A page cannot name another profile. | Its own profile's data. It cannot address other pages. |
| Automation session | One of the profiles in the session's scope, named in the call or defaulted when there is one | The pages in its scope (Added, S10): all pages in those profiles, only pages the session created, or an explicit list the application maintains |

Private profiles follow Chromium. A command in a private profile reads history and bookmarks from the parent, writes bookmarks to the parent, records no history, and keeps its own cookies, permissions and downloads list.

`profiles.*` commands are the exception to "a page sees only its own profile": a page that holds `profiles.read` can list profiles by name, because a profile switcher needs to.

## 4. Capabilities

A capability is a name. A page gets a list of them in `bdk.toml`; a session gets a list from Swift. Reading never implies writing.

| Capability | Allows | Pages | Sessions |
|---|---|---|---|
| `runtime.read` | Versions and build information | Yes | Yes |
| `profiles.read` | List profiles; describe the current one | Yes | Yes |
| `profiles.manage` | Create, rename, delete | Yes | Yes |
| `settings.read` | Read browser settings and exposed application settings | Yes | Yes |
| `settings.write` | Change them | Yes | Yes |
| `history.read` | Search and read visits | Yes | Yes |
| `history.write` | Delete entries and ranges | Yes | Yes |
| `bookmarks.read` | Read the tree, search | Yes | Yes |
| `bookmarks.write` | Create, change, move, remove, import | Yes | Yes |
| `downloads.read` | List downloads and their state | Yes | Yes |
| `downloads.manage` | Start, pause, resume, cancel, remove, open, reveal | Yes | Yes |
| `permissions.read` | Read site settings | Yes | Yes |
| `permissions.manage` | Change site settings and defaults | Yes | Yes |
| `cookies.read` | List cookies and site storage, without values | Yes | Yes |
| `cookies.manage` | Remove cookies and site data | Yes | Yes |
| `cookies.values` | Read and set cookie values | No | Yes |
| `privacy.clear` | Clear browsing data | Yes | Yes |
| `passwords.read` | List saved logins, without passwords | Yes | Yes |
| `passwords.manage` | Add, change, remove, import; start a leak check and dismiss its flags | Yes | Yes |
| `passwords.reveal` | Read a password in clear; export. Always also needs operating-system authentication. | Yes | No |
| `autofill.read` | List saved addresses and masked cards | Yes | Yes |
| `autofill.manage` | Add, change, remove | Yes | Yes |
| `extensions.list` | List extensions and their state | Yes | Yes |
| `extensions.manage` | Install, remove, enable, disable, change site access | Yes | Yes |
| `translation.read` | Languages, preferences, model state | Yes | Yes |
| `translation.manage` | Change preferences; download and remove models | Yes | Yes |
| `search.read` | List engines; classify input; get suggestions | Yes | Yes |
| `search.manage` | Add, change, remove engines; set the default | Yes | Yes |
| `favicons.read` | Favicon for a page URL | Yes | Yes |
| `top-sites.read` | Most-visited list | Yes | Yes |
| `system.default-browser` | Read and request default-browser status | Yes | Yes |
| `contents.read` | List pages in scope; read URL, title, state; text and accessibility snapshots | No | Yes |
| `contents.control` | Create, close, navigate, reload, wait | No | Yes |
| `contents.script` | Evaluate script, query the DOM, inject CSS, register user scripts, synthesise input | No | Yes |
| `contents.capture` | Screenshots and PDFs | No | Yes |
| `contents.debug` | The DevTools protocol | No | Yes |
| `app.<name>` | One application command or event | Yes | Yes |
| `app.<prefix>.*` | Every application command and event under the prefix | Yes | Yes |

Why some are refused to pages: a `contents.*` capability would let one internal page read and drive every other page, including ones signed in to the user's accounts. A page that needs "open this link" calls an application command, and the application decides. Why `passwords.reveal` is refused to sessions: an agent that can read stored passwords in clear is not something BDK should make possible by a capability string; an application that wants it writes an `app.` command and owns the consequence.

There is no wildcard except under `app.`. There is no "all" capability.

## 5. Commands

Every command is available to Swift. "Capability" is what a page or session needs.

### 5.1 `runtime` (Plan, M3)

| Command | Capability | Does |
|---|---|---|
| `runtime.info` | `runtime.read` | BDK version, Chromium version, browser name, internal scheme, platform |
| `runtime.caller` | none | Who the caller is: logical page or session name, profile, and its capability list. Lets a page hide controls it cannot use. |

### 5.2 `profiles` (Plan, M3)

| Command | Capability | Marks | Does |
|---|---|---|---|
| `profiles.list` | `profiles.read` | | Identifier, name, last used |
| `profiles.current` | `profiles.read` | | The caller's profile, and whether it is private |
| `profiles.create` | `profiles.manage` | | Create a persistent profile |
| `profiles.rename` | `profiles.manage` | | |
| `profiles.delete` | `profiles.manage` | G | Refused while pages exist in it |

Events: `profiles.created`, `profiles.renamed`, `profiles.deleted`.

### 5.3 `settings` (Plan, M3, T-SVC-03)

| Command | Capability | Does |
|---|---|---|
| `settings.get` | `settings.read` | One value by key |
| `settings.getMany` | `settings.read` | Several values |
| `settings.describe` | `settings.read` | Type, default, allowed values, scope, and whether the product default or the user set it |
| `settings.set` | `settings.write` | Set a value. Validated against the key's type. |
| `settings.reset` | `settings.write` | Back to the product default |

Events: `settings.changed` with key, old and new value. Keys are in section 6.

### 5.4 `history` (Plan, M3, T-SVC-02)

| Command | Capability | Marks | Does |
|---|---|---|---|
| `history.search` | `history.read` | | Text and time range; returns URL, title, last visit, visit count, typed count. Paginated. |
| `history.visits` | `history.read` | | Individual visits for one URL, with transition and referrer |
| `history.add` | `history.write` | | Add a visit, for import and sync |
| `history.deleteUrls` | `history.write` | | |
| `history.deleteRange` | `history.write` | | |
| `history.deleteAll` | `history.write` | G | |

Events: `history.visited`, `history.removed`.

There is no "recently closed" command. A list of closed pages is the application's, because BDK owns no tabs or windows; the application keeps the session state of pages it closes ([swift-api.md](swift-api.md) 6.8).

### 5.5 `bookmarks` (Plan, M3, T-SVC-02)

| Command | Capability | Marks | Does |
|---|---|---|---|
| `bookmarks.roots` | `bookmarks.read` | | The permanent folders: bar, other |
| `bookmarks.get` | `bookmarks.read` | | One node |
| `bookmarks.children` | `bookmarks.read` | | Children of a folder |
| `bookmarks.subtree` | `bookmarks.read` | | A folder and everything under it |
| `bookmarks.search` | `bookmarks.read` | | By title and URL |
| `bookmarks.forUrl` | `bookmarks.read` | | Nodes that point at a URL. "Is this page bookmarked?" |
| `bookmarks.create` | `bookmarks.write` | | A bookmark in a folder at an index |
| `bookmarks.createFolder` | `bookmarks.write` | | |
| `bookmarks.update` | `bookmarks.write` | | Title or URL |
| `bookmarks.move` | `bookmarks.write` | | To another folder or index. Cycles are refused. |
| `bookmarks.remove` | `bookmarks.write` | | A node; a folder is removed with its contents |
| `bookmarks.import` | `bookmarks.write` | P | From a Netscape-format HTML file |
| `bookmarks.export` | `bookmarks.read` | | To a Netscape-format HTML file |

Events: `bookmarks.created`, `bookmarks.changed`, `bookmarks.moved`, `bookmarks.removed`, `bookmarks.reordered`.

### 5.6 `downloads` (Plan, M3, T-SVC-02)

| Command | Capability | Marks | Does |
|---|---|---|---|
| `downloads.list` | `downloads.read` | | Paginated; filter by state and text |
| `downloads.get` | `downloads.read` | | URL, file path, state, bytes, total, speed, danger, MIME type, start and end time |
| `downloads.start` | `downloads.manage` | U | Download a URL. Goes through the downloads surface like any other. |
| `downloads.pause`, `downloads.resume`, `downloads.cancel`, `downloads.retry` | `downloads.manage` | | |
| `downloads.remove` | `downloads.manage` | | From the list; the file stays |
| `downloads.deleteFile` | `downloads.manage` | G | |
| `downloads.open` | `downloads.manage` | G | Open the file with its default application. Dangerous types are refused, as in Chromium. |
| `downloads.reveal` | `downloads.manage` | | Show in Finder |
| `downloads.clear` | `downloads.manage` | | Remove finished entries from the list |

Keeping a dangerous download is not a command. It is a UI request completion that needs a gesture.

Events: `downloads.created`, `downloads.updated`, `downloads.completed`, `downloads.interrupted`, `downloads.removed`.

### 5.7 `permissions`: site settings (Plan, M3, T-SVC-03)

| Command | Capability | Does |
|---|---|---|
| `permissions.kinds` | `permissions.read` | Every content-setting kind the runtime knows, with its allowed values and default |
| `permissions.get` | `permissions.read` | The value for one origin and kind, and where it came from: default, user, extension, embargo |
| `permissions.forOrigin` | `permissions.read` | Everything set for one origin |
| `permissions.exceptions` | `permissions.read` | Every origin with a non-default value for one kind |
| `permissions.set` | `permissions.manage` | Allow, block or ask for one origin and kind |
| `permissions.reset` | `permissions.manage` | One origin and kind, or everything for an origin |
| `permissions.getDefault`, `permissions.setDefault` | read, manage | The default for a kind |
| `permissions.certificateExceptions` | `permissions.read` | Hosts where the user proceeded past a certificate error |
| `permissions.revokeCertificateException` | `permissions.manage` | |

Kinds include camera, microphone, geolocation, notifications, pop-ups, JavaScript, images, cookies, sound, automatic downloads, clipboard, MIDI, USB, Bluetooth, HID, serial, file-system access, protocol handlers, window management and the rest of Chromium's set at the pin.

Events: `permissions.changed`.

### 5.8 `cookies` and `privacy` (Plan, M3, T-SVC-03)

| Command | Capability | Marks | Does |
|---|---|---|---|
| `cookies.sites` | `cookies.read` | | Sites with stored data: cookie count and storage size. Paginated. |
| `cookies.list` | `cookies.read` | | Cookies for a site: name, domain, path, expiry, flags. No values. |
| `cookies.getValues` | `cookies.values` | N | With values |
| `cookies.set` | `cookies.values` | N | |
| `cookies.remove` | `cookies.manage` | | One cookie |
| `cookies.removeSiteData` | `cookies.manage` | | Cookies and all storage for a site |
| `privacy.clear` | `privacy.clear` | G P | Data types (history, cookies, cache, downloads list, passwords, autofill, site settings, hosted app data), a time range, and an optional origin filter |
| `privacy.estimate` | `privacy.clear` | | What a clear would remove, by type |

Events: `cookies.changed` (session callers with `cookies.values` only), `privacy.cleared`.

Built (M3, T-SVC-07; `cookies_service.cc`): `cookies.changed` comes from a change listener on the profile's default storage partition cookie manager. The payload is `{cookie, removed, source}` with `cookie` a `CookieValue` (the value included, which is why the event needs `cookies.values`), `removed` true when the cookie was deleted (explicitly, by expiry or by eviction) and false when it was inserted or replaced. Replacing a cookie is one event with `removed` false, not a removal followed by an insertion; an overwrite that changed nothing is not reported. The network service does not say who changed a cookie, so `source` is always `application`. The engine emits to subscriptions of the profile; delivering the event to session callers only is the broker's job when sessions can subscribe (it checks the event's `cookies.values` capability).

### 5.9 `passwords` (Plan, M3, T-SVC-04)

| Command | Capability | Marks | Does |
|---|---|---|---|
| `passwords.list` | `passwords.read` | | Identifier, origin, username, created and last-used dates. Never the password. |
| `passwords.forOrigin` | `passwords.read` | | The same, for one origin |
| `passwords.reveal` | `passwords.reveal` | A N for sessions | The password in clear |
| `passwords.add` | `passwords.manage` | | |
| `passwords.update` | `passwords.manage` | A when the password changes | |
| `passwords.remove` | `passwords.manage` | | |
| `passwords.neverSaved` | `passwords.read` | | Sites where the user chose "never" |
| `passwords.removeNeverSaved` | `passwords.manage` | | |
| `passwords.export` | `passwords.reveal` | A G | To a CSV file the user chose |
| `passwords.import` | `passwords.manage` | P | From a CSV file |
| `passwords.check` | `passwords.manage` | A P | Added (S16). Checks every saved login with the leak-check provider and flags the ones it reports as leaked |
| `passwords.issues` | `passwords.read` | | Added (S16). Flagged logins: identifier, origin, username, when it was flagged, whether it is dismissed. Never the password. |
| `passwords.dismissIssue`, `passwords.restoreIssue` | `passwords.manage` | | Added (S16). Hides a flag the user chose to ignore, or shows it again |

Leak check (Added, S16). Chromium's own leak detection and password check-up call Google and are removed. A check exists only when the application supplies one, with `[services.leak_check] provider = "app"` ([configuration.md](configuration.md) 3.8):

| Topic | Rule |
|---|---|
| No provider | With `provider = "none"`, the default, `passwords.check` returns `provider.unavailable`, `passwords.issues` is empty, and nothing is checked or sent. |
| After a sign-in | When `passwords.leakCheck.enabled` is on, the username and password the user just submitted are given to the provider. A leaked result raises the warning on the `passwords` surface ([ui-requests.md](ui-requests.md) 5.8) and flags the login if it is saved. No authentication is needed, because the user has just typed the password. |
| Of every saved login | `passwords.check` hands each stored password, in clear, to the provider. That is the exposure `passwords.reveal` guards, so the command needs operating-system authentication, whoever calls. |
| What leaves the device | Decided by the provider, not by BDK. The provider's privacy descriptor states it and default UI shows it. BDK passes the provider nothing it does not need: one origin, username and password per call. |
| Failure | A provider error flags nothing and is reported in the command's result. An earlier flag is not cleared by a failed check. |
| Flags | Cleared when the password changes or the login is removed. Kept in Chromium's store beside the login. Where they live with `[services.credentials] provider = "app"` is not settled. |

A local weak-and-reused check is Later. It needs no provider and would add its results to `passwords.issues`.

Storage: encrypted with a key held in the Keychain item named in `[storage]` ([configuration.md](configuration.md) 3.2). With `[services.credentials] provider = "app"` (Added, S14) these commands read and write the application's store instead, and `passwords.reveal` still needs operating-system authentication.

Events: `passwords.changed`, `passwords.issuesChanged` (Added, S16).

Built (M3, T-SVC-04; `chromium/bdk/engine/services/passwords_service.cc`). Shapes: `passwords.list` and `passwords.forOrigin` answer `{items: [{id, origin, username, created, lastUsed}]}`; `created` and `lastUsed` are milliseconds since the Unix epoch, `lastUsed` null when never used; `origin` is the serialised origin with a trailing slash. `id` is the store's key as a decimal string. It is stable while the login is edited in place (a new password) but changes when the username changes, because the username is part of the store's key; `passwords.update` answers with the login under its new `id`. `passwords.add` takes an http or https origin and stores a manually added login in the profile store. `passwords.reveal` answers `{password}`; it and an update that sets `password` ask the operating system through `Backend::Authenticate` (LocalAuthentication: Touch ID or the account password), and a refusal is `capability.reauthenticationFailed`. `passwords.neverSaved` answers `{origins}`. `passwords.export` writes a CSV (`name,url,username,password,note`) and `passwords.import` reads one by column name (`url`, `username`, `password`) and answers `{imported}`. With no leak-check provider `passwords.check` fails with `provider.unavailable`, `passwords.issues` is empty, and `dismissIssue` and `restoreIssue` fail with `broker.invalidArguments`. A private profile has no password store: its commands fail with `broker.unavailable`. `passwords.changed` is `{id, change, source}`: one event per login the store reports as added, updated or removed (`change` is `added`, `updated` or `removed`; `id` as in `passwords.list`; `source` is `application`; never a password). A username change is a removal and an addition, because the username is part of the key. Sites the user chose never to save are not reported. `passwords.issuesChanged` (payload `{}`) is emitted when a store change touches the insecure-credential flags of a login. Without a leak-check provider nothing sets those flags, so it cannot fire until the provider exists (M6); the observer is in place and no command-level scenario can drive it. Passwords are encrypted by Chromium's OS crypt under the application's Keychain item (`[storage] keychain_item`, set at start-up from the resolved configuration; `BDK_ENGINE_MOCK_KEYCHAIN=1` in tests); a scenario checks they survive a relaunch of the runtime.

### 5.10 `autofill` (Plan, M3, T-SVC-04)

| Command | Capability | Marks | Does |
|---|---|---|---|
| `autofill.addresses.list`, `.add`, `.update`, `.remove` | read, manage | | Saved addresses |
| `autofill.cards.list` | `autofill.read` | | Card holder, network, last four digits, expiry |
| `autofill.cards.add`, `.update`, `.remove` | `autofill.manage` | | |
| `autofill.cards.reveal` | `passwords.reveal` | A N for sessions | The full number |

Server-side card storage and payment-account features are Google services and are excluded.

Events: `autofill.changed`.

Built (M3, T-SVC-04; `autofill_service.cc`). Addresses map `AddressFields` onto Chromium's address model (`fullName` to the full name, `street` to the street address, `region` to the state, `postalCode` to the ZIP, `country` to the country, `phone` to the whole phone number); a new address is a local profile with the country code `US` until its fields say otherwise. Cards are local cards (server cards and Google payments are excluded): `autofill.cards.list` answers holder, network, `lastFour`, `expiryMonth`, `expiryYear` and never the number; `autofill.cards.reveal` answers `{number}` after operating-system authentication. Chromium writes this data asynchronously; a command answers when the change shows. A private profile has no autofill data (`broker.unavailable`). `autofill.changed` is `{kind, id, source}`: Chromium's data managers report only that their data changed once pending writes finished, not which entry, so `kind` is `address` or `card`, `id` is the empty string and `source` is `application`; the event carries no field values. It fires for changes made by any means (the commands, a saved form, sync), once per batch of writes.

### 5.11 `extensions`

The commands are in [extensions.md](extensions.md) section 3. Capabilities: `extensions.list` and `extensions.manage`.

### 5.12 `translation` (Plan, M6, T-PRV-03)

| Command | Capability | Marks | Does |
|---|---|---|---|
| `translation.provider` | `translation.read` | | Name and privacy descriptor of the configured provider |
| `translation.languages` | `translation.read` | | Supported pairs, and which have a model installed |
| `translation.preferences` | `translation.read` | | Target language, always-translate and never-translate lists, never-for-site list, whether offers are shown |
| `translation.setPreferences` | `translation.manage` | | |
| `translation.downloadModel` | `translation.manage` | P | |
| `translation.removeModel` | `translation.manage` | | |
| `translation.state` | `contents.read` | N | State of one page: unavailable, available with detected language, translating, translated, failed |
| `translation.translate`, `translation.revert` | `contents.control` | N | Translate or restore one page |

A settings page manages preferences and models. Translating a page is done from Swift, by the default presenter, or by a session.

Events: `translation.preferencesChanged`, `translation.modelsChanged`, `translation.stateChanged` (sessions and Swift).

### 5.13 `search`: address-bar services (Added, S1)

| Command | Capability | Marks | Does |
|---|---|---|---|
| `search.engines` | `search.read` | | Identifier, name, keyword, URLs, whether it is the default, whether the product or the user added it |
| `search.addEngine`, `search.updateEngine`, `search.removeEngine` | `search.manage` | | |
| `search.setDefault` | `search.manage` | G | |
| `search.classify` | `search.read` | | Decide whether typed text is a URL or a query, and return the URL to load. The same rules Chromium's address bar uses. |
| `search.suggestions` | `search.read` | S | For partial input: matches from history, bookmarks, open pages in scope, keyword engines, extension keywords, and, if the user allowed it, the default engine's suggest service. Each has a type, a display text, a destination URL, a relevance, and whether it may be autocompleted inline. |
| `search.deleteSuggestion` | `history.write` | | Remove a history-backed suggestion |

Rules:

- Nothing is sent to a search engine while the user types unless the `search.suggestions` setting is on. It is off by default.
- No engine is compiled in. The first-run set comes from `[search]` in `bdk.toml`.
- A caller that passes `.typed` as the transition when it navigates is what makes typed-count ranking work.

### 5.14 `favicons` and `topSites` (Added, S12)

| Command | Capability | Does |
|---|---|---|
| `favicons.get` | `favicons.read` | The stored favicon for a page URL at a requested size, as an image. Never fetches from the network on behalf of the caller. |
| `topSites.list` | `top-sites.read` | Most-visited URLs with titles |
| `topSites.exclude`, `topSites.restore` | `history.write` | Hide one from the list |

Events: `topSites.changed`.

### 5.15 `contents`: pages (Plan, M7, T-AUT-03; written in M7; passes on the stub, engine build and test pending (T-AUT-06))

For Swift and automation sessions. Never for internal pages. Swift normally uses the typed objects in [swift-api.md](swift-api.md) section 6, which are the same operations.

| Command | Capability | Marks | Does |
|---|---|---|---|
| `contents.list` | `contents.read` | | Pages in scope: identifier, URL, title, profile, visibility, whether loading, whether crashed |
| `contents.get` | `contents.read` | | One page, with security, audio and capture state |
| `contents.create` | `contents.control` | | A new page in a profile in scope. It is not on screen until the application attaches it. |
| `contents.close` | `contents.control` | | |
| `contents.navigate` | `contents.control` | | Returns when committed |
| `contents.reload`, `contents.stop`, `contents.goBack`, `contents.goForward` | `contents.control` | | |
| `contents.waitFor` | `contents.read` | | Load finished, network idle, a selector present, a URL pattern, or a script predicate; with a deadline |
| `contents.frames` | `contents.read` | | Frame tree |
| `contents.text` | `contents.read` | | Visible text of a frame |
| `contents.snapshot` | `contents.read` | | DOM or accessibility tree, as structured data |
| `contents.evaluate` | `contents.script` | | Script in the page world or a named isolated world, in a chosen frame |
| `contents.query` | `contents.script` | | Elements matching a selector: tag, attributes, text, box |
| `contents.insertCSS`, `contents.removeCSS` | `contents.script` | | |
| `contents.click`, `contents.type`, `contents.press`, `contents.scroll`, `contents.hover` | `contents.script` | | Synthesised input, targeted by selector or coordinates |
| `contents.screenshot` | `contents.capture` | | Visible area, full page or a rectangle. Binary result. |
| `contents.pdf` | `contents.capture` | P | Binary result |
| `contents.setMuted`, `contents.setZoom` | `contents.control` | | |
| `contents.requestAttention` | `contents.control` | | Ask the application to show the page. The application receives an event and decides. BDK moves no view. |

Rules:

- A session cannot script an internal page. `contents.evaluate` on one returns `contents.internalPage`.
- A session cannot answer a UI request. A permission prompt raised by a page a session is driving waits for the user, or is denied if the application's handler says so.
- Every `contents.*` call by a session emits an audit event to Swift.

Events: `contents.created`, `contents.closed`, `contents.navigated`, `contents.loadingChanged`, `contents.titleChanged`, `contents.crashed`, `contents.dialogOpened` (so a script knows it is blocked on a dialog it cannot answer), and `contents.attentionRequested`.

As built in M7 (the definitions are `interface/contents.json`; the rules are `packages/core/docs/conventions.md`, "Automation"): the page a command addresses is the invoke options' `contents` (a call frame's `contents`), never an argument; the runtime core answers `list`, `get`, `create`, `close`, `navigate`, the history moves, `waitFor` on `load` and `url`, `setMuted`, `setZoom` and `requestAttention`, the engine the rest. `contents.save` writes `htmlOnly`, `completePage` or `singleFile` (MHTML) to a path the caller names and appears in the profile's downloads. `htmlOnly` fetches the page again, as Chrome does; when it cannot be fetched again, the save fails with `platform.failed` rather than answering for a file Chromium did not write. `screenshot` and `pdf` answer `{mimeType, byteLength, path?}` with the bytes outside the JSON, or write the file the caller names; PDF sizes and margins are in inches. A page created by a session belongs to the application, which is told by `contents.created` with `createdBy`. Errors added: `contents.internalPage`, `contents.scriptError`, `contents.notSerializable`, `contents.noElement`, `broker.timeout`.

### 5.16 `scripts`: persistent user scripts (Plan, M7, T-AUT-03; written in M7; passes on the stub, engine build and test pending (T-AUT-06))

| Command | Capability | Marks | Does |
|---|---|---|---|
| `scripts.register` | `contents.script` | N | Match patterns, run-at, world, all-frames, JavaScript and CSS. Per profile. |
| `scripts.unregister` | `contents.script` | N | |
| `scripts.list` | `contents.read` | N | |

Scripts registered by a session are removed when the session is revoked. As built: kept by the runtime core in memory for the runtime's life (not across a relaunch); `runAt` defaults to `documentIdle` and `world` to an isolated world named like the id; a session may replace or remove only its own.

### 5.17 `debug`: the DevTools protocol (Plan, M7, T-AUT-03; written in M7; passes on the stub, engine build and test pending (T-AUT-06))

| Command | Capability | Marks | Does |
|---|---|---|---|
| `debug.attach` | `contents.debug` | N | Open a protocol session on a page in scope |
| `debug.send` | `contents.debug` | N | One protocol method |
| `debug.detach` | `contents.debug` | N | |

Events: `debug.event`, `debug.detached`.

`contents.debug` is close to full control of a page. The protocol domains that reach beyond a page, such as `Browser`, `Target` for other pages, and `SystemInfo`, are refused for sessions. Internal pages cannot be attached to. As built, the refused domains are Browser, Target, SystemInfo, Tethering, Storage, IO, ServiceWorker and BackgroundService; `[devtools] protocol = false` refuses `debug.*` to every caller (`devtools.protocolDisabled`); events go only to the caller that attached. Swift-only `devtools.open`, `close` and `state` with `devtools.stateChanged` drive the front end (`interface/devtools.json`).

### 5.18 `system` (Plan, M3, T-SVC-06)

| Command | Capability | Marks | Does |
|---|---|---|---|
| `system.defaultBrowser.status` | `system.default-browser` | | Whether this application is the default handler for `http` and `https`, and whether it can be |
| `system.defaultBrowser.request` | `system.default-browser` | G | Ask macOS to make it the default. macOS shows its own confirmation. |

### 5.19 `app`: application commands

Registered from Swift before start ([swift-api.md](swift-api.md) section 12). A command `app.tabs.open` needs the capability `app.tabs.open` or a wildcard that covers it. The broker applies the same check order, cancellation, progress and teardown rules as for its own commands.

## 6. Settings catalogue

Planned keys for the `settings.*` commands, `[settings.defaults]` and `@bdk/internal/settings`. Each maps to one Chromium preference or content-setting default. The complete list, with types, is generated from the interface definition; this is the first set. Scope is P for per-profile and A for the whole application.

| Key | Type | Scope | Controls |
|---|---|---|---|
| `appearance.fontSize`, `appearance.minimumFontSize` | number | P | Default and minimum page font size |
| `appearance.fonts.standard`, `.serif`, `.sansSerif`, `.fixed` | string | P | Font families |
| `appearance.pageZoom` | number | P | Default zoom |
| `appearance.colorScheme` | `system`, `light`, `dark` | P | What pages see for `prefers-color-scheme` |
| `languages.accept` | array of string | P | `Accept-Language` |
| `languages.spellcheck.enabled`, `languages.spellcheck.languages` | boolean, array | P | Spell checking |
| `search.suggestions` | boolean | P | Send typed text to the default engine for suggestions |
| `downloads.directory` | path | P | Default download location |
| `downloads.askWhereToSave` | boolean | P | Prompt for a location each time |
| `privacy.cookies.thirdParty` | `allow`, `blockInPrivate`, `block` | P | Third-party cookies |
| `privacy.doNotTrack` | boolean | P | `DNT` header |
| `privacy.globalPrivacyControl` | boolean | P | `Sec-GPC` header |
| `privacy.clearOnExit` | array of data types | P | Data cleared when the last page of the profile closes |
| `privacy.webrtcIPHandling` | enum | P | Which addresses WebRTC may expose |
| `privacy.preloading` | `off`, `standard` | P | Speculative loading |
| `security.httpsOnly` | boolean | P | Upgrade navigations and warn before HTTP |
| `security.reputation.enabled` | boolean | P | Use the configured reputation provider. Turning it off shows a warning in default UI. |
| `security.secureDns.mode`, `security.secureDns.providers` | enum, string | A | DNS over HTTPS |
| `passwords.offerToSave`, `passwords.autoSignIn` | boolean | P | |
| `passwords.leakCheck.enabled` | boolean | P | Added (S16). When `[services.leak_check] provider = "app"`: check the username and password with that provider after a sign-in. On by default. |
| `autofill.addresses.enabled`, `autofill.cards.enabled` | boolean | P | |
| `translation.enabled`, `translation.offer`, `translation.target` | boolean, boolean, language | P | |
| `content.<kind>.default` | `allow`, `block`, `ask` | P | The default for each permission kind; the same values as `permissions.setDefault` |
| `network.proxy.mode`, `network.proxy.server`, `network.proxy.bypass`, `network.proxy.pacUrl` | enum, string | A | Proxy; `system` by default |
| `media.autoplay` | enum | P | Autoplay policy |
| `media.hardwareAcceleration` | boolean | A | Takes effect at next launch |
| `performance.memorySaver.mode` | `off`, `moderate`, `balanced`, `maximum` | A | Added (S15). Chromium's Memory Saver: discard pages that have been hidden for a while. `off` by default, as in Chromium. Discarding under critical memory pressure happens in every mode. |
| `performance.memorySaver.exceptions` | array of site patterns | A | Added (S15). Sites whose pages are never discarded automatically |
| `extensions.developerMode` | boolean | P | When `[extensions] developer_mode = "user"` |
| `devtools.dock` | enum | P | The user's last dock choice |
| `app.*` | declared in Swift | P or A | Application settings ([swift-api.md](swift-api.md) 12.3). Answered by the core: the default is the declared one, and a key declared with `exposedToPages: false` is refused (`capability.denied`) to every page and session, whatever its capabilities |

Rules:

- A key that exists in Chromium and not here is not reachable through BDK. Adding one is an interface-definition change.
- Settings for excluded features, such as sync and sign-in, do not exist.
- `settings.describe` reports `locked: true` for a key the product fixed. Nothing fixes a key in v1; the field is there for a later policy mechanism.

## 7. Events

| Topic | Rule |
|---|---|
| Subscribing | By event name, with an optional filter. Requires the read capability of the namespace: `history.read` for `history.visited`. `app.` events need the capability with the event's name. |
| Delivery | In order per source. Not replayed: a subscriber sees events from the moment it subscribes, and reads current state with a command. |
| Scope | A page receives events for its own profile. A session receives events for the profiles and pages in its scope. |
| Back-pressure | A subscriber that falls behind has events coalesced where the event type allows (progress, state) and is disconnected with an error where it does not (changes). The limit is set in T-SVC-01. |
| End | A subscription ends with its page or session. |

## 8. Building sync, import and backup on these services

The spec excludes any built-in sync and says an application may build its own on BDK's state APIs. What that needs, and where it is:

| Need | Provided by |
|---|---|
| Read everything | `bookmarks.subtree`, `history.search` with pagination, `passwords.list` with `passwords.reveal`, `autofill.*.list`, `settings.getMany`, `extensions.list`, `search.engines` |
| Observe changes | The change events of each namespace |
| Apply remote changes | The write commands, including `history.add` |
| Stable identifiers | Bookmark and extension identifiers are stable across launches. History is keyed by URL and visit time. |
| Tell local from remote | Every change event carries `source`: `user`, `application`, `page`, `session`, `extension`. A sync engine applies remote changes as `application` and ignores its own echoes. |
| Passwords | Reading them in clear needs operating-system authentication each time unless the application caches its own key. That is the application's design, and its risk. |
| Open pages and layout | The application's own data. BDK has none. |

## 8A. Decisions from the Chromium implementation (T-SVC-02, T-SVC-03, T-SVC-06)

- Results are exactly the types of `packages/core/interface/*.json`; timestamps are integer milliseconds; void commands answer `null`. A list's cursor is an opaque string valid for the same arguments.
- `settings.get` and `getMany` answer `null` for a key the catalogue does not list. `settings.set`, `reset` and `describe` reject it with `broker.invalidArguments`. The engine maps 16 catalogue keys and every `content.<kind>.default`; the rest of section 6 is not reachable yet.
- Permission kinds are named `camera`, `microphone`, `geolocation`, `notifications`, `popups`, `javascript`, `images`, `cookies`, `sound`, `automaticDownloads`, `clipboard`, `midi`, `usb`, `bluetooth`, `hid`, `serial`, `windowManagement`, `fileSystemAccess`. A value a kind does not accept (for example `ask` for `javascript`) is `broker.invalidArguments`. `permissions.exceptions` reports Chromium's rule pattern (such as `https://[*.]example.com:443`) as `origin`. The `embargo` source is not reported yet.
- A private profile reads and deletes in its parent's history and records none (`history.add` does nothing); it reads and writes its parent's bookmarks; its cookies, content-setting exceptions, preferences and downloads list are its own; content-setting defaults are set on the parent's map.
- `cookies.sites` reports `storageBytes` as 0: only cookies are counted. A cookie's `sameSite` is `none`, `lax`, `strict` or `null`.
- `privacy.clear` cannot stop a removal that has begun: a cancelled call is answered cancelled and the removal runs on. `privacy.estimate` counts history, cookies and downloads within the call's `range` and `origins` (history and downloads by the origin of their URL, cookies by domain match on the origin's host and by creation time, downloads by start time); the other types report 0 items.
- `downloads.start` starts the download directly; the downloads UI request it is marked with belongs to the downloads surface work. `downloads.retry` resumes an interrupted download that can resume and otherwise starts again from the URL.
- Built-in events are emitted through the core (conventions.md, "The broker", item 12). `history.visited` says `application` for a visit added with `history.add` and `user` for a navigation.
- Screenshots, PDFs and saved pages are `contents.screenshot`, `pdf` and `save` since M7 (5.15).
- `bookmarks.import` (T-SVC-07) reads the file with Chromium's `ContentBookmarkParser`, which runs the Netscape-format HTML parser in a utility process as Chromium's own import does, then adds the entries itself. They go into a new folder titled "Imported bookmarks" at the end of the other bookmarks (never the bar, and the profile's existing bookmarks are untouched), keeping the file's folders; an entry with an invalid URL is skipped. It answers `{imported}`, the number of bookmarks (folders are not counted), and reports progress (0 on starting to read, 0.5 once parsed, then every 100 added). A path that is empty, unreadable or not bookmarks HTML fails with `broker.invalidArguments`. A private profile imports into its parent's bookmarks.
- `bookmarks.export` (T-SVC-07) writes the profile's bookmarks with Chromium's `bookmark_html_writer`, favicons included. It answers `null`; a file that cannot be written fails with `platform.failed`, and a second export while one runs for the same bookmarks with `platform.busy`.
- `permissions.certificateExceptions` answers `{hosts}`: the hosts (sorted, no port) where a certificate error was proceeded past and the decision still holds, from Chromium's `StatefulSSLHostStateDelegate` (T-SVC-07). `permissions.revokeCertificateException` takes `{host}`, forgets the decision (and closes idle connections, so the next visit meets the error again) and answers `null`, also when there was none. Each profile keeps its own; a private profile's go with it.

## 9. Where definitions live

`packages/core/interface/` holds one file per namespace. The format is in [c-abi.md](c-abi.md) section 7. From those files the build generates:

| Output | For |
|---|---|
| C command and event identifiers | The engine and the stub runtime |
| Swift wrappers and value types | `packages/swift` |
| TypeScript declarations and thin clients | `@bdk/internal/*`, `@bdk/automation` |
| The capability table | The engine's broker and the `bdk.toml` schema |
| Reference documentation | `apps/docs` |

The Swift wrappers are generated, not written by hand (T-SVC-02 to T-SVC-06, decided 2026-11-19). `packages/js/codegen` (`src/swift.ts`) writes `packages/swift/Sources/BDK/Generated/BDKServices.swift`, which is committed; `nx run js-codegen:codegen-check` fails when a definition changed and the file did not, as it does for `bdk_ids.h` and the broker table. A command therefore exists in Swift when it is defined and `callers` includes `swift`. Swift-only naming (a method renamed, a label, a parameter order, the namespaces the SDK implements natively) is a small table at the top of `swift.ts`, never a second definition. The mapping rules are in `packages/swift/README.md`, "Services".

A command that needs Chromium is implemented in `chromium/bdk`. Its argument validation, capability mapping and pagination logic live in `packages/core`, where hosted CI tests them without a Chromium build (tooling document 10.1).
