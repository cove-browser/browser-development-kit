import Foundation

/// A browser-owned surface (ui-requests.md section 4). The raw value is `BDK_SURFACE_*`.
public enum BDKUISurface: UInt32, Sendable, CaseIterable {
    case permissions = 1
    case javascriptDialogs = 2
    case httpAuth = 3
    case clientCertificates = 4
    case filePickers = 5
    case contextMenus = 6
    case newWindows = 7
    case downloads = 8
    case passwords = 9
    case autofill = 10
    case autofillSuggestions = 11
    case printing = 12
    case translation = 13
    case find = 14
    case fullscreen = 15
    case exclusiveAccess = 16
    case notifications = 17
    case certificateErrors = 18
    case reputationWarnings = 19
    case devTools = 20
    case deviceChoosers = 21
    case screenCapture = 22
    case webAuthn = 23
    case externalProtocols = 24
    case extensionActions = 25
    case extensionPopups = 26
    case extensionSidePanels = 27
    case extensionContextMenus = 28
    case extensionOptionsPages = 29
    case extensionDevTools = 30
    case extensionInstalls = 31
    case extensionUninstalls = 32
    case pageHealth = 33
    case webAppInstalls = 37

    /// The key under `[ui.surfaces]` in `bdk.toml` and in `bdk.resolved.json`.
    public var configurationKey: String {
        switch self {
        case .permissions: "permissions"
        case .javascriptDialogs: "javascript_dialogs"
        case .httpAuth: "http_auth"
        case .clientCertificates: "client_certificates"
        case .filePickers: "file_pickers"
        case .contextMenus: "context_menus"
        case .newWindows: "new_windows"
        case .downloads: "downloads"
        case .passwords: "passwords"
        case .autofill: "autofill"
        case .autofillSuggestions: "autofill_suggestions"
        case .printing: "printing"
        case .translation: "translation"
        case .find: "find"
        case .fullscreen: "fullscreen"
        case .exclusiveAccess: "exclusive_access"
        case .notifications: "notifications"
        case .certificateErrors: "certificate_errors"
        case .reputationWarnings: "reputation_warnings"
        case .devTools: "devtools"
        case .deviceChoosers: "device_choosers"
        case .screenCapture: "screen_capture"
        case .webAuthn: "webauthn"
        case .externalProtocols: "external_protocols"
        case .extensionActions: "actions"
        case .extensionPopups: "popups"
        case .extensionSidePanels: "side_panels"
        case .extensionContextMenus: "context_menus"
        case .extensionOptionsPages: "options_pages"
        case .extensionDevTools: "devtools"
        case .extensionInstalls: "extension_installs"
        case .extensionUninstalls: "extension_uninstalls"
        case .pageHealth: "page_health"
        case .webAppInstalls: "web_app_installs"
        }
    }

    /// True for the surfaces configured under `[extensions.ui]` rather than `[ui.surfaces]`. The
    /// install and uninstall prompts are configured in neither: they cannot be disabled.
    var isConfiguredWithExtensions: Bool {
        switch self {
        case .extensionActions, .extensionPopups, .extensionSidePanels, .extensionContextMenus,
            .extensionOptionsPages, .extensionDevTools:
            true
        default: false
        }
    }
}

/// The mode of a surface, from `[ui.surfaces]` (configuration.md section 3.6).
public enum BDKUISurfaceMode: String, Sendable, Equatable {
    case `default`
    case custom
    case disabled
}
