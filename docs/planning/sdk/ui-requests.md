# BDK browser-owned UI requests

Status: proposal, 2026-11-15. The pattern is from [../architecture.md](../architecture.md) section 7. This document lists every surface, what a request carries, how it is completed, what the default presenter does and which rule cannot be overridden. Names are placeholders. Status labels are in [README.md](README.md) section 2. Tasks: T-UIR-01 to T-UIR-06, T-SWF-04, T-MAC-04, T-MAC-05.

A browser-owned surface is UI that Chromium needs shown on behalf of a page, an extension or itself. The application did not ask for it and cannot predict it.

## 1. The pattern

```text
Chromium asks for UI ──► BDK builds a typed request ──► mode in bdk.toml?
                                                          │
                              disabled ◄──────────────────┤
                              (deny or cancel; API         │ default or custom
                               reports unavailable)        ▼
                                                   handler registered?
                                                    │ no            │ yes
                                                    ▼               ▼
                                             default presenter   handler(request)
                                                                  │ .handled     the application presents and completes
                                                                  │ .useDefault  default presenter
                                                                  │ .reject      deny or cancel, where safe
```

Modes are set under `[ui.surfaces]` and `[extensions.ui]` ([configuration.md](configuration.md) 3.6 and 3.7). Handlers are registered as in [swift-api.md](swift-api.md) section 10.

## 2. The request object

Every request type shares this base.

```swift
public protocol BDKUIRequest: AnyObject, Sendable {
    var id: RequestID { get }
    var contents: BDKWebContents? { get }       // the page it concerns, if any
    var window: BDKWindow? { get }              // the window it should appear in
    var profile: BDKProfile { get }
    var initiator: Initiator { get }            // .page(origin:), .extension(id:), .browser
    var anchor: CGRect? { get }                 // in window coordinates, when the request has a natural position
    var isCompleted: Bool { get }
    var cancellation: Cancellation { get }      // fires if Chromium withdraws the request
    func useDefaultPresenter()                  // hand it back after taking it
}
```

| Rule | Detail |
|---|---|
| One completion | A request is completed once. A second completion throws `ui.requestCompleted`. |
| Withdrawal | Chromium can withdraw a request: the page navigated, the page closed, the request was superseded. `cancellation` fires and the application dismisses its UI. Completing a withdrawn request is a no-op. |
| Dropped requests | A request the application took and released without completing is cancelled. It is never granted. |
| Page moves | If the page moves to another window while a request is open, the request stays valid and `window` changes. BDK closes its own default presenters before a move and re-presents after. |
| Queueing | Chromium queues requests of the same kind for the same page. The handler sees them one at a time: the next is delivered after the previous is completed, used by default, rejected, released or withdrawn. A request withdrawn while it waits is never delivered. Requests that are notices (nothing to answer, such as leaving fullscreen) are delivered at once. |
| Hidden pages | A request from a page that is not visible is still delivered. The application may defer showing it; the default presenters defer until the page is shown. |

## 3. Rules no configuration or handler can override

From `architecture.md` section 7 and invariant 6.

| Rule | How it is enforced |
|---|---|
| A security interstitial cannot be disabled, auto-dismissed or skipped | The interstitial surfaces accept `default` and `custom` only; the runtime refuses a configuration that says otherwise (`ui.surfaceMode`). They are completed by `go_back` or `proceed` and nothing else. `proceed` needs a gesture and a request whose `isOverridable` is true, and a request that does not say is not overridable (`ui.notOverridable`). `.reject`, a dropped request and a released one all complete as `go_back`. |
| A permission, an extension install, a credential fill or a dangerous download cannot be granted without a user gesture | The granting completion takes the `NSEvent` that confirmed it. The core refuses a granting completion with no `NSEvent` (`ui.gestureRequired`); the engine also checks that it is a recent mouse-up or key-down delivered to a window of this application. The granting completion of each surface is named in section 5 (`grant`, `keep`, `save`, `accept` for suggestions, `proceed`, `select`, `share`, `open`). |
| A custom presenter is given the same facts the default shows | Requests carry the origin, the permission text, the risk text and the extension's permission warnings. A grant from a presenter that did not read them is still a grant, so review handlers accordingly. |
| `disabled` never means allowed | A disabled surface denies. The request finishes as disabled with no handler called, and the web or extension API behind the surface reports itself unavailable. |
| A handler cannot widen what was asked | Completions accept a subset of what the request asked for. A permission `grant` that names a kind the request did not ask for fails with `ui.widensRequest` and leaves the request open. |
| A page cannot answer its own request | Requests are delivered to Swift only. No internal-page capability and no automation capability completes a UI request. |

