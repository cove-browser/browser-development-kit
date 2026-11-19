# BDK configuration reference

Status: proposal, 2026-11-15. Key names are placeholders except where the spec or [../architecture.md](../architecture.md) already uses them. The schema is task T-CFG-01 and the resolver is T-CFG-02. Status labels are defined in [README.md](README.md) section 2.

## 1. The files

| File | Written by | Read by | When | Holds |
|---|---|---|---|---|
| `bdk.toml` | The developer | The CLI only | Build | All BDK-specific configuration. The single source of truth. |
| `bdk.resolved.json` | `bdk build` | The Swift SDK, then the engine | Start | The validated, defaulted, fully resolved form of `bdk.toml`, with the route table and capability grants. Inside the signed bundle. |
| `Info.plist` | The developer, in Xcode | macOS, Chromium | Launch | Bundle identity, principal class, usage descriptions, URL and document types |
| Entitlements | The developer; the signing step for helpers | macOS | Launch | Hardened-runtime exceptions and resource access |
| `Package.resolved` | SwiftPM | SwiftPM | Build | Which BDK version, and so which runtime, the application uses |

The engine never parses TOML. It reads `bdk.resolved.json` and nothing else from disk (`architecture.md` 8.2).

Xcode stays authoritative for the bundle identifier, signing, provisioning and targets. Where `bdk.toml` and the Xcode project disagree on something both express, the build fails and names both values. It does not pick one.

## 2. Rules for `bdk.toml`

| Rule | Detail |
|---|---|
| Location | The directory that contains the Xcode project. One per application target. |
| Required content | `[browser] name`. Everything else has a default. `bdk init` always writes the file, and a missing file is a build error. |
| Unknown keys | A build error that names the key and the nearest valid one. No key is ignored. |
| No `chrome://` | A value containing `chrome://`, `chrome-untrusted://` or `devtools://` is a build error. Pages are named logically. |
| No secrets | API keys and tokens are supplied from Swift or a build setting. A key that looks like a credential is a build error. |
| Paths | Relative to the directory of `bdk.toml`, forward slashes, and inside it. A path that leaves the directory is a build error. |
| Prebuilt and source | A key marked "source only" is a build error when `runtime.mode = "prebuilt"`. |
| A flag is not a credential | Enabling a provider or a store never implies that keys, store authorisation or backend access exist. A provider that needs a key and gets none is a start error. |

## 3. Reference

Each table lists the key, its type, its default, its status and what it does.

### 3.1 `[browser]`

| Key | Type | Default | Status | Meaning |
|---|---|---|---|---|
| `name` | string | required | Plan | Display name. Used in default-presenter text and as the fallback for the identifiers below. |
| `internal_scheme` | string | derived from `name` | Plan | The branded internal URL scheme. Lower case, matches `[a-z][a-z0-9+.-]*`. Set it explicitly: a derived scheme changes when the name does. |
| `logo` | path | The application icon | Added (S13) | An SVG or PNG shown wherever Chromium's own pages and dialogs show a product logo. See 3.1.1. |

Validation: the scheme must not be empty after derivation, must not be a reserved name (`http`, `https`, `file`, `chrome`, `chrome-untrusted`, `chrome-extension`, `devtools`, `about`, `blob`, `data`, `javascript`, `view-source`, `ws`, `wss`, `ftp`, `mailto`, `tel`, and the rest of the list fixed in T-CFG-01), and must not collide with a URL scheme the application declares in `Info.plist` for another purpose.

#### 3.1.1 Branding beyond the name (Added, S13)

`bdk.toml` holds the name, the scheme, the user-agent brand and the storage identifiers. The rest of a browser's visual identity comes from these places:

| Where the brand shows | Source | Who sets it |
|---|---|---|
| Dock, Finder, the application switcher, macOS permission prompts, the default-browser list, notifications | The application's `AppIcon` and bundle name | The developer, in Xcode. BDK does not read or copy it into `bdk.toml`; Xcode stays authoritative. |
| Product logo in Chromium's built-in pages (`about`, and the header of `settings`, `history`, `downloads`, `extensions`) and in default dialogs that show one | Compiled into the prebuilt runtime's resources as the Chromium logo | BDK substitutes `[browser] logo` at start, or the application icon when the key is absent. Which resources carry the logo at the pin is unverified. |
| Product name in Chromium's strings | `[browser] name` | Plan: task T-PKG-02 |
| Helper process names in Activity Monitor | Neutral names in the prebuilt runtime | `bdk build sign` writes `<name> Helper` display names into the helper bundles before signing. Unverified. |
| Icon on alert-style notifications | Upstream copies the application icon into the alerts helper at build time; the prebuilt runtime has none | `bdk build sign` copies the application icon into that helper. Unverified. |
| Favicon of an internal page | The page's own `<link rel="icon">` for a bundled page; Chromium's for a built-in page | The developer, for bundled pages |
| Colours and appearance of default presenters | Chromium's, following the system light or dark setting | Not configurable in v1. Theming is Later ([ui-requests.md](ui-requests.md) section 6). A product that needs its own look uses `custom` mode for that surface. |

