# Compatibility and known gaps

What a browser built with BDK does differently from Google Chrome on the same Chromium version, and what BDK
does not support. Written 2026-11-25, during M9, before 1.0; each gap that is planned names its task. The
planning documents ([roadmap.md](../planning/roadmap.md), [risks.md](../planning/risks.md)) are the source;
update this page in the same change as they move.

## Platforms

| Area | Supported | Not supported |
|---|---|---|
| Operating system | macOS 14 or later (the Swift package's minimum) | Windows and Linux: a feasibility build is M10 (T-XPL-02); the ABI review is [cross-platform-review.md](cross-platform-review.md) |
| Hardware | Apple Silicon (decision D6) | Intel Macs; no universal runtime |
| Distribution | Developer ID, notarized ([procedure.md](procedure.md)) | The Mac App Store: the browser process does not run under the App Sandbox (unverified) |

## Web platform

| Gap | Why | Status |
|---|---|---|
| H.264, AAC and HEVC do not play; pages are offered VP9, AV1 and Opus | No proprietary decoder is bundled (D7); decoding through macOS's decoders is not built | T-CHR-12 and the legal review ([licence-review.md](licence-review.md) L7) |
| DRM (Widevine) content does not play | Not in v1 (product owner, 2026-11-25) | Nothing in the design rules out an application opt-in later |
| Installable web apps are off unless `[web_apps] enabled = true`; installed apps get no Dock icon, file or protocol handlers | Product-owner decision (amendments 2.24) | T-PWA-01 to T-PWA-05 |
| Push messages need `[services.push] provider = "google"` and the developer's own Google project | Chromium delivers push through Google's service, which BDK contacts only when asked | T-PWA-05 |

## Google services

BDK ships no Google key and contacts Google only for providers an application configures (architecture
invariant 9).

| Chrome has | BDK |
|---|---|
| Sign-in and Sync | Not available; those pages are excluded from the runtime |
| Safe Browsing | The default reputation provider is signed static block lists (D2), weaker than Safe Browsing; `web-risk` or `google` are opt-ins |
| Google's page translation | BDK's own translation step; on-device models where configured |
| Password leak detection | Off; an application can provide one (`[services.leak_check]`) |
| Component updates: CRLSet, CT log list and root store refreshed between releases | Off (D5). The CT log list and root store are those compiled into the runtime; there is no CRLSet revocation; the data is as fresh as the BDK release |
| Chrome Web Store | Off by default; an application enables it as a named source after its own legal review (D4) |

## Extensions

- Manifest V3 only. Chromium 158 disables every non-component MV2 extension in every build.
- `chrome.tabGroups`, the reading list and side search report themselves unavailable.
- The published compatibility matrix is T-EXT-10; until it is complete, an extension API not in the matrix
  should be treated as untested.

## Upgrades

- An application chooses its BDK version; BDK never updates itself.
- Profile snapshots before a newer milestone, and the `[runtime] downgrade` policy when an older engine meets
  newer data, run in the runtime core, on the stub and on the Chromium engine (T-PKG-05), at the first start of the
  process, before Chromium opens its data directory. The engine scenarios prepare a directory as another
  milestone would leave it; a real move between two released runtimes has not been run.

## Not verified on BDK's window yet

- Chromium's own `browser_tests` for sandboxing, site isolation and process locks have not been run against
  BDK's browser window (T-ADP-09). Until they pass, the process-isolation gate is open.
- A page whose view has never been in a window reports itself visible (T-ADP-11).
- Touch ID passkeys, passkeys on a phone and iCloud Keychain passkeys need a signed application with Apple's
  entitlements (T-UIR-11, T-UIR-12); passkeys on a phone are off by default (`[services.webauthn] phones`).

## Not planned

Enterprise policy (`[settings.defaults]` is not a policy mechanism), importing history and passwords from other
browsers (HTML bookmark import and export are supported), and a BDK tab, space or window model: the application
owns its layout.