About the gesture check: the application runs in the browser process and is trusted, so this is not a defence against the application. It stops accidental auto-grant in handler code and makes it structurally impossible for a page or an automation session to grant. That an `NSEvent` can be attested well enough for this is unverified.

## 4. Surfaces

The key is what goes in `[ui.surfaces]`. "Modes" lists what the surface allows: D default, C custom, X disabled.

| Key | Surface | Modes | Status | Milestone, task |
|---|---|---|---|---|
| `permissions` | Site permission prompts | D C | Plan | M3, T-UIR-02 |
| `javascript_dialogs` | `alert`, `confirm`, `prompt`, `beforeunload` | D C | Plan | M3, T-UIR-02 |
| `http_auth` | HTTP and proxy authentication | D C | Plan | M3, T-UIR-02 |
| `client_certificates` | Client certificate selection | D C | Added (S3) | — |
| `file_pickers` | File and directory choosers | D C X | Plan | M3, T-UIR-02 |
| `context_menus` | Page context menus | D C X | Plan | M3, T-UIR-03 |
| `new_windows` | Popups, undocked DevTools, picture-in-picture, extension windows | D C | Plan | M2, T-ADP-05; M3, T-UIR-03 |
| `downloads` | Download start, progress, warnings | D C X | Plan | M3, T-UIR-03 |
| `passwords` | Save, update, generate; leaked-password warning (Added, S16) | D C X | Plan | M3, T-UIR-04 |
| `autofill` | Save address or card; fill confirmation | D C X | Plan | M3, T-UIR-04 |
| `autofill_suggestions` | The suggestion list under a form field | D C | Added (S3) | — |
| `printing` | Print preview and the system print dialog | D C X | Plan | M3, T-UIR-04 |
| `translation` | Offer, progress, result, errors, preferences | D C X | Plan | M6, T-PRV-03 |
| `find` | Find bar | D C | Plan | M3, T-UIR-06 |
| `fullscreen` | HTML fullscreen | D C X | Plan | M2, T-MAC-05; M3, T-UIR-06 |
| `exclusive_access` | Keyboard-lock and pointer-lock notices | D C | Added (S3) | — |
| `notifications` | Web notifications | D C X | Plan | M3, T-UIR-06 |
| `certificate_errors` | Certificate interstitials | D C | Plan | M3, T-UIR-05 |
| `reputation_warnings` | Malware, phishing and download warnings | D C | Plan | M3, T-UIR-05 |
| `devtools` | The DevTools front end | D C X | Plan | M7, T-AUT-05 |
| `device_choosers` | WebUSB, Web Bluetooth, WebHID, Web Serial | D C X | Added (S3) | — |
| `screen_capture` | The `getDisplayMedia` source picker | D C X | Added (S3) | — |
| `webauthn` | Passkey and security-key dialogs | D C X | Added (S3) | — |
| `external_protocols` | "Open this link in another application?" | D C X | Added (S3) | — |
| `page_health` | Unresponsive page; blocked popups | D C | Added (S3) | — |
| Extension surfaces | Actions, popups, side panels, menus, options, install prompts | See [extensions.md](extensions.md) | Plan | M5 |

`[ui] default_browser_surfaces = false` sets every key above to `custom`, which means the application must register a handler for each before start.

## 5. Each surface

For every surface: what the request carries, how it is completed, what the default presenter does, and what `disabled` means.

### 5.1 Permissions

```swift
public final class PermissionRequest: BDKUIRequest {
    public let origin: URL                      // the requesting origin
    public let embeddingOrigin: URL?            // the top-level origin, when different
    public let kinds: Set<PermissionKind>       // .camera, .microphone, .geolocation, .notifications, .clipboardRead,
                                                // .midi, .storageAccess, .localFonts, .windowManagement, .idleDetection, …
    public let text: PermissionText             // the wording the default presenter shows
    public let offersOneTime: Bool
    public func grant(_ kinds: Set<PermissionKind>? = nil, duration: GrantDuration = .persistent,
                      confirmedBy event: NSEvent) throws        // .persistent, .thisVisit
    public func deny(persist: Bool = true)
    public func dismiss()                                        // no decision; the site may ask again
}
```