There is no icon key in `bdk.toml` on purpose: a second place to declare the application icon could only disagree with Xcode.

#### `[browser.user_agent]` (Added, S5)

| Key | Type | Default | Meaning |
|---|---|---|---|
| `brand` | string | none | Added to the `Sec-CH-UA` brand list beside "Chromium". With no value the list is "Chromium" only. |
| `brand_version` | string | The application's marketing version from `Info.plist` | The version sent with the brand. |

The `User-Agent` header keeps Chromium's reduced, Chrome-compatible form, because sites depend on it. BDK never adds a "Google Chrome" brand. A per-page override is a Swift call, not configuration.

### 3.2 `[storage]` (Added, S2)

| Key | Type | Default | Meaning |
|---|---|---|---|
| `directory` | string | `name` | The directory under `~/Library/Application Support` that holds profiles and local state. |
| `keychain_item` | string | `"<name> Safe Storage"` | The Keychain item that holds the key protecting cookies and passwords. |

Set both explicitly before the first public release and never change them. They exist so that renaming the browser does not orphan user data.

### 3.3 `[runtime]`

| Key | Type | Default | Status | Meaning |
|---|---|---|---|---|
| `mode` | `"prebuilt"` or `"source"` | `"prebuilt"` | Plan | Prebuilt uses the published runtime for the resolved BDK version. Source builds one from the checkout with the application's patches. |
| `downgrade` | `"refuse"` or `"fresh-profile"` | `"refuse"` | Plan, M9, T-PKG-05 | What happens when an older engine meets a profile last used by a newer one and no snapshot exists. `refuse` fails start with an error the application can show. `fresh-profile` starts with a new profile and leaves the old one untouched. |
| `snapshot_on_upgrade` | boolean | `true` | Plan, M9 | Snapshot each profile before the first launch of a newer engine milestone. |
| `locales` | array of strings | all shipped | Later | Locale resources to keep in the bundle. Reduces size. |

The BDK version is not in `bdk.toml`. It is the version of the Swift package the application depends on.

### 3.4 `[chromium]`

| Key | Type | Default | Status | Meaning |
|---|---|---|---|---|
| `revision` | string | The pin of the resolved BDK version | Plan, source only | A Chromium tag. It must be one the BDK version declares compatible. |
| `patches` | string or array | `"Chromium/patches"` if it exists | Plan, source only | A directory with a `series` file, or an ordered list of patch files, applied after BDK's own series. |
| `gn_args` | table | none | Plan, source only | Extra GN arguments. Arguments on the deny list in section 3.4.1 are rejected. |
| `modules` | array of paths | none | Later | Native modules built into the runtime. The spec leaves "how native extensions register" open (section 9). |

#### `[chromium.features]` (Added, S9)

| Key | Type | Default | Meaning |
|---|---|---|---|
| `enable` | array of strings | `[]` | Chromium feature names to enable at start. |
| `disable` | array of strings | `[]` | Feature names to disable. |

Works in both modes, because features are runtime switches. A runtime started with overrides reports itself as outside the tested configuration in `runtime.info` and in `bdk doctor`. Unknown names are a warning, since features come and go between milestones.

#### 3.4.1 Deny list

Rejected in `gn_args`, in `features`, and anywhere else a switch could be expressed, in both modes:

| Rejected | Invariant it protects ([../architecture.md](../architecture.md) section 16) |
|---|---|
| Anything that disables the sandbox, site isolation or process locks | 1 |
| Remote debugging on a network port or a pipe | 7 |
| Disabling web security, certificate checks or CORS | 1, 6 |
| Google API keys in configuration | 9 |
| Re-enabling sign-in, sync, metrics upload, crash upload or variations fetches | Section 12 |
| Auto-granting permissions, media capture or extension installs | 6 |

The list is maintained with the Google-removal audit list (T-PRV-07) and checked by fixtures.

### 3.5 `[pages]`

| Key | Type | Default | Status | Meaning |
|---|---|---|---|---|
| `auto_discover` | boolean | `true` | Plan | Treat each directory under `directory` that has an `index.html` as a page. |
| `directory` | string | `"Pages"` | Plan | Where bundled pages live. |
| `diagnostics` | `"chromium"` or `"disabled"` | `"chromium"` | Added | One switch for the diagnostic built-ins in section 4.2, so a product can turn them all off without listing each. An entry under `[pages.overrides]` still wins for a single route. |

#### `[pages.overrides]`

One line per logical route: `route = mode`.

| Value | Meaning |
|---|---|
| `"chromium"` | Serve the built-in page. An error if the route has no built-in. |
| `"native"` | Deliver the navigation to a Swift handler. A handler must be registered before start. |
| `"disabled"` | Block navigation with the standard error page. No redirect. |
| A path | Serve the bundled page at that path. The directory must contain `index.html`, or a Vite entry that the build turns into one. |

#### `[pages.routes]`

