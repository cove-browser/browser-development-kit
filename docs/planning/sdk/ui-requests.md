# BDK browser-owned UI requests

Status: proposal, 2026-11-15. The pattern is from [../architecture.md](../architecture.md) section 7. This document lists every surface, what a request carries, how it is completed, what the default presenter does and which rule cannot be overridden. Names are placeholders. Status labels are in [README.md](README.md) section 2. Tasks: T-UIR-01 to T-UIR-06, T-UIR-09 to T-UIR-12, T-SWF-04, T-MAC-04, T-MAC-05.

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
| A permission, an extension install, a credential fill or a dangerous download cannot be granted without a user gesture | The granting completion takes the `NSEvent` that confirmed it. The core refuses a granting completion with no `NSEvent` (`ui.gestureRequired`); the engine also checks that it is a recent mouse-up or key-down delivered to a window of this application. The granting completion of each surface is named in section 5 (`grant`, `keep`, `save`, `accept` for suggestions, `proceed`, `select`, `share`, `open`, and `install` for a web app). |
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
| `client_certificates` | Client certificate selection | D C | Accepted (S3, 2026-11-25); default is a sheet of BDK's own, not routed yet (8.2) | T-UIR-10, M4; routing T-UIR-13, M9 |
| `file_pickers` | File and directory choosers | D C X | Plan | M3, T-UIR-02 |
| `context_menus` | Page context menus | D C X | Plan | M3, T-UIR-03 |
| `new_windows` | Popups, undocked DevTools, picture-in-picture, extension windows | D C | Plan | M2, T-ADP-05; M3, T-UIR-03 |
| `downloads` | Download start, progress, warnings | D C X | Plan | M3, T-UIR-03 |
| `passwords` | Save, update, generate; leaked-password warning (Added, S16) | D C X | Plan | M3, T-UIR-04 |
| `autofill` | Save address or card; fill confirmation | D C X | Plan | M3, T-UIR-04 |
| `autofill_suggestions` | The suggestion list under a form field | D C | Accepted (S3, 2026-11-25) | T-UIR-15, M9 |
| `printing` | Print preview and the system print dialog | D C X | Plan | M3, T-UIR-04 |
| `translation` | Offer, progress, result, errors, preferences | D C X | Plan | M6, T-PRV-03 |
| `find` | Find bar | D C | Plan | M3, T-UIR-06 |
| `fullscreen` | HTML fullscreen | D C X | Plan | M2, T-MAC-05; M3, T-UIR-06 |
| `exclusive_access` | Keyboard-lock and pointer-lock notices | D C | Accepted (S3, 2026-11-25) | T-UIR-15, M9 |
| `notifications` | Web notifications | D C X | Plan | M3, T-UIR-06 |
| `certificate_errors` | Certificate interstitials | D C | Plan | M3, T-UIR-05 |
| `reputation_warnings` | Malware, phishing and download warnings | D C | Plan | M3, T-UIR-05 |
| `devtools` | The DevTools front end | D C X | Plan; built in M7, engine pending (T-AUT-06) | M7, T-AUT-05 |
| `device_choosers` | WebUSB, Web Bluetooth, WebHID, Web Serial | D C X | Accepted (S3, 2026-11-25); refused until routed, Web Bluetooth off (8.2) | T-UIR-10, M4; routing T-UIR-13, M9 |
| `screen_capture` | The `getDisplayMedia` source picker | D C X | Accepted (S3, 2026-11-25); refused until routed (8.2) | T-UIR-10, M4; routing T-UIR-13, M9 |
| `webauthn` | Passkey and security-key dialogs | D C X | Accepted (S3, 2026-11-25); built and tested on the engine 2026-11-22; the `bdk.toml` key is T-UIR-15 | M4, T-UIR-09; key T-UIR-15, M9 |
| `external_protocols` | "Open this link in another application?" | D C X | Accepted (S3, 2026-11-25); default is a sheet of BDK's own, not routed yet (8.2) | T-UIR-10, M4; routing T-UIR-13, M9 |
| `page_health` | Unresponsive page; blocked popups | D C | Accepted (S3, 2026-11-25) | T-UIR-15, M9 |
| `payments` (name settled in T-UIR-14) | Payment Request sheet and Secure Payment Confirmation | D C X | Planned (product owner, 2026-11-25); off at start until built (8.2) | T-UIR-14, M9 |
| `identity` (name settled in T-UIR-14) | FedCM account chooser | D C X | Planned (product owner, 2026-11-25); off at start until built (8.2) | T-UIR-14, M9 |
| `digital_credentials` (name settled in T-UIR-14) | Digital Credentials API dialogs | D C X | Planned (product owner, 2026-11-25); off at start until built (8.2) | T-UIR-14, M9 |
| `web_app_installs` | The install prompt of an installable web app | D C | Planned (product owner, 2026-11-25); raised only with `[web_apps] enabled = true`, which is how it is turned off ([web-apps.md](web-apps.md) 4.1) | T-PWA-03, T-PWA-04, M9 |
| Extension surfaces | Actions, popups, side panels, menus, options, install prompts | See [extensions.md](extensions.md) | Plan | M5: surfaces 25 to 32 (`extension_actions` … `extension_uninstalls`), configured under `[extensions.ui]`; see `packages/core/docs/conventions.md`, "Extensions (M5)" |

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

