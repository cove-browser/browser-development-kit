# Cutting a release by hand

Task T-PKG-06. A BDK release is cut on a developer's Mac, from a clean tree, after the full local test pass
([repository-and-tooling.md](../planning/repository-and-tooling.md) sections 8.1 and 10.2). Nothing here runs
in CI: no signing certificate or notarization key is stored in GitHub, and CI never builds Chromium. An
automated `release.yml`, release-please and a `dev` beta branch are deferred.

One release ships one version of everything: the Swift package, the C ABI headers, the `@bdk/*` packages, the
`bdk` executable and the runtime ([versioning.md](versioning.md)). The tag is `vX.Y.Z`.

## Status

Written on 2026-11-25 and **never run end to end**. What has been run is the dry run (`chromium:release-check`,
section 3) against fixtures and against this repository, where it fails as it should: `chromium/pin.json`
records no published runtime, and `Package.swift` has no binary target yet (T-PKG-04). Signing with a Developer
ID identity, notarization and a real upload have never been done; they need the product owner's credentials.

## 1. Before you start

| Need | Check |
|---|---|
| Apple Silicon Mac with about 120 GB free, the pinned Xcode, the toolchain `pnpm run setup` checks | `pnpm run setup` |
| `BDK_HOME` outside any backup volume (default `~/.bdk`) | `tooling/src/paths.ts` |
| A **Developer ID Application** identity for BDK's own artifacts (the `bdk` executable), in the login keychain | `security find-identity -v -p codesigning` |
| A **second** team's Developer ID identity for the example application (the v1 packaging gate: the runtime signs under another identity) | as above |
| A notarytool keychain profile for each team | `xcrun notarytool store-credentials <profile> --apple-id … --team-id …` once; then `xcrun notarytool history --keychain-profile <profile>` |
| `gh` authenticated with permission to create releases on the repository | `gh auth status` |
| `main` checked out, up to date, clean | `git status --porcelain` prints nothing |

Pick the version from the changes since the last tag ([versioning.md](versioning.md)): a breaking change bumps
major (minor before 1.0), a Chromium milestone or additive API bumps minor, fixes and a Chromium point or
security release bump patch. The Conventional Commit types since the last tag say which.

## 2. Steps

Record the start and end time of each step; the security-patch rehearsal ([security-patches.md](security-patches.md))
uses the same steps and needs the times.

### 2.1 Dry run first

```sh
pnpm exec nx run chromium:release-check -- --version X.Y.Z --skip-swift-resolve
```

Before a runtime is built for this commit, `source-key` (and so `pin-record` or `runtime-digest`) is expected
to fail when the Chromium pin, the patches, the overlay, the core runtime or the GN arguments changed since the
last publish. Every other failure is a reason to stop now: a dirty tree, an existing tag, an ABI hash that does
not match the headers.

### 2.2 Build and package the runtime

```sh
pnpm chromium:sync
pnpm chromium:build                                    # release preset, chromium/gn/release.gn
pnpm chromium:package
pnpm exec nx run chromium:package -- --reproducible-check
```

`chromium:package` refuses a checkout that is not the pin plus the series, a stale ABI hash, and a build whose
third-party notices are Chromium's placeholder (`generate_about_credits`, [licence-review.md](licence-review.md)).
`--reproducible-check` packages the same output twice and compares the manifests and archives (T-CHR-09).

When a runtime for the same source key was built elsewhere (a second machine, or before a toolchain update),
compare them; the ABI hash and the manifest must be equivalent:

```sh
pnpm exec nx run chromium:compare-runtimes -- ~/.bdk/runtimes/<key> /path/to/other/runtime
```

### 2.3 The full local test pass

```sh
pnpm exec nx run chromium:runtime        # points .bdk/runtime at the new runtime
pnpm test:release
pnpm exec nx run core:test-engine        # the conformance suite and engine scenarios on the real runtime
```

`pnpm test:release` runs every `test`, `test-integration`, `test-extensions`, `test-e2e` and `test-release`
target: unit tests, the runtime resolver suite, the robustness suites (T-REL-03: `core:test-release` with
50000 rounds, `arc-style-browser:test-release` with 500) and the example applications' end-to-end tests.
`core:test-engine` needs a logged-in Mac with the screen unlocked. Any failure stops the release; a known
failure that is accepted is written into the release notes with its task ID.

### 2.4 The `bdk` executable

```sh
pnpm cli:package
pnpm exec tsx tooling/src/release/notices.ts --package packages/cli --node \
  --out packages/cli/dist/bdk.artifactbundle/THIRD_PARTY_NOTICES.txt
codesign --force --options runtime --timestamp \
  --sign "Developer ID Application: <BDK team> (<TEAMID>)" packages/cli/dist/bdk
ditto -c -k packages/cli/dist/bdk /tmp/bdk-cli.zip
xcrun notarytool submit /tmp/bdk-cli.zip --keychain-profile <BDK profile> --wait
```

`notices.ts` fails if a bundled package has no licence, a licence outside the accepted list, or no licence
text. A bare Mach-O executable cannot be stapled; Gatekeeper looks the ticket up online. Copy the signed
executable into the artifact bundle where `cli:package` laid it out, replacing the ad hoc one.

