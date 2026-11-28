# core/config

Validation of `bdk.toml` and the resolver that writes `bdk.resolved.json` (tasks T-CFG-01 and T-CFG-02).
The rules are in [configuration.md](../../../../../docs/planning/sdk/configuration.md) and
[architecture.md](../../../../../docs/planning/architecture.md) section 8. Fixtures are in
[tests/config](../../../../../tests/config/README.md). The schema is
`packages/cli/assets/bdk.schema.json`.

The core is platform neutral: it uses `node:fs` and `node:path` and nothing else. The caller reads
`Info.plist` and the entitlements and passes the facts in.

## Interface

`index.ts` exports `resolveConfig(options)`, `serialiseResolved(resolved)`, `validateConfigText(text, schema)` and
the types `ResolveOptions`, `BundleFacts` and `ResolveResult`. `resolved` is present only when no diagnostic
has severity `error`; warnings may accompany it. Diagnostics are sorted by file, line, column, code and
message. Diagnostic files are relative to the project directory.

`ResolveOptions.bdkVersion` is an addition to the agreed interface: the BDK version for `bdk_version`.
It defaults to `0.0.0`, so the caller must pass the real one.

## Pipeline

| Step | Module | Needs files |
|---|---|---|
| Parse TOML (`smol-toml`) | `validate.ts` | No |
| Validate against the schema (`ajv`, draft 2020-12, strict) and map errors to stable codes | `schema-check.ts` | No |
| Semantic checks: internal URLs, credentials, paths, source-only keys, deny list, scheme, route names, alias shape, capabilities | `checks.ts` | No |
| Discover pages, normalise names, apply overrides, check aliases and grants | `pages.ts` | Yes |
| Discover scripts, assemble, digest | `resolve.ts`, `digest.ts` | Yes |

The file-based steps run only when the first three report no error. Line numbers come from `locator.ts`, a
scanner that records where each key, table header and array element is written, because TOML parsers give
none.

Precedence follows architecture.md 8.2: an explicit `[pages.overrides]` entry beats an auto-discovered
directory, which beats the built-in page. Swift registrations are applied by the application in memory and
are not part of the CLI.

## Not yet accepted

These are marked Added or Conditional in configuration.md and await the product owner
([sdk/README.md](../../../../../docs/planning/sdk/README.md) section 6). They are not in the schema, so the
validator rejects them with the ordinary `config.unknown_key` error.

| Keys | Status |
|---|---|
| `[browser] logo` | Added (S13) |
| `[browser.user_agent]` | Added (S5) |
| `[storage]` | Added (S2) |
| `[chromium.features]` | Added (S9) |
| `[pages] diagnostics` | Added, no S number |
| `[services.credentials]` | Added (S14) |
| `[services.leak_check]` | Added (S16) |
| `[services.components]` | Conditional (D5) |
| `[search]` | Added (S1) |
| `[settings.defaults]` | Added (S7) |
| `[diagnostics]` | Added (S4) |
| `[runtime] locales`, `[chromium] modules` | Later |
| `[services.translation] model_source` | Open |

Accepted on 2026-11-25 (amendments 2.23) and now in the schema: the eight S3 `[ui.surfaces]` keys, with the modes
of ui-requests.md section 4 (the special case that reported them as `config.unknown_key` is gone, T-UIR-15);
`[extensions] developer_mode`, `[extensions.install] blocklist` and `[extensions.bundled]` (T-EXT-14); and
`[services.webauthn] phones` (T-UIR-12). The resolver writes each. A bundled extension is checked on disk
(`extensions.bundled_invalid`, `config.path_escape`) and written in the long form with its directory's digest. The
note `plist.passkeys_phone` is raised only with `phones = true`.

The error codes `browser.logo_invalid`, `search.default_missing`,
`search.url_invalid`, `settings.key_unknown` and `settings.locked` are unreachable until their keys are
accepted. `plist.min_os` and `plist.conflict` need facts the build does not pass yet (the runtime manifest
and the Xcode project).

## Web apps and push (T-PWA-02)

`[web_apps] enabled` and `[services.push] provider` are in the schema, and `web_app_installs` in `[ui.surfaces]`.
The resolver writes `web_apps` and `services.push` with their defaults, and reports the note
`web_apps.push_unconfigured` and the warning `services.push_google` (`checkWebAppsAndPush` in `resolve.ts`, which
`index.ts` exports so that `bdk doctor` can list the same findings). A push provider outside the enum is
`services.provider_unknown`, as for the other providers.

## Defaults for keys that are not accepted

`bdk.resolved.json` has the shape of configuration.md section 7. For the sections whose keys are not
accepted, the resolver writes the documented default, so the file's shape does not change when they are:
`browser.logo = "app-icon"`, `storage` from the name, `services.credentials`, `services.leak_check`,
`settings_defaults = {}`, `diagnostics`, `chromium_features`. `browser.user_agent`
holds `brand_version` from `CFBundleShortVersionString` when `bundleFacts` has it. `search` is omitted
because no default engine exists.

Build-time keys (`[pages] directory`, `auto_discover`, `[automation] directory`, `[codegen]`, `[chromium]`) are
not in the resolved file; the CLI reads them from `bdk.toml`.

## The deny list

`denylist.ts` is the initial Chromium deny list (configuration.md 3.4.1). Its patterns are not yet checked
against the pinned tag and are to be kept in step with the Google-removal audit list (T-PRV-07).
