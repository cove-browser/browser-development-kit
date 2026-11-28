# Versioning

The published form of [repository-and-tooling.md](../planning/repository-and-tooling.md) section 8.1 and
amendment 2.6, for application developers.

## One version for everything

BDK has one SemVer version for the Swift package, the C ABI headers, the `@bdk/*` packages, the `bdk` CLI and
the runtime. They are tied together by the ABI hash, so they are only ever used as a set. A release is the Git
tag `vX.Y.Z`; SwiftPM resolves the Swift package from it, and the package names the runtime it needs.

| Bump | When |
|---|---|
| Major | A breaking change to the Swift API, the C ABI, the `@bdk/*` APIs, the CLI's contract, or the `bdk.toml` schema (a file that was valid stops validating or changes meaning) |
| Minor | Additive API or schema; a new Chromium milestone |
| Patch | Fixes; a Chromium point or security release within the same milestone |

Before 1.0 the version is `0.y.z`, and a breaking change bumps `y`. Everything is experimental until 1.0
([experimental-apis.md](experimental-apis.md)). 1.0 is the release that passes the public v1 gates of the roadmap.

## The Chromium version

The Chromium version is not part of the BDK version. It is in the runtime's manifest
(`chromium.tag`, `chromium.commit`), in `bdk_runtime_manifest_json()` at run time, and in the release notes.
BDK follows the newest Chromium stable, so expect a minor release about every four weeks for a milestone and
patch releases between them ([security-patches.md](security-patches.md)).

## Other numbers

| Number | Rule |
|---|---|
| `bdk.toml` schema | No number of its own; it follows the BDK version. Inside one major version, every earlier valid file still validates. |
| `bdk_abi_version()` | Increases on any incompatible C ABI change (a major bump after 1.0). The ABI hash is checked exactly at start: an application built against other headers fails `bdk_runtime_start` with `abi.hashMismatch` before Chromium starts. |
| `manifest.json` and `bdk.resolved.json` `schema` | Integers for file formats BDK writes and reads; bumped only on an incompatible change. |

## Choosing a version, upgrading and rolling back

An application chooses its BDK version through its Swift package dependency, and ships its own updates; BDK
never updates itself. Moving to a newer release that rolls to a new Chromium milestone snapshots each data
directory first, and moving back to an older milestone restores that snapshot or applies the application's
`[runtime] downgrade` policy ([configuration.md](../planning/sdk/configuration.md) 3.3). Pin an exact version
(`.package(url:…, exact: "X.Y.Z")`) for a release build, so that a newer BDK never arrives unreviewed.