Built for client certificates (T-UIR-10, 2026-11-22; built, with no engine scenario yet; 8.2): no request is raised yet (S3, T-UIR-13). Chrome's automatic selection (`AutoSelectCertificateForUrls`) runs first; otherwise a sheet of BDK's own lists the first eight certificates by subject and issuer, with Cancel, which goes on without one, as Chromium's selector does. Chromium's selector is a views dialog and is never shown on a BDK page.

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
    public let kind: Kind                        // .popup, .newPage, .devTools, .pictureInPicture, .extensionWindow, .application, .webApp
    public let disposition: Disposition          // .foreground, .background, .newWindow, .popup, .privateWindow
    public let targetURL: URL?
    public let opener: BDKWebContents?
    public let features: WindowFeatures          // requested size and position, from window.open
    public let isUserGesture: Bool
    public let initiatorExtensionID: String?     // the extension that asked (an options page, a link in its popup)
    public let webApp: WebAppInfo?               // kind .webApp only: the installed app being launched (not built: T-PWA-03, M9)
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
| `.webApp` | An installed web app being launched, or opening another window inside its scope ([web-apps.md](web-apps.md) 4.2). Not built: T-PWA-03, T-PWA-04, M9 |

| | |
|---|---|
| Default presenter | A plain `NSWindow` holding the new page's view, sized from the requested features. No application owns the page: closing the window, or `window.close()` in the page, destroys it |
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

Built (M3, T-UIR-03; engine/services/download_prompts.cc). Each profile gets a `ChromeDownloadManagerDelegate` subclass that asks through a request wherever Chromium would show its file picker (the "ask where to save" setting, save-as, a name conflict). Payload: `download` (`url`, `suggestedName`, `mimeType`, `size` when known, `referrer`) and `needsLocation: true`. Completions: `accept` with an optional `destination` path (the suggested path if absent) or `deny`. The default presenter is a native save panel on the page's window; with no window it cancels. A download Chromium holds as dangerous or insecure raises a second request, with `danger` (`dangerousFileType`, `dangerousURL`, `dangerousContent`, `uncommon`, `insecure`) and `text`; `grant` keeps it and needs the gesture; `deny`, a rejection, a dropped or a released request discards it. The default presenter is a native Keep or Discard sheet. A request whose download got resolved elsewhere is withdrawn. Chromium's download shelf and bubble are not hosted: BDK has no toolbar, so the download list is the `downloads` service's commands and events.

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
| Default presenters | A native sheet on the page's window for the password, address and card questions (a BDK window has no `addressBar` for Chromium's bubbles); Chromium's suggestion popup under the field; a sheet on the page view for the leaked-password warning, which names the site and advises changing the password |
| Leaked-password warning | Raised after a sign-in when the provider reports the submitted login as leaked ([services.md](services.md) 5.9). It informs; it grants nothing and has no accepting completion. Opening a password-management page from it is the application's choice. |
| Storage | Passwords are encrypted with a key in the Keychain item named in `[storage]`. A presenter never sees a stored password in clear. |
| Rule | Filling a credential or a card into a page needs a user gesture on the suggestion. Revealing a stored password is a service command that needs operating-system authentication ([services.md](services.md) 5.9). |
| `disabled` | Nothing is offered and nothing is filled. Stored data stays. |

Built (M3, T-UIR-04; engine/services/password_prompts.cc and autofill_prompts.cc). Not built: the address-bar anchor and the leaked-password request (needs the M6 provider).

Passwords. The password manager's bubble goes through `TabDialogs::ShowManagePasswordsBubble`, which a patch lets BDK supply. Only the two questions are requests, save and update (`isUpdate`); the manage list and the confirmation have no counterpart. Payload: `origin`, `username`, `isUpdate`; the password is never in it. Completions: `grant` (save or update, needs the gesture), `deny` with `never: true` (never for this site; save only) or `dismissed: true`. A rejected, dropped or disabled request saves nothing and leaves the password manager's state as an ignored bubble does. The default presenter is a native sheet (Save or Update, Not now, Never for this site) on the page's window. Filling a saved login into a form is Chromium's own and needs no request.