One line per alias: `alias = "target"`. The target must be a page, never another alias. The alias shares the target's identity and capabilities.

#### `[pages.capabilities.<route>]`

| Key | Type | Default | Meaning |
|---|---|---|---|
| `allow` | array of capability names | `[]` | The BDK capabilities this page may use. The list is in [services.md](services.md) section 4. A trailing `*` is allowed only after `app.`: `"app.sidebar.*"`. |

Rules:

- Only bundled pages can be granted capabilities. A grant for a route in `chromium`, `native` or `disabled` mode is a build error. Built-in pages keep the interfaces Chromium gives them and get nothing more.
- A grant for a route that does not exist is a build error.
- A capability on the "never grantable to pages" list (`contents.*`, `automation.*`, `devtools.*`) is a build error.
- A page with no table has no capability. Importing `@bdk/internal/*` then yields calls that are refused.
- An unknown `app.` capability is a build error once the application's command declarations are available to the build, and a start error otherwise.

Precedence between sources is in `architecture.md` 8.2: Swift registration, then `[pages.overrides]`, then auto-discovery, then the built-in default.

### 3.6 `[ui]`

| Key | Type | Default | Status | Meaning |
|---|---|---|---|---|
| `default_browser_surfaces` | boolean | `true` | Plan | The mode for every surface not listed under `[ui.surfaces]`: `true` means `default`, `false` means `custom`. |

#### `[ui.surfaces]`

One line per surface: `surface = "default" | "custom" | "disabled"`. Surface names and which modes each allows are in [ui-requests.md](ui-requests.md) section 4.

| Mode | A handler is | Behaviour |
|---|---|---|
| `default` | Optional | The handler, if any, sees each request and returns handled, use-default or reject. With no handler the default presenter shows it. |
| `custom` | Required before the registration freeze | As `default`, but a surface with no handler when registrations are frozen fails the freeze with `config.handlerMissing` (the runtime reads the resolved configuration at start, and handlers are registered after start and before the freeze). The default presenter is still available through use-default. |
| `disabled` | Ignored | The feature is unavailable. Requests are denied or cancelled, and the web or extension API behind them reports itself unavailable. Not allowed for surfaces marked "cannot be disabled". |

### 3.7 `[extensions]`

| Key | Type | Default | Status | Meaning |
|---|---|---|---|---|
| `enabled` | boolean | `true` | Plan | The extension system as a whole. `false` removes extension APIs, pages and installation. |
| `install_policy` | `"reject_known_incompatible"` or `"allow_all"` | `"reject_known_incompatible"` | Plan; the second value is proposed | Whether a manifest that requires a disabled or unsupported surface is refused before installation. With `allow_all` it installs, and unsupported calls still fail explicitly. |
| `allow_in_private` | `"user"` or `"never"` | `"user"` | Plan (spec: incognito compatibility is open) | Whether the user may allow an extension in private profiles. |
| `developer_mode` | `"user"`, `"on"` or `"off"` | `"user"` | Added | Whether loading unpacked extensions is a user setting, always on, or unavailable. |

#### `[extensions.install]`

| Key | Type | Default | Status | Meaning |
|---|---|---|---|---|
| `unpacked` | boolean | `true` | Plan | Load from a directory. |
| `crx` | boolean | `true` | Plan | Install a local CRX file. BDK owns the off-store policy and keeps Chromium's install prompt. |
| `update_urls` | array of URL patterns, or `"any"` | `"any"` | Plan | Which self-hosted update manifests an extension may name. |
| `stores` | array of names | `[]` | Plan, M5 | Named store sources. `"chrome-web-store"` is available and off unless listed (decision D4); when listed, the store's install button reads "Add to <browser name>". The spec's `compatible_stores = true` is replaced by this list. |
| `blocklist` | array of extension identifiers | `[]` | Added | Identifiers that never install or load. |

#### `[extensions.ui]`

One line per surface, with the same three modes as `[ui.surfaces]`: `actions`, `popups`, `side_panels`, `context_menus`, `options_pages`, `devtools`. Details are in [extensions.md](extensions.md) section 4.

#### `[extensions.bundled]` (Added, S6)

One line per extension: `name = "Extensions/<directory>"`. The directory is an unpacked extension inside the application bundle, covered by its signature. It loads at start in every persistent profile, cannot be uninstalled by the user, and can be disabled by the user unless `required = true` is set in the long form:

```toml
[extensions.bundled.reader]
path = "Extensions/Reader"
required = true
```

### 3.8 `[services]`

One table per service. All three accept the same three ways of choosing a provider (`architecture.md` 11.1).

| Key | Type | Meaning |
|---|---|---|
| `provider` | string | `"default"`, `"app"`, or a named provider BDK ships |
| `endpoint` | URL | Optional. Keeps the provider's protocol and sends requests to this host. |

