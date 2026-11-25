# Experimental APIs

What may change without a major version, and how to tell. Written 2026-11-25, before 1.0.

## Before 1.0, everything is experimental

Until 1.0 the version is `0.y.z` ([versioning.md](versioning.md)) and any `0.y` release may break the Swift API,
the `@bdk/*` APIs, the CLI's flags and output, the C ABI and the meaning of a `bdk.toml` key. Each break is in
the release notes. Pin an exact version and read the notes before moving.

What holds even before 1.0:

- **The ABI check.** The runtime refuses to start for an application built against other headers
  (`abi.versionMismatch`, `abi.hashMismatch`), before Chromium starts. A mismatch is never a crash later.
- **`bdk.toml` within a release line.** A file valid for `0.y.0` stays valid for every `0.y.z`.
- **Stored identifiers.** Profile, bookmark and extension identifiers survive relaunches, and
  `[storage] directory` and `keychain_item` keep user data across a rename of the browser.

## Marked experimental after 1.0

Some surfaces will stay experimental after 1.0, and say so where they are declared:

| Mark | Where | Means |
|---|---|---|
| `PROVISIONAL` | C ABI headers (`packages/core/include/bdk`) | The signature was chosen where the specification gave only the family (conventions.md, "Decisions"). It can change in a minor release until the mark is removed. |
| Status column `Added` | [sdk/](../planning/sdk/README.md) documents | Proposed and not yet accepted by the product owner. Not built; may never be. |
| Status `Plan, M<n>` with no "built" note | sdk documents | Designed and not yet built, or built only in part; the entry names the task that finishes it. |
| `Later` | sdk documents | Not in v1. |
| `chromium_features` / `[chromium.features]` | `bdk.toml` | Chromium's own feature switches. They follow Chromium, change with every milestone, and are not covered by BDK's compatibility promise. |
| Automation scripts' host APIs | `@bdk/automation`, javascript-api.md 5 | The budget and the host's limits (`[automation] cpu_budget_ms`) may tighten. |
| Engine facts | `EngineFacts`, `runtime.info` | Report what the current Chromium supports (for example `manifest_versions`). Values change with the milestone. |

## Behaviour that follows Chromium

Anything BDK passes through from Chromium unchanged follows Chromium's own compatibility, not BDK's: web
platform features, DevTools protocol domains (`debug.*`), the extension APIs' behaviour, and default settings.
A Chromium milestone is a BDK minor release, and its notes list web-platform changes that reach applications.