When the question is asked. Chromium's route to `ShowManagePasswordsBubble` works in a BDK window: logging each step of it showed the tab is active and the browser, the tab features, the passwords page action and the bubble manager all exist, and it delivered the password manager's other prompts. The question it never delivered is the save question in the engine scenarios (a scripted form posted to a scripted page). There the password manager never calls `ManagePasswordsUIController::OnPasswordSubmitted`, the call that pops the bubble up: the page that follows the submission sends no `PasswordFormsRendered`, so `PasswordManager::OnLoginSuccessful` never runs (why it does not was not found). The state is pending all the same, because the password manager's manual fallback (`ShowManualFallbackForSaving`, as the person types credentials) makes it so and then only shows an icon, which opens the bubble when clicked and which a BDK window has none of. BDK therefore asks a pending question when the page finishes a navigation (`BdkTabDialogs::OnPageNavigated`, after the password manager has handled the navigation itself): no timer, no polling. A login Chromium does detect reaches `ShowManagePasswordsBubble` by its own route first, and the request in progress or the answer already given stops this from asking twice. Before this, BDK looked for the question 0.3, 1 and 2.5 seconds after each navigation.

Autofill save and update prompts (`autofill`). Chromium asks the window for an `AutofillBubbleHandler`: the address bubbles through `BrowserWindow::GetAutofillBubbleHandler()` (found through `BrowserWindow::FindBrowserWindowWithWebContents()`, which only finds a `BrowserView`; a patch makes them look in the Browser's user data instead, as the card bubbles already do) and the card bubbles through that user data. BDK's host-context window creates its handler and registers it there, so both routes find it. Save address, update address and save card are each one `autofill` request. Payload: `kind` (`address`, `card`), `isUpdate`, `summary` (the address in envelope style, or the card's network and last four digits; the number is never in a request), `text` `{title, message}`. Completions: `grant` (or `save`; needs the gesture) saves, `deny` declines, `deny` with `dismissed: true` leaves it undecided as an ignored bubble does; a rejected, dropped, disabled or withdrawn request saves nothing. The default presenter is a native sheet on the page's window with Chromium's own button labels. A card that asks for a name or an expiry date from the person is saved as it is: the sheet asks for nothing. The card prompt has no engine scenario (the engine has no payments backend to offer one). Every other autofill bubble (offers, IBAN, virtual cards, mandatory re-authentication, the address sign-in promo) is server-driven or tied to the toolbar and is not shown: the handler gives Chromium an object that never shows.

The suggestion popup (`autofill_suggestions`). Chromium builds its popup in `AutofillPopupView::Create`; a patch lets BDK supply a factory. With a handler for the surface the suggestions are a request. Payload: `fieldRect` (`x`, `y`, `width`, `height`, in the page view's coordinates, which is also where the request is anchored) and `suggestions`, each `id` (the popup's row, as a decimal string), `label`, `sublabel`, `kind` (`password`, `address`, `card`, `generatedPassword`); titles, separators and footers are not listed. `grant` (Swift `accept`; `accept` is taken too) with `suggestion` set to an id fills the form and needs the gesture. Chromium ignores a choice made in the half second after the popup appeared; BDK delays an earlier one until then. `deny` closes the popup. A new list (the field changed) withdraws the request and raises another. With no handler, or `.useDefault`, the default presenter is Chromium's own popup, which needs no toolbar: on macOS it is a child window of the page's window (`CanShowRootPopup` asks only for the native view's window), so a window that is not a `BrowserView` hosts it. The engine scenarios create it for the page. Chromium shows suggestions only to a view that has keyboard focus, which on macOS includes being in the key window; the conformance harness turns that requirement off (`RWHVMacHasFocusReturnsFalseForNonKeyWindow`), as it does for fullscreen, because it cannot rely on its windows being key. The surface has no `disabled` mode (D C); a disabled `autofill` surface turns the offers and the popup off, so nothing is offered and nothing is filled.

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

Built (M3, T-UIR-04; engine/services/print_prompts.cc, a patch adds a hook in `PrintViewManager`). `window.print()`, the print command and `page.print()` (`bdk_contents_print`, T-SWF-07, always user-initiated) raise one request (`isUserInitiated` is false for `window.print()`) before Chromium creates its preview dialog, which is a constrained web dialog of a Browser window and cannot be shown by a windowless host context (feasibility.md 1). So `previewContents` is not offered yet and the default presenter is the system print panel through Chromium's basic print path (`PrintViewManager::BasicPrint`); the page's `afterprint` still fires. Completions: `accept` with `action: "systemDialog"`, or `deny`. `savePDF` prints nothing: the print-to-PDF command (`system`) is the way to a file. Hosting Chromium's print preview as a page of its own needs the modal-dialog host of amendments 2.13 backed by the page's host view and is not done.

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

Built in M6 (2026-11-20; stub and Swift tested, engine written and not yet built). The request is the offer, raised once per page load when `translation_decide` says to offer, and again by `translation.present`; later phases are `translation.stateChanged` events and `page.translation`'s state, not updates of the request. Payload: `phase` (`offer`, `translating`, `translated`, `failed`), `detected`, `target`, `languages` (`[{from, to, installed}]`), `provider` (the descriptor), `progress`, `error`. Completions, none of which grants: `translate` (`from`, `to`), `revert`, `always` and `never` (`language`, default the detected one), `neverForSite`, `dismiss`. An offer is made only when the provider answers which languages it translates. The default presenter is a sheet on the page's window (Translate, Never translate this language, Never translate this site, Not now) that names a remote provider's host. The address-bar anchor is not built (S11, T-SWF-08).

### 5.11 Find in page

```swift
public final class FindRequest: BDKUIRequest {
    public let initialText: String?              // the selection, or the last search
    public let session: FindSession              // the same object as page.find
    public func close()
}
```

Default presenter: a find bar at the top trailing corner of the page view. Raised by `page.find.present()` and by the page command `.find`. `session` is Built (T-SWF-07): `FindRequest.session` is `page.find`, which also starts a search without any request (`bdk_contents_find_start`; results are `BDK_CONTENTS_EVENT_FIND_RESULT` events). An application with its own find field uses `page.find` directly and never sees this request.

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

Built (M3, T-UIR-05; engine/services/interstitial_prompts.cc). When a certificate-error (`SSLBlockingPage`) or Safe Browsing blocking page commits, a request goes to the handler of its surface. Payload: `url`, `isOverridable` (read from the page's own load-time data, the value its Proceed button obeys), `text`; certificate errors add `error` (`dateInvalid`, `authorityInvalid`, `commonNameInvalid`, `revoked`, `pinningFailure`, else the net error name) and `certificate` (`id`, `subject`, `issuer`); reputation warnings add `verdict` and `provider`. `go_back` and `proceed` drive the page's own commands, so Chromium refuses a proceed for HSTS or pinning even if the request said otherwise. With no handler the page is the whole UI. The page stays blocked whatever the application does: a withdrawn request (the page moved on) does nothing. A request that is not a safe-browsing or certificate page (HTTPS-only, bad clock, captive portal) is not raised. HTTPS-First asks with its interstitial page in the page, not with its newer "ask before HTTP" dialog, which is off at start (T-UIR-10, 8.2). The reputation provider comes in M6; whatever Chromium raises is routed. Since M6 (written, engine pending) the reputation provider is BDK's: `provider` in the payload is the configured provider's descriptor, and for the default lists the message says their protection is reduced.

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

Default presenters: Chromium's chooser dialogs and media picker. `disabled`: the API rejects. Chromium's choosers and picker are views and cannot be shown on a BDK page (8.2), so T-UIR-13 (M9) writes BDK's own default presenters for both, and Web Bluetooth is turned on with them (product owner, 2026-11-25). While a page is capturing, `page.capture` reports it, and the application must show an indicator; the default presenter for that is a small "sharing" bar in the page's window.

Built so far (T-UIR-10, 2026-11-22, passes on Chromium 158; 8.2): neither surface is routed (S3, T-UIR-13), and Chromium's chooser bubbles and picker cannot be shown on a BDK page, so both behave as `disabled` does. `navigator.usb.requestDevice()` and `navigator.serial.requestPort()` reject with NotFoundError, `navigator.hid.requestDevice()` resolves with no device, and `getDisplayMedia` rejects with NotAllowedError. Web Bluetooth is off at start (`navigator.bluetooth` is absent): nothing could choose a device, and its adapter would need the application's Bluetooth usage string.

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

Default presenter: Chromium's confirmation dialog. `disabled`: external schemes are blocked. BDK's sheet (below) becomes the default presenter once the request is routed (T-UIR-13, M9).

Built so far (T-UIR-10, 2026-11-22, passes on Chromium 158; 8.2): no request is raised yet (S3, T-UIR-13). Chrome decides whether to ask: blocked schemes stay blocked, `mailto:` and schemes the person allowed open without asking, and policy applies. When it asks, a patch gives the question to the engine, which shows a sheet of BDK's own with Chromium's wording ("Open <application>?") and Cancel as the default button, and opens the application only on "Open". The "always allow" checkbox is not offered.

### 5.18 WebAuthn (Added; built, T-UIR-09)

```swift
public final class WebAuthnRequest: BDKUIRequest {      // one request per step of a ceremony
    public let ceremonyID: String                        // the same for every step of one ceremony
    public let relyingParty: String
    public let origin: URL?
    public let operation: Operation                      // .create, .get
    public let step: Step                                // .chooseMechanism, .touchSecurityKey, .enterPIN,
                                                         // .chooseAccount, .connectPhone, .confirm, .error, .other
    public let detail: String                            // Chromium's step name, e.g. "clientPinTapAgain"
    public let error: ErrorKind?                         // .timedOut, .noPasskeys, .keyNotRegistered, .pinBlocked, ...
    public let text: BDKUIText                           // Chromium's wording; also errorMessage, hint
    public let acceptLabel, cancelLabel, otherMechanismsLabel: String?
    public let isBusy: Bool
    public let mechanisms: [Mechanism]                   // .securityKey, .phone, .platform, .iCloudKeychain, .passkey, ...
    public let accounts: [Account]
    public let pin: PINEntry?                            // mode .enter, .setup, .change; minimumLength; attemptsRemaining
    public let qrCode: String?
    public func choose(_ mechanism: Mechanism) throws
    public func providePIN(_ pin: String) throws
    public func chooseAccount(_ account: Account, confirmedBy event: NSEvent) throws
    public func accept() throws                          // the step's own button
    public func useOtherMechanism() throws
    public func cancel()
    public var presentation: Presentation                // what to show, whatever draws it; perform(_:) answers
    public func qrCodeImage(scale: CGFloat) -> NSImage?
    public static func cancelAll(_ request: WebAuthnRequest) -> BDKUIResponse
}
public struct BDKWebAuthnSupport { static var current; securityKeys, phones, systemPasskeys, touchID }
```

A ceremony is Chromium's own (`ChromeAuthenticatorRequestDelegate` and its dialog model): BDK does not choose the
steps, the mechanisms or the accounts. Each step Chromium's dialog would show is one request. When Chromium moves on
(the key was touched, a PIN was wrong, the request finished) the step's request is withdrawn and the next one, with the
same `ceremonyID`, is delivered, so a presenter can keep one sheet up for the ceremony; the ceremony is over when a
step is withdrawn and none follows. A step whose facts change while it is up (a PIN error, the busy indicator, the list
of mechanisms) is withdrawn and raised again.

| | |
|---|---|
| Payload | `ceremony`, `relyingParty`, `origin` (the top-level page), `operation`, `step`, `detail`, `error` (error steps), `text` `{title, message, error?, hint?}` from Chromium's sheet for the step, `acceptLabel`, `cancelLabel`, `otherMechanismsLabel` (each when the step has the button), `busy`, `mechanisms` `[{id, kind, name, description?}]` (chooseMechanism), `accounts` `[{id, name, displayName}]` (chooseAccount), `pin` `{mode, minLength, attemptsRemaining?}` (enterPIN), `qrCode` (connectPhone) |
| Completions | `accept` `{mechanism}`, `{pin}` (and `pinConfirmation` from a presenter that asked twice), `{action:"otherMechanisms"}`, or `{}` for the step's own button; `grant` `{account}`, with the gesture: choosing an account releases a credential to the site; `deny` cancels. The core refuses anything the step did not offer (`ui.widensRequest`), an account chosen with `accept` (`ui.completionNotAllowed`) and every other completion name |
| Default presenter | A sheet on the page's window, built from the same payload: a button per mechanism or account, a PIN field (two for a new PIN), the step's own buttons, then cancel. Chromium's tab-modal views dialog is never shown: it needs a views widget, and on a BDK page it ended the process (a patch lets the engine supply the dialog) |
| Rule | A rejected, dropped or disabled request, an answer that does not fit the step, a grant whose event is not a recent click or key of the application's window, and a page that is not a BDK page all cancel the ceremony: the page's promise rejects with `NotAllowedError` |
| `disabled` | Every ceremony that needs UI is cancelled before anything is shown. The resolved configuration takes `disabled`; `bdk.toml` cannot say it until S3 is accepted, so an application that offers no passkeys sets `WebAuthnRequest.cancelAll` as the handler |
| Not routed | Conditional mediation (passkeys in the autofill list) and passkey upgrades show no dialog step, so they raise no request (T-UIR-12) |

What each mechanism needs of the application, as Chromium decides it (`BDKWebAuthnSupport.current` reads the same
facts; `bdk build config` adds the note `plist.passkeys_phone`):

| Mechanism | Needs | Status |
|---|---|---|
| Security keys (USB; NFC and Bluetooth keys as Chromium supports them) | Nothing | Built; tested on the engine with a virtual CTAP2 key, not with hardware |
| Passkeys on a phone (hybrid: QR code, Bluetooth, the phone's tunnel service) | `NSBluetoothAlwaysUsageDescription`; under the hardened runtime `com.apple.security.device.bluetooth`. Without the usage string the engine reports Bluetooth as unsupported (`ui_webauthn.cc`), so Chromium offers no phone and makes no Bluetooth call: macOS ends a process that calls Bluetooth without it, and Chromium checks only before its power manager, not before the phone's discovery | Routed (`connectPhone`, `qrCode`); not verified with a phone, and the tunnel service the phone picks (Google's for Android, Apple's for an iPhone) is contacted once the person chooses the phone (T-UIR-12) |
| System passkeys (iCloud Keychain and other credential providers) | macOS 13.5 and Apple's `com.apple.developer.web-browser.public-key-credential` entitlement, which Apple grants to browsers | Chromium's own path, which shows the system's sheet; not verified (T-UIR-12) |
| Touch ID passkeys kept by the browser | A `keychain-access-groups` entitlement for `<team>.<bundle id>.webauthn` with a provisioning profile; Chromium compiles its own group in, which no BDK application can hold | Not offered to BDK applications yet (T-UIR-11) |

A custom presenter: `apps/examples/minimal-browser` sets one before start (`PasskeyPresenter` and `PasskeySheet`). It
keeps one sheet per `ceremonyID` on the page's window, redraws it for each step from `presentation`, answers with
`perform(_:)`, and closes it when the step is withdrawn with no next one, or a short while after an answer that no
step follows. Its logic is unit-tested on the stub; on a real page it is a hand check (T-UIR-09).

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

### 5.21 Web app installs (product owner, 2026-11-25; not built, T-PWA-03 and T-PWA-04, M9)

The install prompt of an installable web app, and the window it is launched in, are in [web-apps.md](web-apps.md) section 4. They follow sections 1 to 3 of this document without exception: `install` is a granting completion.

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
| Arguments | `permissions`: grant `{"kinds":[…],"duration":"persistent"\|"thisVisit"}`, deny `{"persist":bool}` or `{"dismissed":true,"persist":false}`. `javascript_dialogs`: accept `{"text"?,"suppressFurther"}`, deny `{"suppressFurther"}`. `http_auth`: accept `{"username","password"}`. `client_certificates`: accept `{"certificate":id}`. `file_pickers`: accept `{"paths":[…]}`. `context_menus`: accept `{"action":"show","items":[…],"at"?}` or `{"action":"perform","item":id}`. `downloads`: accept `{"destination"?}`. `printing`: accept `{"action":"systemDialog"}` or `{"action":"savePDF","path"}`. `find`: accept `{"action":"close"}`. `passwords`, `autofill`: deny `{"never":true}` or `{"dismissed":true}`. `autofill_suggestions`: grant `{"suggestion":id}`. `notifications`: accept `{"event":"clicked","action"?}`, deny `{"event":"closed","byUser"}`. `external_protocols`: grant `{"remember"}`. `webauthn`: accept `{"mechanism"}`, `{"pin"}`, `{"action":"otherMechanisms"}` or `{}`, grant `{"account"}` (5.18). |
| Payload | The field names of section 5 in camelCase. Handles (`opener`, `newContents`, `previewContents`, `inspected`, `frontend`) are decimal strings. Two kinds on one surface are told apart by the payload: `downloads` with a `danger` field is a dangerous download, `passwords` with `"kind":"leaked"` is the leaked-password warning. |
| Narrowing | Checked in Swift before the ABI call: a grant for kinds the request did not ask for (`ui.kindsNotRequested`), a certificate, menu item or suggestion it did not offer, more than one file for a single-file picker, `proceed` on a non-overridable interstitial (`ui.notOverridable`). |
| Gaps | The ABI cannot report a notification as shown without completing it, preview an autofill suggestion, or complete an `NSEvent`-less `grant`. `shown()` is local, `preview` is absent. |

### 8.1 What the engine sends and does (T-UIR-02, T-UIR-06, 2026-11-19)

Settled against the Chromium engine (`chromium/bdk/engine/ui_*.cc`). Where this differs from the Swift decoders above, the decoders already match.

| Surface | Payload fields the engine sends | Outcomes |
|---|---|---|
| `permissions` | `origin`, `embeddingOrigin` (when different), `kinds` (camera, microphone, geolocation, notifications, clipboardRead, midi, storageAccess, localFonts, windowManagement, idleDetection, and Chromium's others by name), `text` `{title: "<origin> wants to", message: <Chromium's fragment>}`, `offersOneTime` | `grant` needs a recent mouse-up or key-down `NSEvent` of one of this application's windows (the engine checks type, age under 10 s and window; anything less is a dismissal, nothing granted). `kinds` must equal what was asked: the request manager decides a prompt's requests together, so a grant of fewer kinds grants nothing (dismissed). `duration` `thisVisit` is Chromium's one-time grant. `deny` `{persist:true}` blocks for the site, `{persist:false}` and a rejected or dropped request leave no decision and no embargo count, `{dismissed:true}` counts as a dismissal. Withdrawn when the manager drops the prompt (navigation, close). The decision lands in the profile's content settings, so a persistent profile keeps it and a private profile never shares it. Camera and microphone arrive here through Chrome's media capture logic. A prompt for a page that is not visible waits for it to be (Chromium's rule). |
| `javascript_dialogs` | `kind`, `origin`, `pageTitle`, `message`, `defaultPromptText` (prompt), `isReload`, `offersSuppression` (from a page's second dialog since its last navigation) | `accept` `{text?, suppressFurther}` and `deny` `{suppressFurther}`. Suppressing silences the page's later dialogs until it navigates; the engine honours it even if the dialog did not offer it. `beforeUnload` carries Chromium's fixed text, never the page's. Navigation or closing withdraws it. An automation session cannot answer a dialog. |
| `http_auth` | `challenger`, `realm`, `isProxy`, `scheme`, `savedCredentialsAvailable` (false: a saved login is not offered for a challenge yet) | `accept` `{username, password}`; everything else cancels the challenge. The request is withdrawn when the network stack drops the challenge. |
| `file_pickers` | `mode` (open, openMultiple, directory, save), `acceptedTypes` (MIME types and `.ext`), `suggestedName` (save), `isFileSystemAccessAPI` (always false: the File System Access pickers are not routed yet), `origin` | `accept` `{paths}`. More than one path for a single-file input keeps the first. A directory upload is enumerated by the engine. `disabled` cancels the input. |
| `find` | `initialText` (the page's selection as the browser process knows it, else the last search; only the last search is tested) | Raised by the engine's find entry point (`PageFind::Present`, which `page.find.present()` calls through `bdk_contents_find_present`). `accept` `{action:"close"}` ends it and clears the search. Only one is open per page; asking again focuses the bar. Navigation withdraws it. With no handler the default bar searches through Chrome's find helper and shows "n of m". |
| `passwords` | `origin`, `username`, `isUpdate` | `grant` needs the gesture; `deny` `{never:true}` or `{dismissed:true}`. Asked when the page finishes a navigation with a question pending ([5.8](#58-passwords-and-autofill)). |
| `autofill` | `kind` (`address`, `card`), `isUpdate`, `summary`, `text` `{title, message}` | `grant` (or `save`) needs the gesture; `deny` declines, `deny` `{dismissed:true}` leaves it undecided. A rejected, dropped, disabled or withdrawn request saves nothing. |
| `autofill_suggestions` | `fieldRect` (page view coordinates), `suggestions` `[{id, label, sublabel?, kind}]` | `grant` (or `accept`) `{suggestion:id}` needs the gesture and is held back until Chromium accepts a choice (half a second after the popup appeared); `deny` closes. A new list is a new request. The surface has no `disabled` mode; a disabled `autofill` surface turns it off too. |
| `fullscreen` | `origin`, `isEntering` | `accept` (Swift `allow()`) and `deny`. `disabled` makes the Fullscreen API reject without a request. |
| `notifications` | `notification` `{title, body, origin, tag, actions, requireInteraction}` | `accept` `{event:"clicked", action?}` fires the page's `onclick` (action by its title), `deny` `{event:"closed", byUser}` fires `onclose`; a rejected, dropped or disabled one closes without being shown. `disabled` also makes the permission prompt answer denied, so `Notification.requestPermission()` resolves `denied` without asking. Web, persistent and extension notifications are routed; Chrome's own notification types are not shown. A notification belongs to no page: the engine attaches it to a page of the profile (preferring one at the origin) and drops it if none is open. The default presenter is an in-window banner (the system notification centre needs the alert helper and entitlements). |
| `webauthn` | As 5.18: one request per step of Chromium's dialog model, raised by BDK's dialog controller (`engine/ui_webauthn.cc`) through the patch that lets an embedder supply it | Mechanism, PIN, other mechanisms and the step's button go back through Chromium's sheet models; a grant selects the account only with a recent mouse-up or key-down of this application's window (anything less cancels); `deny`, reject, drop and `disabled` cancel the ceremony (`NotAllowedError`). Navigation or a finished request withdraws the step. Tested on the engine (`tests_engine_webauthn.c`) with a virtual security key |

Neither `window` nor an operating-system permission step is filled in yet: `window` stays 0, and macOS camera, microphone and location access are not requested by the engine after a grant.

### 8.2 Chromium's other browser dialogs (T-UIR-10, 2026-11-22)

Chromium shows many browser dialogs as tab-modal views dialogs or bubbles (`TabDialogManager`,
`constrained_window::ShowWebModalDialogViews`, `chrome::ShowTabModal`). A BDK page's window is not a views widget, so
each one a page reached failed a check (`native_widget_mac.mm`, `tab_dialog_manager.cc`) or dereferenced a null
`BrowserView`, and ended the application. Every caller at the pin was listed and sorted by whether a BDK page can
reach it. The engine side is `chromium/bdk/engine/ui_browser_dialogs.cc`; the scenarios are
`packages/core/tests/tests_engine_dialogs.c`. **Built and run on Chromium 158 (2026-11-22): the 7 scenarios pass;
the client-certificate sheet has no scenario yet, so T-UIR-10 stays open for it.** "Not reachable" below is from
reading the source, not from a test.

| Chromium's dialog | Reached from a BDK page by | Now |
|---|---|---|
| External protocol (`ExternalProtocolDialog`) | A link to a scheme another application handles that Chrome asks about | A sheet (5.17); patch `bdk-let-an-embedder-present-the-external-protocol-dialog` |
| Tab-modal dialog models: File System Access ("view files?", restricted directory, dangerous file), DevTools' folder permission, extension "install blocked" and "ask a parent" | `showDirectoryPicker()` and the save picker; DevTools workspaces; installing a blocked extension | A sheet: the model's title and paragraphs, OK and Cancel with the model's default first, and the model's own callbacks; a checkbox keeps its initial state; a model with a text field or combobox is closed as dismissed. Patch `bdk-let-an-embedder-present-tab-modal-dialog-models` |
| Credential Management account chooser and auto sign-in notice | `navigator.credentials.get({password: true})` on a site with saved passwords | Sheets of the `passwords` surface: one button per account (the first eight); OK or Turn off. Patch `bdk-let-an-embedder-present-the-credential-manager-prompts` |
| Client certificate selector | A server that asks for a certificate the Keychain has | A sheet (5.3); no patch |
| Device chooser bubbles (WebUSB, WebHID, Web Serial) | `requestDevice()`, `requestPort()` | Refused as a cancel (5.16); no patch |
| Web Bluetooth chooser, scanning prompt, pairing dialogs | `navigator.bluetooth` | Off at start (`WebBluetooth`), and refused if reached |
| Screen-capture picker, "share this tab", capture-policy confirmation | `getDisplayMedia()` | Refused: NotAllowedError (5.16) |
| HTTPS-First's "ask before HTTP" dialog | An HTTP navigation with HTTPS-First on | Off (`HttpsFirstDialogUi`): the interstitial page asks instead (5.14) |
| Payment Request sheet, Secure Payment Confirmation | `new PaymentRequest(...).show()` | Off (`WebPayments`, `SecurePaymentConfirmation`): `PaymentRequest` is absent |
| FedCM account chooser | `navigator.credentials.get({identity})` | Off (`FedCm`): `IdentityCredential` is absent |
| Digital Credentials dialogs | `navigator.credentials.get({digital})` | Off (`WebIdentityDigitalCredentials`, `...Creation`) |
| Web application install dialogs | `beforeinstallprompt`'s `prompt()` | Off: `profile.web_app.install_by_user_enabled` defaults to false, so no page offers itself for installation. From M9 that is the behaviour of `[web_apps] enabled = false`, the default; with `true` the dialog becomes a `web_app_installs` request (T-PWA-04, [web-apps.md](web-apps.md)) |
| JavaScript dialogs, HTTP authentication, permission prompts, file pickers and folder upload, password and autofill save bubbles, printing, WebAuthn, extension install and uninstall prompts, Chrome's `TabModalConfirmDialog` from the `Browser`'s delegate | Pages | Already BDK's (5.1 to 5.18, extensions.md): no views dialog is reached |
| Bookmarks editor, application info, shortcuts, web application uninstall, update review and launch, tab groups, the window name prompt, site data, certificate viewer, caret browsing, "downloads in progress", relaunch and update notices, search engine choice, profiles, sync and sign-in | Chrome's own windows, menus and accelerators | Not reachable: a BDK application has none of them, and sign-in is off |
| Autofill payments (server cards, OTP, BNPL, save and fill, payments WebAuthn), password change, the leaked-password dialog, tailored security, password reuse, Lens, Glic, the actor, skills, Save to Drive, dictation onboarding, feature first run | Google services | Not reachable: off at start or with no Google key (providers/README.md); the leaked-password warning is S16 |
| Content analysis, data controls, device signals, idle and enterprise start-up dialogs, cloud management | Enterprise policy | Not reachable: they need policies BDK applications do not set |
| Platform-app choosers, media galleries, kiosk file system, `chrome.desktopCapture.chooseDesktopMedia` | Extension and platform-app APIs | Platform apps cannot run. An installed extension can still open the desktop-capture picker: T-UIR-13, M9 |

**Decided by the product owner, 2026-11-25** ([../amendments.md](../amendments.md) 2.23): every row above that is
refused, off or a fixed sheet becomes a routed request in M9. The web application install dialogs were left out of
that decision and decided separately the same day (amendments 2.24): they stay off by default and are routed when
the application sets `[web_apps] enabled = true` (T-PWA-04). T-UIR-13 routes the device choosers (with Web Bluetooth on), screen
capture, client certificates, external protocols, the File System Access pickers through `file_pickers`, and
`chrome.desktopCapture.chooseDesktopMedia` through `screen_capture`. T-UIR-14 turns on and routes Payment Request,
Secure Payment Confirmation, FedCM and the Digital Credentials API, each with a default presenter of BDK's own.

Default presenters scripted in tests (`present.script`, `bdk_testing.h`): `external_protocols` (`cancel`, `open`),
`client_certificates` (`certificate<N>`, `cancel`), `browser_dialogs` (`accept`, `cancel`) and `passwords`
(`account<N>`, `cancel`; `ok`, `turnOff`). `dialogs.tab_modal` puts a model through `chrome::ShowTabModal` as File
System Access does, and `pref.set` sets a Chromium preference (HTTPS-First's `https_only_mode_enabled`).