| Table | `provider` values | Default | Status |
|---|---|---|---|
| `[services.translation]` | `"default"` (local engine), `"app"`, and after v1 `"apple"`, `"deepl"`, `"microsoft"`, `"google"` | `"default"` | Plan, M6 |
| `[services.reputation]` | `"default"` (signed static block lists, decision D2), `"app"`, `"web-risk"`, `"google"` | `"default"` | Plan, M6 |
| `[services.geolocation]` | `"system"`, `"app"`, `"google"` | `"system"` | Plan, M6 |

Extra keys:

| Key | Table | Type | Default | Status | Meaning |
|---|---|---|---|---|---|
| `models` | translation | `"on-demand"` or `"bundled"` | `"on-demand"` | Plan | Whether language models download when first needed or ship in the bundle. |
| `bundled_languages` | translation | array of language codes | `[]` | Plan | With `models = "bundled"`, which pairs to include. |
| `model_source` | translation | URL | undecided | Open, [README.md](README.md) section 7 | Where on-demand models come from. |
| `update_interval` | reputation | duration string | `"1h"` | Plan | How often list updates are fetched, for list-based providers. |

`provider = "app"` requires a Swift registration before start. A named provider that needs a key requires that key from Swift before start. Both are start errors when missing.

The spec's illustrative `[services.google]` table of booleans is not used. Choosing Google is `provider = "google"` on the service concerned, so there is one way to say it.

#### `[services.credentials]` (Added, S14)

| Key | Type | Default | Meaning |
|---|---|---|---|
| `provider` | `"chromium"` or `"app"` | `"chromium"` | Where saved logins live. `chromium` is the profile's own encrypted store. `app` sends every read and write to a provider registered from Swift ([swift-api.md](swift-api.md) 11.2); Chromium still detects forms, raises the save and suggestion requests and fills, and stores no login itself. |

This key changes storage, not UI. Native save prompts and suggestion popups are the `passwords` and `autofill_suggestions` surfaces in `custom` mode, with either provider. The Keychain item in `[storage]` exists with either provider too, because it also protects cookies.

#### `[services.leak_check]` (Added, S16)

| Key | Type | Default | Meaning |
|---|---|---|---|
| `provider` | `"none"` or `"app"` | `"none"` | Who answers "has this username and password appeared in a known breach?". `none` means no check: nothing is sent anywhere, no warning is shown, and the check commands return `provider.unavailable`. `app` asks a provider registered from Swift ([swift-api.md](swift-api.md) 11.2), after a sign-in and when the saved logins are checked ([services.md](services.md) 5.9). |

`provider = "app"` with no registered provider is a start error, as for the other services. There is no `endpoint` key, because there is no shipped protocol to redirect.

There is no `"google"` value. Read at the pinned tag in `components/password_manager/core/browser/leak_detection/`:

| Chromium's check | How it authenticates | Consequence for BDK |
|---|---|---|
| After a sign-in | An access token for a signed-in user, otherwise the Google API key, sent to `passwordsleakcheck-pa.googleapis.com` (`leak_detection_request.h:39-40`; `leak_detection_check_factory_impl.cc:26-31`) | The application's key would be accepted mechanically. Whether Google allows a third-party key on this endpoint is unverified; it is not one of the published Cloud APIs. |
| Of every saved login | A signed-in Google account only; no key path (`leak_detection_check_factory_impl.cc:58-59`; `bulk_leak_check_impl.cc:151-153`) | Cannot work, because BDK removes sign-in. |

A named non-Google provider, such as a hash-prefix range service, is Later: each one needs its terms reviewed first. Until then an application reaches such a service through `provider = "app"`.

#### `[services.components]` (Conditional, D5, delegated to the implementation agent)

| Key | Type | Meaning |
|---|---|---|
| `source` | `"bdk"`, `"bundled"`, `"google"`, or a URL | Where revocation lists, the Certificate Transparency log list and root-store updates come from. The values that exist depend on task T-PRV-06 and decision D5. |

### 3.9 `[search]` (Added, S1)

| Key | Type | Default | Meaning |
|---|---|---|---|
| `default` | string | required if the table exists | The identifier of the default engine, one of the engines below. |
| `suggestions` | boolean | `false` | Whether typed text is sent to the default engine for suggestions before the user submits it. Off by default because it sends keystrokes to a third party. It is also a user setting. |

```toml
[search]
default = "duckduckgo"

[search.engines.duckduckgo]
name = "DuckDuckGo"
keyword = "ddg"
search_url = "https://duckduckgo.com/?q={searchTerms}"
suggest_url = "https://duckduckgo.com/ac/?q={searchTerms}&type=list"
favicon_url = "https://duckduckgo.com/favicon.ico"
```

No engine is compiled in. With no `[search]` table the classification call still tells a URL from a query, and the application decides what a query does. Users can add, edit and remove engines at runtime; these are the first-run set.

### 3.10 `[settings.defaults]` (Added, S7)

One line per setting key from [services.md](services.md) section 6: `"privacy.cookies.thirdParty" = "block"`. The value becomes the default for new and existing profiles where the user has not chosen. It is not a policy: the user can change it.

Keys that would weaken a security invariant cannot be defaulted off. The schema lists them.

### 3.11 `[automation]`