### 2.5 Sign and notarize the example under a second team

```sh
pnpm exec nx run minimal-browser:build
pnpm exec tsx packages/cli/src/main.ts build sign --bundle apps/examples/minimal-browser/.build/MinimalBrowser.app \
  --identity "Developer ID Application: <second team> (<TEAMID2>)" --team <TEAMID2>
ditto -c -k --keepParent apps/examples/minimal-browser/.build/MinimalBrowser.app /tmp/minimal.zip
xcrun notarytool submit /tmp/minimal.zip --keychain-profile <second profile> --wait
xcrun stapler staple apps/examples/minimal-browser/.build/MinimalBrowser.app
spctl --assess --type execute -vvv apps/examples/minimal-browser/.build/MinimalBrowser.app
codesign --verify --deep --strict apps/examples/minimal-browser/.build/MinimalBrowser.app
```

Then launch the stapled application from a quarantined copy (download it through a browser, or
`xattr -w com.apple.quarantine …`) and load a page. This is the packaging gate of the roadmap. If notarization
rejects the runtime, `xcrun notarytool log <id>` names each file; fix the signing step (`bdk build sign`,
[sdk/cli.md](../planning/sdk/cli.md) 5.1), never the entitlements of the renderer and GPU helpers, which stay
JIT only.

### 2.6 Publish the runtime and record the checksums

```sh
pnpm chromium:publish                                  # gh release runtime-<key>; writes chromium/pin.json
```

The Swift binary target (T-PKG-04, not built yet) will need its own archive, a zip of an XCFramework, because
SwiftPM binary targets accept only `https` `.zip` archives. When it exists:

```sh
swift package compute-checksum BDKRuntime.xcframework.zip
# edit Package.swift: .binaryTarget(name:url:checksum:) with the release asset URL and this checksum
pnpm exec nx run chromium:release-check -- --version X.Y.Z --swift-archive BDKRuntime.xcframework.zip
```

Every check must pass now. Commit `chromium/pin.json` and `Package.swift` as `chore(release): vX.Y.Z`
through a pull request, as any change to `main` is.

### 2.7 Tag and announce

The order is fixed: the archive is published, then its checksum is committed, then that commit is tagged.
SwiftPM resolves the tag, so `Package.swift` must be right at the tagged commit.

```sh
git switch main && git pull --ff-only
git tag -s vX.Y.Z -m "BDK X.Y.Z"
git push origin vX.Y.Z
gh release create vX.Y.Z --verify-tag --title "BDK X.Y.Z" --notes-file notes.md
```

The notes name the Chromium version and its security fixes (link the Chrome Releases post), web-platform
changes of a new milestone, the `bdk_abi_version` and hash, the runtime's manifest digest, known failures with
their task IDs, and what applications must do (for example, a `bdk.toml` key that changed meaning). A tag is
never moved; a broken release is followed by a new patch release.

### 2.8 After the release

From a machine that has never built BDK: create an empty Swift package depending on
`.package(url: "<repository>", exact: "X.Y.Z")`, run `swift package resolve`, build, and check that
`bdk doctor` reports the runtime as compatible. Record the result in the release's notes.

## 3. The dry run

`pnpm exec nx run chromium:release-check -- [--version X.Y.Z] [--swift-archive <zip>] [--skip-swift-resolve]`
(`tooling/src/release/`) runs every check a release needs that needs no Chromium build, no signing identity
and no upload. It writes nothing to the repository, `.bdk/` or `BDK_HOME`: the published runtime is fetched
into a scratch store that is removed afterwards. Exit 0 only when every check passes or is skipped.

| Check | Fails when |
|---|---|
| `clean-tree` | `git status --porcelain` prints anything |
| `version-tag` | `--version` is not SemVer, or `vX.Y.Z` already exists (skipped without `--version`) |
| `pin-record` | `chromium/pin.json` records no published runtime |
| `abi-hash` | the headers' ABI version or hash differ from what the pin record was published with |
| `source-key` | this tree's inputs name a different runtime than the published one: rebuild, package and publish |
| `runtime-digest` | the published archive does not match its recorded SHA-256, any file differs from the manifest, the manifest digest or ABI hash differs from the pin record, or the runtime has no third-party notices |
| `swift-manifest` | `Package.swift` has no remote binary target, or one that is not `https`, not `.zip`, or has no 64-digit checksum |
| `swift-checksum` | the archive given with `--swift-archive` is not the one a binary target names |
| `swift-resolve` | `swift package resolve` fails (skipped with `--skip-swift-resolve`) |

Tests: `tooling/src/release/checks.test.ts` (fixture repository, a runtime packaged and published to a `file://`
release, a fake `swift` and `git`).

## 4. Not done

| What | Task |
|---|---|
| A run of this procedure from start to finish, with Developer ID signing, notarization, stapling and a published release | T-PKG-06 |
| The SwiftPM binary target and its XCFramework archive; the dry run's `swift-manifest` check fails until then | T-PKG-04 |
| Copying the runtime's `THIRD_PARTY_NOTICES.html` into an application by `bdk build sign` (or the Xcode build phase) | T-REL-02 |
| `release.yml`, release-please, the `dev` branch and beta prereleases | deferred (amendments 2.8) |