| | |
|---|---|
| Default presenter | Chromium's prompt, anchored to the window's `addressBar` anchor, or to the top of the page view |
| Rule | A grant needs a user gesture. It may cover fewer kinds than were asked, never more. |
| Operating-system permission | BDK asks macOS for camera, microphone or location access after the site grant and before capture. The application supplies only the usage strings in `Info.plist`. |
| Persisted decisions | Stored in the profile's site settings and visible through `permissions.*` commands |

### 5.2 JavaScript dialogs

```swift
public final class JavaScriptDialogRequest: BDKUIRequest {
    public let kind: Kind                        // .alert, .confirm, .prompt, .beforeUnload
    public let origin: URL
    public let message: String
    public let defaultPromptText: String?
    public let isReload: Bool                    // for .beforeUnload
    public let offersSuppression: Bool           // Chromium offers "prevent further dialogs" after repeats
    public func accept(text: String? = nil, suppressFurther: Bool = false)
    public func cancel(suppressFurther: Bool = false)
}
```

| | |
|---|---|
| Default presenter | A dialog modal to the page view, not to the window |
| Rule | A dialog from a page that is not foremost is queued, as in Chromium. The page's title must be shown with the message so a page cannot pass a dialog off as the browser's. |
| `beforeUnload` | The message is Chromium's fixed text. The page's own string is not shown. |

### 5.3 Authentication and client certificates

```swift
public final class HTTPAuthRequest: BDKUIRequest {
    public let challenger: URL                   // the host asking
    public let realm: String?
    public let isProxy: Bool
    public let scheme: String                    // "basic", "digest", "ntlm", "negotiate"
    public let savedCredentialsAvailable: Bool
    public func provide(username: String, password: String)
    public func cancel()
}

public final class ClientCertificateRequest: BDKUIRequest {       // Added
    public let host: String
    public let certificates: [CertificateSummary]                 // from the Keychain
    public func select(_ certificate: CertificateSummary)
    public func cancel()
}
```

Default presenters: a sheet on the page view for credentials; Chromium's certificate selector for client certificates.

### 5.4 File pickers

```swift
public final class FilePickerRequest: BDKUIRequest {
    public let mode: Mode                        // .open, .openMultiple, .directory, .save
    public let acceptedTypes: [UTType]
    public let suggestedName: String?
    public let isFileSystemAccessAPI: Bool
    public func choose(_ urls: [URL])
    public func cancel()
}
```

| | |
|---|---|
| Default presenter | `NSOpenPanel` or `NSSavePanel` as a sheet |
| `disabled` | File inputs do nothing and the File System Access API rejects |
| Rule | The chosen paths are granted to that page only, as in Chromium |

### 5.5 Context menus

```swift
public final class ContextMenuRequest: BDKUIRequest {
    public let location: CGPoint                 // in the page view
    public let context: Context                  // link URL, image URL, media state, selection text, editable,
                                                 // misspelled word and suggestions, frame URL, page URL
    public let items: [MenuItem]                 // the complete default menu, extension items included
    public func show(_ items: [MenuItem], at point: CGPoint? = nil)   // BDK builds an NSMenu from these
    public func perform(_ item: MenuItem)        // run an item from the application's own menu
    public func dismiss()
}

public struct MenuItem: Identifiable {
    public let id: ID
    public let kind: Kind                        // .command, .separator, .submenu, .checkbox, .radio
    public let role: Role?                       // .openLinkInNewPage, .copy, .paste, .inspect, .spellingSuggestion, …
    public let title: String
    public let icon: NSImage?
    public let isEnabled: Bool, isChecked: Bool
    public let children: [MenuItem]
    public let source: Source                    // .browser, .extension(id), .application
}
```