| Key | Type | Default | Status | Meaning |
|---|---|---|---|---|
| `enabled` | boolean | `false` | Plan, M7 | Whether the automation host exists in this application. |
| `directory` | string | `"Automation"` | Plan | Where application-authored scripts live. Each top-level `.ts` or `.js` file is one script, bundled with Rolldown. |

#### `[automation.external]`

| Key | Type | Default | Meaning |
|---|---|---|---|
| `enabled` | boolean | `false` | Whether the application may open the authenticated local endpoint at runtime. `true` only permits it. Swift opens it and supplies the authenticator. |

There is no key for a network port. The endpoint is a user-restricted local socket (invariant 7).

### 3.12 `[devtools]`

| Key | Type | Default | Status | Meaning |
|---|---|---|---|---|
| `frontend` | `"default"`, `"disabled"`, or a path | `"default"` | Plan, M7, T-AUT-05 | Chromium's DevTools front end, none, or a complete replacement packaged from the path. |
| `overlay` | path | none | Plan | A directory of CSS and JavaScript loaded into the default front end. The "modified" mode. |
| `protocol` | boolean | `true` | Plan | Whether the DevTools protocol is available to Swift and to automation sessions that hold `contents.debug`. Independent of the front end. |

`frontend = "disabled"` removes the front end for users. It does not disable the protocol.

### 3.13 `[diagnostics]` (Added, S4)

| Key | Type | Default | Meaning |
|---|---|---|---|
| `crash_reports` | `"off"`, `"local"` or `"app"` | `"local"` | `local` keeps reports on disk for the user. `app` also delivers each completed report to a Swift handler, which may upload it to the application's own service after obtaining consent. BDK uploads nothing. |
| `log_level` | `"error"`, `"warning"`, `"info"` or `"verbose"` | `"warning"` | Verbosity of the BDK and Chromium log. |

### 3.14 `[codegen]`

| Key | Type | Default | Status | Meaning |
|---|---|---|---|---|
| `typescript` | boolean | `true` when `Pages/` or `Automation/` exists | Plan | Generate `@bdk/*` declarations. An application with no JavaScript needs none. |
| `commands_module` | string | The application target | Plan | The Swift module whose command, event and setting declarations are extracted. |

## 4. Logical routes

### 4.1 Standard routes

The complete table is produced by task T-WUI-05 from the pinned tag. This is the planned classification.

| Route | Built-in | Default mode | Notes |
|---|---|---|---|
| `new-tab` | A minimal page shipped by BDK | `chromium` | Chromium's own new-tab page depends on Google services. It is not exposed. Unverified how much of it would work without them. |
| `settings` | Chromium settings | `chromium` | Sign-in and sync sections are removed by the audit list. |
| `extensions` | Chromium extension management | `chromium` | Disabled automatically when `[extensions] enabled = false`. |
| `history` | Chromium history | `chromium` | Synced-tabs sections removed. |
| `bookmarks` | Chromium bookmark manager | `chromium` | |
| `downloads` | Chromium downloads | `chromium` | |
| `passwords` | Chromium password manager | `chromium` | Chromium's leak detection, which calls Google, is removed. With `[services.leak_check] provider = "app"` the page's check uses the application's provider; whether the built-in page's check section works through it is unverified. |
| `about` | Version and build information | `chromium` | Shows the BDK version, the Chromium version and the application's name. |
| `credits` | Third-party licences | `chromium` | Cannot be disabled: the notices have to ship (task T-REL-02). An application may replace it with a bundled page that shows the same notices. |
| `flags` | Chromium feature flags | `chromium` | Many products will disable this. |

For these routes, "mode `chromium`" in the table means the built-in page. For `new-tab` the built-in is BDK's own.

### 4.2 Diagnostic routes

Governed together by `[pages] diagnostics`: `gpu`, `net-export`, `net-internals`, `sandbox`, `process-internals`, `inspect`, `tracing`, `crashes`, `components`, `media-internals`, `webrtc-internals`, `serviceworker-internals`, `accessibility`, `histograms`, `device-log`, `quota-internals`, `indexeddb-internals`, `blob-internals`, `extensions-internals`, and the rest of the set at the pinned tag.

### 4.3 Reserved names

Names Chromium needs internally. An application cannot use them for a page, an alias or a directory: `resources`, `theme`, `favicon`, `favicon2`, `print`, `devtools`, `network-error`, `error`, and every name beginning `bdk-`. The full list is fixed in T-CFG-01.

`print` is reserved and not a route because print preview is a surface ([ui-requests.md](ui-requests.md)), not a page a user navigates to.

### 4.4 Excluded

Never reachable, whatever the configuration, because the feature is excluded from the runtime: sign-in, sync internals, feedback, "what's new", the welcome flow, and every page that exists only for a Google service.

### 4.5 Application routes

Any directory under `Pages/` whose normalised name is not in 4.1 to 4.4 becomes an application route. `Pages/Agents` is `<scheme>://agents`. Name normalisation and collisions are in `architecture.md` 8.2.

## 5. A complete example

