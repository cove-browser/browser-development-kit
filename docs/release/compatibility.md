# Compatibility and known gaps

What a browser built with BDK does differently from Google Chrome on the same Chromium version, and what BDK
does not support. Written 2026-11-25 and brought up to date 2026-11-27, during M9, before 1.0; each gap that is planned names its task. The
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
| Installable web apps are off unless `[web_apps] enabled = true`; installed apps get no Dock icon, app shim, file or protocol handlers, and links do not open in an installed app | Product-owner decision (amendments 2.24); operating-system integration is not in v1 ([web-apps.md](../planning/sdk/web-apps.md) 8) | Not planned for v1 |
| Push messages arrive only with `[services.push] provider = "google"`, and only while the browser runs | Chromium's push client registers the device with Google as Chrome does and keeps a connection to Google's messaging host; BDK contacts it only when asked. No Google project or key is needed. Whether Google's terms cover a browser that is not Chrome registering this way is the developer's question | Built (T-PWA-05) |
| Periodic Background Sync events are at least 12 hours apart, and no sync event runs while the browser is closed | Chromium's minimum interval; BDK keeps no process in the background | As Chromium |
| Payment Request works only with a payment handler (a service-worker payment app); there is no card form and no Google Pay, and a request for shipping or payer details is refused | BDK has no wallet, address book or contact picker of its own ([ui-requests.md](../planning/sdk/ui-requests.md) 5.22) | Built (T-UIR-14); a handler must show its own page, as Chromium requires |
| Secure Payment Confirmation and the Digital Credentials API are absent | Their UI is Chromium's views dialogs; BDK's requests for them are designed and not built. Secure Payment Confirmation verifies with Touch ID, which needs a signed application (T-UIR-11); a digital credential on a Mac comes from a phone, which needs `[services.webauthn] phones` | T-UIR-14 |
| Web Bluetooth is absent unless the application's `Info.plist` has `NSBluetoothAlwaysUsageDescription` | macOS ends a process that uses Bluetooth without declaring why | As designed ([ui-requests.md](../planning/sdk/ui-requests.md) 5.16) |

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
- Touch ID passkeys are kept in the application's own keychain access group, `<team>.<bundle id>.webauthn`:
  the application needs the `keychain-access-groups` entitlement for it and the provisioning profile macOS
  requires for that entitlement (T-UIR-11; not yet run on a signed build). Passkeys on a phone and iCloud
  Keychain passkeys need a signed application with Apple's entitlements (T-UIR-12); passkeys on a phone are off
  by default (`[services.webauthn] phones`).

## Not planned

Enterprise policy (`[settings.defaults]` is not a policy mechanism), importing history and passwords from other
browsers (HTML bookmark import and export are supported), and a BDK tab, space or window model: the application
owns its layout.