| | |
|---|---|
| Default presenter | An `NSMenu` with Chromium's items for the context and the extensions' items |
| Custom, three levels | Reorder, remove and add items, then `show`. Or build a completely native menu and call `perform` for the items that are BDK's. Or ignore `items` entirely. |
| Items that create pages | "Open link in new page" and similar do not open anything themselves. Performing one raises a new-window request (5.6), so the application's layout decides. |
| Extension items | Performing an extension's item grants `activeTab` for that page and fires its event. The application cannot run an extension item without that. |
| `disabled` | No menu. The page still receives its `contextmenu` event. |

### 5.6 New windows

Everything that would have made Chromium open a window or a tab.

```swift
public final class NewWindowRequest: BDKUIRequest {
    public let kind: Kind                        // .popup, .newPage, .devTools, .pictureInPicture, .extensionWindow, .application
    public let disposition: Disposition          // .foreground, .background, .newWindow, .popup, .privateWindow
    public let targetURL: URL?
    public let opener: BDKWebContents?
    public let features: WindowFeatures          // requested size and position, from window.open
    public let isUserGesture: Bool
    public let newContents: BDKWebContents       // already created; has an opener relationship where the web requires one
    public func accept()                         // the application has placed newContents.view, or will
    public func reject()                         // destroys newContents
}
```

| Kind | Arises from |
|---|---|
| `.popup` | `window.open` with features |
| `.newPage` | `target="_blank"`, a middle click, a context-menu item, `chrome.tabs.create` |
| `.devTools` | An undocked DevTools front end |
| `.pictureInPicture` | Document picture-in-picture |
| `.extensionWindow` | `chrome.windows.create` |
| `.application` | Chromium's own code asking for a page, such as a link in a built-in settings page |

| | |
|---|---|
| Default presenter | A plain `NSWindow` holding the new page's view, sized from the requested features |
| Why the page already exists | `window.open` returns a window object synchronously, with an opener link the web depends on. BDK cannot defer creation. Until the application attaches its view, the new page lives in the parking context. |
| Popup blocking | Chromium's popup blocker runs first. A blocked popup is a `page_health` request (5.19), not a new-window request. |
| No `disabled` | Rejecting every request breaks sign-in flows and payment pages. An application that wants that writes a handler that rejects. |

### 5.7 Downloads

```swift
public final class DownloadRequest: BDKUIRequest {
    public let download: Download                // url, suggested name, MIME type, size, referrer, danger
    public let needsLocation: Bool               // the "ask where to save" setting, or a save-as
    public func proceed(to destination: URL? = nil)
    public func cancel()
}

public final class DangerousDownloadRequest: BDKUIRequest {
    public let download: Download
    public let danger: DangerKind                // .dangerousFileType, .dangerousURL, .dangerousContent, .uncommon, .insecure
    public let text: WarningText
    public func keep(confirmedBy event: NSEvent) throws
    public func discard()
}
```

| | |
|---|---|
| Default presenter | A save panel when a location is needed; a downloads popover attached to the window's `downloads` anchor that lists progress and offers open, reveal, pause and cancel |
| Progress | Not a request. Progress is the `downloads` service's event stream, which the default presenter and any application UI both read. |
| Rule | Keeping a dangerous download needs a user gesture. The warning cannot be skipped by mode. |
| `disabled` | Downloads are cancelled when they start |

### 5.8 Passwords and autofill

```swift
public final class PasswordSaveRequest: BDKUIRequest {
    public let origin: URL
    public let username: String
    public let isUpdate: Bool
    public func save(confirmedBy event: NSEvent) throws
    public func never()                          // never for this site
    public func dismiss()
}

public final class AutofillSaveRequest: BDKUIRequest {
    public let kind: Kind                        // .address, .card
    public let summary: String                   // what would be saved, masked for cards
    public func save(confirmedBy event: NSEvent) throws
    public func dismiss()
}

public final class AutofillSuggestionsRequest: BDKUIRequest {     // Added
    public let fieldRect: CGRect
    public let suggestions: [Suggestion]         // label, sublabel, icon, kind: .password, .address, .card, .generatedPassword
    public func accept(_ suggestion: Suggestion, confirmedBy event: NSEvent) throws
    public func preview(_ suggestion: Suggestion?)
    public func dismiss()
}

public final class LeakedPasswordRequest: BDKUIRequest {          // Added (S16); raised only with a leak-check provider
    public let origin: URL
    public let username: String
    public let isSaved: Bool                     // the login is in the store and now appears in passwords.issues
    public let provider: ProviderDescriptor      // who reported it
    public func dismiss()
}
```