```toml
[browser]
name = "Cove"
internal_scheme = "cove"

[browser.user_agent]
brand = "Cove"

[storage]
directory = "Cove"
keychain_item = "Cove Safe Storage"

[runtime]
mode = "prebuilt"
downgrade = "refuse"

[pages]
auto_discover = true
diagnostics = "disabled"

[pages.overrides]
history = "chromium"          # keep the built-in even though Pages/History exists
downloads = "native"          # delivered to a Swift handler
flags = "disabled"

[pages.routes]
preferences = "settings"      # cove://preferences, same identity as cove://settings

[pages.capabilities.settings]
allow = ["settings.read", "settings.write", "permissions.manage", "search.manage", "app.appearance.*"]

[pages.capabilities.new-tab]
allow = ["top-sites.read", "favicons.read", "app.tabs.open"]

[ui]
default_browser_surfaces = true

[ui.surfaces]
context_menus = "custom"
downloads = "custom"
notifications = "disabled"

[extensions]
enabled = true
install_policy = "reject_known_incompatible"

[extensions.install]
unpacked = true
crx = true
update_urls = "any"
stores = []

[extensions.ui]
actions = "custom"
popups = "custom"
side_panels = "custom"
context_menus = "default"
options_pages = "default"

[services.translation]
provider = "default"

[services.reputation]
provider = "default"

[services.geolocation]
provider = "system"

[search]
default = "duckduckgo"
suggestions = false

[search.engines.duckduckgo]
name = "DuckDuckGo"
keyword = "ddg"
search_url = "https://duckduckgo.com/?q={searchTerms}"

[settings.defaults]
"privacy.cookies.thirdParty" = "block"
"security.httpsOnly" = true

[automation]
enabled = true

[automation.external]
enabled = false

[devtools]
frontend = "default"

[diagnostics]
crash_reports = "app"
```

The smallest valid file:

```toml
[browser]
name = "Tide"
```

## 6. `Info.plist` and entitlements

BDK does not edit the application's `Info.plist`. `bdk doctor` and `bdk build` check it and report what is missing.

| Key | Required | Why |
|---|---|---|
| `NSPrincipalClass` = `BDKApplication`, or a subclass | Yes | Chromium's message pump needs an application object that conforms to its protocols (`architecture.md` 6.1). |
| `CFBundleIdentifier` | Yes | Mach service names, sandbox names and helper bundle identifiers are derived from it. |
| `CFBundleShortVersionString` | Yes | The product version in default UI and in the user-agent brand. |
| `LSMinimumSystemVersion` | Yes | Must be at least the `min_os` in the runtime manifest. |
| `NSCameraUsageDescription`, `NSMicrophoneUsageDescription` | For camera and microphone | macOS refuses capture without them, whatever the site permission says. |
| `NSLocationUsageDescription` | For geolocation with the system provider | As above. |
| `NSBluetoothAlwaysUsageDescription` | For Web Bluetooth | As above. |
| `NSLocalNetworkUsageDescription` | For pages that reach local-network hosts | macOS prompts for local-network access. |
| `CFBundleURLTypes` with `http` and `https` | To be a default-browser candidate | Launch Services lists only applications that declare them. |
| `CFBundleDocumentTypes` for HTML | To be a default-browser candidate | As above. |
| `CFBundleURLTypes` with the internal scheme | No, and not recommended | It would let other applications open internal pages. If declared, the application decides in its URL handler whether to honour it. |

Entitlements for the main application, under the hardened runtime. The list is expected to match upstream's `chrome/app/app-entitlements.plist` at the pin and is confirmed in T-PKG-03:

| Entitlement | Needed for |
|---|---|
| `com.apple.security.device.camera` | Camera |
| `com.apple.security.device.audio-input` | Microphone |
| `com.apple.security.personal-information.location` | Geolocation |
| `com.apple.security.device.bluetooth` | Web Bluetooth |
| `com.apple.security.device.usb` | WebUSB |
| `com.apple.security.device.print` | Printing |

The helper applications inside the runtime are re-signed by the BDK signing step with the application's identity. Renderer and GPU helpers get the JIT entitlement only. The application never signs them by hand.

The App Sandbox entitlement must not be set on the main application. See [README.md](README.md) section 6.

## 7. `bdk.resolved.json`

Written by `bdk build config` into `Contents/Resources`. The Swift SDK loads it with `BDKConfiguration.load()`.

```json
{
  "schema": 1,
  "bdk_version": "0.4.0",
  "config_digest": "sha256:…",
  "browser": {
    "name": "Cove",
    "internal_scheme": "cove",
    "logo": "app-icon",
    "user_agent": { "brand": "Cove", "brand_version": "1.2.0" }
  },
  "storage": { "directory": "Cove", "keychain_item": "Cove Safe Storage" },
  "runtime": { "mode": "prebuilt", "downgrade": "refuse", "snapshot_on_upgrade": true },
  "routes": {
    "settings":  { "mode": "bundled",  "path": "Pages/settings",  "host": "bdk-settings", "digest": "sha256:…",
                   "capabilities": ["settings.read", "settings.write", "permissions.manage", "search.manage", "app.appearance.*"] },
    "new-tab":   { "mode": "bundled",  "path": "Pages/new-tab",   "host": "bdk-new-tab",  "digest": "sha256:…",
                   "capabilities": ["top-sites.read", "favicons.read", "app.tabs.open"] },
    "history":   { "mode": "chromium" },
    "downloads": { "mode": "native" },
    "flags":     { "mode": "disabled" }
  },
  "aliases": { "preferences": "settings" },
  "surfaces": { "permissions": "default", "context_menus": "custom", "downloads": "custom", "notifications": "disabled" },
  "extensions": { "enabled": true, "install_policy": "reject_known_incompatible",
                  "install": { "unpacked": true, "crx": true, "update_urls": "any", "stores": [] },
                  "ui": { "actions": "custom", "popups": "custom", "side_panels": "custom",
                          "context_menus": "default", "options_pages": "default", "devtools": "default" },
                  "bundled": {} },
  "services": { "translation": { "provider": "default", "models": "on-demand" },
                "reputation": { "provider": "default", "update_interval": "1h" },
                "geolocation": { "provider": "system" },
                "credentials": { "provider": "chromium" },
                "leak_check": { "provider": "none" } },
  "search": { "default": "duckduckgo", "suggestions": false, "engines": { "duckduckgo": { "…": "…" } } },
  "settings_defaults": { "privacy.cookies.thirdParty": "block", "security.httpsOnly": true },
  "automation": { "enabled": true, "scripts": { "summarise": { "path": "Automation/summarise.js", "digest": "sha256:…" } },
                  "external": { "enabled": false } },
  "devtools": { "frontend": "default", "protocol": true },
  "diagnostics": { "crash_reports": "app", "log_level": "warning" },
  "chromium_features": { "enable": [], "disable": [] }
}
```

Properties:

- Every default is written out. The engine has no defaults of its own.
- Every bundled page and script carries a digest, so `bdk doctor` can tell a stale resolved file from a tampered bundle. The code signature is what protects the file; the digests are for diagnosis.
- `schema` is an integer that changes only on an incompatible change (tooling document 8.1).
- Swift registrations are not in the file. They are applied in memory, before start.

## 8. Environment variables

For the developer's machine and the build. None is read by a shipped application.

| Variable | Read by | Meaning |
|---|---|---|
| `BDK_HOME` | CLI | Root of the runtime store, managed checkout and compiler cache. Default `~/.bdk`. No spaces. |
| `BDK_RUNTIME_MODE` | CLI | `prebuilt` or `source`. Overrides `runtime.mode` for one build. |
| `BDK_RUNTIME_PATH` | CLI | Use this runtime directory and skip resolution. For engine developers. Its ABI hash is still checked. |
| `BDK_SKIP_SIGNING` | CLI | Skip the signing step. For builds that are signed later. |
| `BDK_LOG` | CLI | CLI verbosity. |

Debug builds of the Swift SDK also honour `BDK_LOG_LEVEL` at launch, so a developer can raise verbosity from the Xcode scheme. Release builds ignore it.

## 9. Validation errors

Each has a stable code, used by fixtures in `tests/config/`. Messages name the file, the key and the fix.

| Code | Condition |
|---|---|
| `config.missing` | No `bdk.toml` |
| `config.syntax` | Invalid TOML |
| `config.unknown_key` | A key not in the schema |
| `config.type` | A value of the wrong type or outside the allowed set |
| `config.required` | A required key or table is missing |
| `config.internal_url` | A value contains an internal Chromium scheme |
| `config.secret` | A value looks like a credential |
| `config.path_escape` | A path leaves the project directory |
| `config.source_only` | A source-only key in prebuilt mode |
| `config.denied_switch` | A GN argument or feature on the deny list |
| `browser.scheme_invalid` | The scheme is empty, malformed or reserved |
| `browser.scheme_conflict` | The scheme collides with one declared in `Info.plist` |
| `browser.logo_invalid` | `logo` is not an SVG or PNG inside the project |
| `pages.collision` | Two directories normalise to the same route |
| `pages.bad_link` | A link inside a page directory points nowhere or back to a directory that contains it |
| `pages.alias_collision` | An alias has the same name as a page |
| `pages.alias_chain` | An alias points at an alias |
| `pages.alias_target` | An alias points at nothing |
| `pages.reserved` | A page, alias or directory uses a reserved name |
| `pages.name_invalid` | A page directory, route or alias name does not match `[a-z][a-z0-9-]*` after normalisation |
| `pages.no_builtin` | `chromium` mode for a route with no built-in page |
| `pages.missing_entry` | A bundled path with no `index.html` or Vite entry |
| `pages.capability_unknown` | A capability name that does not exist |
| `pages.capability_denied` | A capability that pages can never hold |
| `pages.capability_target` | A grant for a route that is not a bundled page |
| `automation.collision` | Two files in the automation directory have the same script name |
| `ui.surface_unknown` | An unknown surface name |
| `ui.surface_mode` | A mode the surface does not allow, such as `disabled` for interstitials. The runtime core checks the resolved file again at start and reports the same condition as `ui.surfaceMode` (and an unknown surface as `ui.surfaceUnknown`, a custom surface without a handler at the freeze as `config.handlerMissing`) |
| `extensions.store_unknown` | A store name BDK does not ship |
| `extensions.bundled_invalid` | A bundled extension with no valid manifest |
| `services.provider_unknown` | A provider name BDK does not ship |
| `search.default_missing` | `default` names an engine that is not defined |
| `search.url_invalid` | A search URL with no `{searchTerms}` |
| `settings.key_unknown` | A default for a setting that does not exist |
| `settings.locked` | A default that would weaken a security invariant |
| `plist.principal_class` | `NSPrincipalClass` is not `BDKApplication` or a subclass |
| `plist.conflict` | `bdk.toml` and the Xcode project disagree |
| `plist.min_os` | The deployment target is below the runtime's minimum |
| `plist.app_sandbox` | The App Sandbox entitlement is set |