| | |
|---|---|
| Default presenters | Chromium's save and update bubbles at the `addressBar` anchor; Chromium's suggestion popup under the field; a sheet on the page view for the leaked-password warning, which names the site and advises changing the password |
| Leaked-password warning | Raised after a sign-in when the provider reports the submitted login as leaked ([services.md](services.md) 5.9). It informs; it grants nothing and has no accepting completion. Opening a password-management page from it is the application's choice. |
| Storage | Passwords are encrypted with a key in the Keychain item named in `[storage]`. A presenter never sees a stored password in clear. |
| Rule | Filling a credential or a card into a page needs a user gesture on the suggestion. Revealing a stored password is a service command that needs operating-system authentication ([services.md](services.md) 5.9). |
| `disabled` | Nothing is offered and nothing is filled. Stored data stays. |

### 5.9 Printing

```swift
public final class PrintRequest: BDKUIRequest {
    public let isUserInitiated: Bool             // false for window.print()
    public let previewContents: BDKWebContents   // Chromium's print preview, as a page the application may host
    public func printWithSystemDialog()
    public func savePDF(to url: URL)
    public func cancel()
}
```

| | |
|---|---|
| Default presenter | Chromium's print preview as a dialog modal to the page view |
| Custom | Host `previewContents.view` in the application's own sheet, or skip the preview and go to the system dialog |
| Headless PDF | `page.capture.pdf` produces a PDF with no request and no UI |
| `disabled` | `window.print()` does nothing and the print command is unavailable |

### 5.10 Translation

```swift
public final class TranslationRequest: BDKUIRequest {
    public let phase: Phase                      // .offer, .translating(progress:), .translated, .failed(BDKError)
    public let detected: Language?
    public let target: Language
    public let languages: [Language]             // what the provider supports
    public let provider: ProviderDescriptor      // name and privacy model, to show the user
    public func translate(from: Language? = nil, to: Language? = nil)
    public func revert()
    public func always(for language: Language)
    public func never(for language: Language)
    public func neverForThisSite()
    public func dismiss()
}
```

| | |
|---|---|
| Default presenter | An offer at the `addressBar` anchor, an indicator while translated, and the language choices |
| Custom | The application gets one request per page that moves through the phases, and may render it anywhere |
| Rule | When the provider is remote, the first offer names it and says page text will be sent to it |
| `disabled` | The translation service is off for the product: no detection offer, no command, and `translation.*` commands return `provider.unavailable` |

### 5.11 Find in page

```swift
public final class FindRequest: BDKUIRequest {
    public let initialText: String?              // the selection, or the last search
    public let session: FindSession              // the same object as page.find
    public func close()
}
```

Default presenter: a find bar at the top trailing corner of the page view. Raised by `page.find.present()` and by the page command `.find`. An application with its own find field uses `page.find` directly and never sees this request.

### 5.12 Fullscreen and exclusive access

```swift
public final class FullscreenRequest: BDKUIRequest {
    public let origin: URL
    public let isEntering: Bool
    public func allow()                          // the application has made, or will make, the page view fill the screen
    public func deny()
}

public final class ExclusiveAccessRequest: BDKUIRequest {         // Added
    public let kind: Kind                        // .keyboardLock, .pointerLock
    public let origin: URL
    public let exitInstruction: String           // "Press and hold Esc to exit"
    public func acknowledge()                    // the application showed the instruction
}
```

| | |
|---|---|
| Default presenter | Makes the page's window fullscreen with the page view filling it, and shows Chromium's exit instruction |
| Rule | The exit instruction must be shown, and Escape must exit. BDK handles Escape itself; a custom presenter cannot prevent it. |
| `disabled` (fullscreen) | The Fullscreen API rejects |

### 5.13 Notifications

```swift
public final class NotificationRequest: BDKUIRequest {
    public let notification: WebNotification     // title, body, icon, origin, tag, actions, requireInteraction
    public func shown()                          // the application displayed it
    public func clicked(action: String? = nil)
    public func closed(byUser: Bool)
}
```