### Decisions made in T-CFG-01/02

Dated 2026-11-18. The documents were silent on these points; the schema and resolver in `packages/cli` follow them.

- Only keys with status Plan are in the schema. Every key marked Added, with or without an S number, and every Conditional or Later key, is rejected as an unknown key until the product owner accepts it. The list is in `packages/cli/src/core/config/README.md`. For the resolved file, the sections those keys would fill carry their documented defaults.
- Three codes were added to the table above: `config.required`, `pages.name_invalid` and `automation.collision`. Disabling `credits` is `config.type`.
- Surface names under `[ui.surfaces]` and `[extensions.ui]` that are not in the schema are `ui.surface_unknown`, except the S3 surfaces, which are the ordinary `config.unknown_key`. A mode the surface does not allow is `ui.surface_mode`; a value that is not a mode at all is `config.type`.
- Provider values outside the listed enum are `services.provider_unknown`. The named providers documented as "after v1" are accepted by the schema.
- Reserved names are the fixed list in 4.3 plus `chrome-error`, `chrome-search`, `chrome-untrusted`. The excluded routes of 4.4 (`signin`, `sign-in`, `sync-internals`, `feedback`, `whats-new`, `welcome`) are also `pages.reserved`.
- Links inside a page directory are checked like the directory itself: one that leaves the project is `config.path_escape`, and one that points nowhere or back to a directory it is inside is `pages.bad_link` (a code added to the table above).
- A directory under `Pages/` is a page only if it contains `index.html`; other directories are ignored. Directory names are split on case boundaries (`HTMLPage` is `html-page`); underscores and spaces are not converted, so they are `pages.name_invalid`.
- Collisions between aliases and pages cover discovered pages, overrides and the standard and diagnostic routes. An alias of an alias, including itself or a cycle, is `pages.alias_chain` on each alias that points at an alias.
- A capability grant for an alias, a built-in route, a `native` or `disabled` route, or a route that does not exist is `pages.capability_target`. A capability on a `contents.`, `automation.` or `devtools.` prefix, or one the services.md table marks as not granted to pages (`cookies.values`), is `pages.capability_denied`.
- `app.<segments>` capabilities are accepted by shape; whether the application declares them is not checked by the CLI step.
- A route table lists discovered pages and `[pages.overrides]` entries only, as in section 7. With `[extensions] enabled = false` and no entry for `extensions`, the route is written as `disabled`.
- Digests are SHA-256 of the files in the source directory (path, size and content in sorted path order) and of the script file. `config_digest` is the SHA-256 of the resolved file without that key.
- `bdk_version` comes from the caller; `Info.plist` checks run only for the facts passed in. A scheme declared in `CFBundleURLTypes` is `browser.scheme_conflict`; a `NSPrincipalClass` other than `BDKApplication` is a warning because a subclass cannot be recognised from the plist.
- The deny list in 3.4.1 is implemented as name patterns in `denylist.ts`; it applies to `gn_args` keys and to switch-like values.

Start errors, raised by `BDKRuntime.start`, are in [swift-api.md](swift-api.md) section 16.

## 10. Differences from the spec's illustrative configuration

| Spec | Here | Why |
|---|---|---|
| `[services.google]` with `translation`, `safe_browsing`, `geolocation` booleans | `provider = "google"` on each service | One way to choose a provider, as in `architecture.md` 11.1 |
| `[extensions.install] compatible_stores = true` | `stores = []`, a list of named sources | Each store needs its own adapter and legal review (decision D4) |
| `[chromium] revision` and `patches` shown for every project | Source mode only | In prebuilt mode the BDK version fixes the revision |
| "Whether `bdk.toml` can be omitted for fully default projects" is open | It cannot | The browser needs a name, and one required file is simpler than a derivation rule |
| `[ui] default_browser_surfaces = true` with no per-surface keys | The same key, plus `[ui.surfaces]` | The spec requires a mode per surface and gives no key for it outside extensions |