Default presenter: the macOS notification centre, attributed to the application. `disabled`: the Notifications API reports permission denied and extension notifications are dropped.

### 5.14 Interstitials

```swift
public final class CertificateErrorRequest: BDKUIRequest {
    public let url: URL
    public let error: CertificateError           // .dateInvalid, .authorityInvalid, .commonNameInvalid, .revoked, .pinningFailure, …
    public let certificate: CertificateSummary
    public let isOverridable: Bool               // false for HSTS hosts, pinning failures, revocation
    public let text: InterstitialText
    public func goBack()
    public func proceed(confirmedBy event: NSEvent) throws        // throws when isOverridable is false
}

public final class ReputationWarningRequest: BDKUIRequest {
    public let url: URL
    public let verdict: Verdict                  // .malware, .phishing, .unwantedSoftware
    public let provider: ProviderDescriptor
    public let isOverridable: Bool
    public let text: InterstitialText
    public func goBack()
    public func proceed(confirmedBy event: NSEvent) throws
}
```

| | |
|---|---|
| Default presenter | Chromium's interstitial page, in the page |
| Custom | The application draws its own warning over the page view. The navigation stays blocked underneath until the request is completed. |
| Rules | No `disabled` mode. `.reject` and a dropped request both mean "go back". Proceeding needs a user gesture and is refused where Chromium refuses it. |

### 5.15 DevTools

```swift
public final class DevToolsRequest: BDKUIRequest {
    public let inspected: BDKWebContents
    public let frontend: BDKWebContents          // the DevTools front end, as a page
    public let preferredDock: Dock               // .undocked, .bottom, .right, .left — the user's last choice
    public func accept()                         // the application has placed frontend.view
    public func reject()
}
```

Default presenter: an undocked window. Custom: the application docks `frontend.view` beside the inspected page in its own layout. `disabled`: no front end; the protocol is governed separately by `[devtools] protocol`.

### 5.16 Choosers (Added)

```swift
public final class DeviceChooserRequest: BDKUIRequest {
    public let kind: Kind                        // .usb, .bluetooth, .hid, .serial
    public let origin: URL
    public var devices: Devices { get }          // AsyncSequence; devices appear and disappear while the chooser is open
    public func select(_ device: Device, confirmedBy event: NSEvent) throws
    public func cancel()
}

public final class ScreenCaptureRequest: BDKUIRequest {
    public let origin: URL
    public let requestsAudio: Bool
    public let sources: Sources                  // AsyncSequence of screens, windows and pages, with thumbnails
    public func share(_ source: CaptureSource, audio: Bool, confirmedBy event: NSEvent) throws
    public func cancel()
}
```

Default presenters: Chromium's chooser dialogs and media picker. `disabled`: the API rejects. While a page is capturing, `page.capture` reports it, and the application must show an indicator; the default presenter for that is a small "sharing" bar in the page's window.

### 5.17 External protocols (Added)

```swift
public final class ExternalProtocolRequest: BDKUIRequest {
    public let url: URL                          // mailto:, zoommtg:, …
    public let initiatingOrigin: URL?
    public let handlerName: String?              // the application macOS would open
    public let isUserGesture: Bool
    public func open(remember: Bool = false, confirmedBy event: NSEvent) throws
    public func cancel()
}
```

Default presenter: Chromium's confirmation dialog. `disabled`: external schemes are blocked.

### 5.18 WebAuthn (Added)

```swift
public final class WebAuthnRequest: BDKUIRequest {
    public let relyingParty: String
    public let operation: Operation              // .create, .get
    public let step: Step                        // .chooseMechanism, .touchSecurityKey, .enterPIN, .chooseAccount, .error
    public let mechanisms: [Mechanism]
    public func choose(_ mechanism: Mechanism)
    public func providePIN(_ pin: String)
    public func chooseAccount(_ account: Account)
    public func cancel()
}
```

Default presenter: Chromium's WebAuthn dialog. Platform passkeys on macOS need entitlements and an associated domain that belong to the application's Apple developer account; whether a BDK application can use the system passkey provider is unverified. Security keys do not depend on that. `disabled`: `navigator.credentials` rejects for public-key credentials.

### 5.19 Page health (Added)

```swift
public final class UnresponsivePageRequest: BDKUIRequest {
    public func wait()
    public func terminate()                      // kills the renderer; the page then reports .processGone
}

public final class BlockedPopupRequest: BDKUIRequest {
    public let urls: [URL]
    public func allowOnce(_ url: URL)            // raises a new-window request
    public func alwaysAllow()                    // a site setting
    public func dismiss()
}
```

Default presenters: a dialog offering to wait or stop the page; a small notice at the `addressBar` anchor. A crashed renderer is not a request. It is the `processGone` event, and what the user sees is the application's choice.

### 5.20 Extension surfaces

Action state, popups, side panels, extension context-menu items, options pages, install and permission prompts, and the default extensions menu are in [extensions.md](extensions.md) section 4. They follow sections 1 to 3 of this document without exception.

## 6. Default presenters

| Question | Answer |
|---|---|
| What are they built from? | Preferred: Chromium's existing dialog and bubble views, anchored to rectangles the BDK window supplies. Fallback: small AppKit presenters. Decided by task T-MAC-04; risk R7. The request types above do not change either way. |
| Where do they appear? | At the window's registered anchor for their role, or relative to the page view ([swift-api.md](swift-api.md) 8.1). |
| What do they look like? | Chromium's own appearance, following the system light or dark setting, with the browser's name from `bdk.toml` in their text. Theming them is Later. |
| What language? | The system language, from Chromium's locale resources. |
| Can an application call one directly? | Only by returning `.useDefault` or calling `useDefaultPresenter()` on a request. There is no free-standing "show a permission prompt" call. |
| An application with no toolbar | Every default presenter works with no anchor. The extensions menu attaches to the window ([extensions.md](extensions.md) 4.7). |

## 7. Testing a handler

Each request type has a fixture in `BDKTestSupport`, raised against the stub runtime ([swift-api.md](swift-api.md) section 17). The stub records the completion, so a test can assert that a handler denied, deferred to the default or granted with a gesture. The stub enforces the rules in section 3, so a handler that grants without a gesture fails in a unit test, before it meets Chromium.

## 8. The Swift binding (T-SWF-04)

Written against the stub; the real runtime's payloads are the engine tasks' (T-UIR-02 to T-UIR-06).

| Topic | Decision |
|---|---|
| Completions | The ABI names three: `"grant"` (needs the confirming `NSEvent`; permission grants, a kept dangerous download, a saved password or card, an accepted suggestion, a proceed past an interstitial, opening an external protocol), `"accept"` (every other positive answer) and `"deny"` (every negative one, including dismiss and go back). The arguments JSON distinguishes the rest. |
| Arguments | `permissions`: grant `{"kinds":[…],"duration":"persistent"\|"thisVisit"}`, deny `{"persist":bool}` or `{"dismissed":true,"persist":false}`. `javascript_dialogs`: accept `{"text"?,"suppressFurther"}`, deny `{"suppressFurther"}`. `http_auth`: accept `{"username","password"}`. `client_certificates`: accept `{"certificate":id}`. `file_pickers`: accept `{"paths":[…]}`. `context_menus`: accept `{"action":"show","items":[…],"at"?}` or `{"action":"perform","item":id}`. `downloads`: accept `{"destination"?}`. `printing`: accept `{"action":"systemDialog"}` or `{"action":"savePDF","path"}`. `find`: accept `{"action":"close"}`. `passwords`, `autofill`: deny `{"never":true}` or `{"dismissed":true}`. `autofill_suggestions`: grant `{"suggestion":id}`. `notifications`: accept `{"event":"clicked","action"?}`, deny `{"event":"closed","byUser"}`. `external_protocols`: grant `{"remember"}`. |
| Payload | The field names of section 5 in camelCase. Handles (`opener`, `newContents`, `previewContents`, `inspected`, `frontend`) are decimal strings. Two kinds on one surface are told apart by the payload: `downloads` with a `danger` field is a dangerous download, `passwords` with `"kind":"leaked"` is the leaked-password warning. |
| Narrowing | Checked in Swift before the ABI call: a grant for kinds the request did not ask for (`ui.kindsNotRequested`), a certificate, menu item or suggestion it did not offer, more than one file for a single-file picker, `proceed` on a non-overridable interstitial (`ui.notOverridable`). |
| Gaps | The ABI cannot report a notification as shown without completing it, preview an autofill suggestion, or complete an `NSEvent`-less `grant`. `shown()` is local, `preview` is absent. |

### 8.1 What the engine sends and does (T-UIR-02, T-UIR-06, 2026-11-19)

Settled against the Chromium engine (`chromium/bdk/engine/ui_*.cc`). Where this differs from the Swift decoders above, the decoders already match.

| Surface | Payload fields the engine sends | Outcomes |
|---|---|---|
| `permissions` | `origin`, `embeddingOrigin` (when different), `kinds` (camera, microphone, geolocation, notifications, clipboardRead, midi, storageAccess, localFonts, windowManagement, idleDetection, and Chromium's others by name), `text` `{title: "<origin> wants to", message: <Chromium's fragment>}`, `offersOneTime` | `grant` needs a recent mouse-up or key-down `NSEvent` of one of this application's windows (the engine checks type, age under 10 s and window; anything less is a dismissal, nothing granted). `kinds` must equal what was asked: the request manager decides a prompt's requests together, so a grant of fewer kinds grants nothing (dismissed). `duration` `thisVisit` is Chromium's one-time grant. `deny` `{persist:true}` blocks for the site, `{persist:false}` and a rejected or dropped request leave no decision and no embargo count, `{dismissed:true}` counts as a dismissal. Withdrawn when the manager drops the prompt (navigation, close). The decision lands in the profile's content settings, so a persistent profile keeps it and a private profile never shares it. Camera and microphone arrive here through Chrome's media capture logic. A prompt for a page that is not visible waits for it to be (Chromium's rule). |
| `javascript_dialogs` | `kind`, `origin`, `pageTitle`, `message`, `defaultPromptText` (prompt), `isReload`, `offersSuppression` (from a page's second dialog since its last navigation) | `accept` `{text?, suppressFurther}` and `deny` `{suppressFurther}`. Suppressing silences the page's later dialogs until it navigates; the engine honours it even if the dialog did not offer it. `beforeUnload` carries Chromium's fixed text, never the page's. Navigation or closing withdraws it. An automation session cannot answer a dialog. |
| `http_auth` | `challenger`, `realm`, `isProxy`, `scheme`, `savedCredentialsAvailable` (false until T-UIR-04) | `accept` `{username, password}`; everything else cancels the challenge. The request is withdrawn when the network stack drops the challenge. |
| `file_pickers` | `mode` (open, openMultiple, directory, save), `acceptedTypes` (MIME types and `.ext`), `suggestedName` (save), `isFileSystemAccessAPI` (always false: the File System Access pickers are not routed yet), `origin` | `accept` `{paths}`. More than one path for a single-file input keeps the first. A directory upload is enumerated by the engine. `disabled` cancels the input. |
| `find` | `initialText` (the page's selection as the browser process knows it, else the last search; only the last search is tested) | Raised by the engine's find entry point (`PageFind::Present`, which `page.find.present()` will call: the find functions are not in the C ABI yet). `accept` `{action:"close"}` ends it and clears the search. Only one is open per page; asking again focuses the bar. Navigation withdraws it. With no handler the default bar searches through Chrome's find helper and shows "n of m". |
| `fullscreen` | `origin`, `isEntering` | `accept` (Swift `allow()`) and `deny`. `disabled` makes the Fullscreen API reject without a request. |
| `notifications` | `notification` `{title, body, origin, tag, actions, requireInteraction}` | `accept` `{event:"clicked", action?}` fires the page's `onclick` (action by its title), `deny` `{event:"closed", byUser}` fires `onclose`; a rejected, dropped or disabled one closes without being shown. `disabled` also makes the permission prompt answer denied, so `Notification.requestPermission()` resolves `denied` without asking. Web, persistent and extension notifications are routed; Chrome's own notification types are not shown. A notification belongs to no page: the engine attaches it to a page of the profile (preferring one at the origin) and drops it if none is open. The default presenter is an in-window banner (the system notification centre needs the alert helper and entitlements). |

Neither `window` nor an operating-system permission step is filled in yet: `window` stays 0, and macOS camera, microphone and location access are not requested by the engine after a grant.
