import AppKit
import CBDK
import Foundation

// What `profile.extensions` adds to the generated commands (extensions.md sections 2 and 4): the
// install sources, the action states of a page, and the calls that need the user's own gesture.

extension BDKExtensionSource {
    /// A directory with a `manifest.json`.
    public static func unpacked(_ directory: URL) -> BDKExtensionSource {
        BDKExtensionSource(kind: .unpacked, path: directory.path)
    }

    /// A packed `.crx` file.
    public static func crx(_ file: URL) -> BDKExtensionSource {
        BDKExtensionSource(kind: .crx, path: file.path)
    }

    /// An extension served from an update URL.
    public static func updateURL(id: String, url: URL) -> BDKExtensionSource {
        BDKExtensionSource(kind: .updateUrl, id: id, updateUrl: url)
    }

    /// An extension from a store named in `bdk.toml`.
    public static func store(name: String, id: String) -> BDKExtensionSource {
        BDKExtensionSource(kind: .store, id: id, store: name)
    }
}

extension BDKExtensionInfo {
    /// The icon, decoded from the `data:` URL the runtime sends. Nil for none.
    public var iconImage: NSImage? {
        BDKImageDecoding.image(fromDataURL: icon)
    }
}

extension BDKExtensionActionState {
    public var iconImage: NSImage? {
        BDKImageDecoding.image(fromDataURL: icon)
    }

    /// The badge background, from `#RRGGBB` or `#RRGGBBAA`. Nil for none or another notation.
    public var badgeNSColor: NSColor? {
        badgeColor.flatMap(BDKImageDecoding.color(fromHex:))
    }
}

/// Decodes what the runtime sends for pictures and colours.
enum BDKImageDecoding {
    static func image(fromDataURL text: String?) -> NSImage? {
        guard let text, text.hasPrefix("data:"), let comma = text.firstIndex(of: ",") else {
            return nil
        }
        let header = text[text.index(text.startIndex, offsetBy: 5)..<comma]
        let body = String(text[text.index(after: comma)...])
        let data =
            header.hasSuffix(";base64")
            ? Data(base64Encoded: body) : body.removingPercentEncoding.map { Data($0.utf8) }
        return data.flatMap { NSImage(data: $0) }
    }

    static func color(fromHex text: String) -> NSColor? {
        guard text.hasPrefix("#"), let value = UInt32(text.dropFirst(), radix: 16) else {
            return nil
        }
        switch text.count {
        case 7:
            return NSColor(
                srgbRed: CGFloat((value >> 16) & 0xFF) / 255,
                green: CGFloat((value >> 8) & 0xFF) / 255, blue: CGFloat(value & 0xFF) / 255,
                alpha: 1)
        case 9:
            return NSColor(
                srgbRed: CGFloat((value >> 24) & 0xFF) / 255,
                green: CGFloat((value >> 16) & 0xFF) / 255,
                blue: CGFloat((value >> 8) & 0xFF) / 255, alpha: CGFloat(value & 0xFF) / 255)
        default:
            return nil
        }
    }
}

extension BDKExtensions {
    /// What a click on an extension's action did, as Chromium decided (extensions.md 4.1).
    public enum ActivationOutcome: Sendable, Equatable {
        /// `action.onClicked` fired and `activeTab` is granted for the page.
        case dispatchedClick
        /// An `ExtensionPopupRequest` follows.
        case popupRequested
        /// An `ExtensionSidePanelRequest` follows.
        case sidePanelRequested
        /// The extension needs site access; a permission request follows.
        case accessRequested
        case blocked(reason: String)

        init(json: String) throws {
            let fields = BDKJSValue(jsonString: json)?.objectValue
            switch fields?["outcome"]?.stringValue {
            case "dispatchedClick": self = .dispatchedClick
            case "popupRequested": self = .popupRequested
            case "sidePanelRequested": self = .sidePanelRequested
            case "accessRequested": self = .accessRequested
            case "blocked": self = .blocked(reason: fields?["reason"]?.stringValue ?? "")
            default:
                throw BDKError(
                    domain: .runtime, code: "malformedResult",
                    message: "the runtime returned an activation outcome this version cannot read")
            }
        }
    }

    /// Installs an extension. Always raises the install prompt (`ExtensionInstallRequest`).
    public func install(_ source: BDKExtensionSource) async throws -> BDKExtensionInfo {
        try await install(source: source)
    }

    /// A compatibility report for a package, before installing it.
    public func inspect(_ source: BDKExtensionSource) async throws -> BDKCompatibilityReport {
        try await inspect(source: source, id: nil)
    }

    /// The action states for the page: what its window's toolbar would show, in the user's order.
    /// Asynchronous, unlike the synchronous call extensions.md sketches, because it is a command.
    public func actions(for page: BDKWebContents) async throws -> [BDKExtensionActionState] {
        let json = try await scope.runtime.commands.invoke(
            "extensions.actions", arguments: "{}", profile: scope.profile, contents: page)
        return try BDKServiceCoding.decode([BDKExtensionActionState].self, from: json)
    }

    /// Changes to an action's state (title, badge, pin, popup, access). Re-read with `actions(for:)`.
    public var actionChanges: AsyncStream<Event.ActionChanged> {
        scope.events(["extensions.actionChanged"]) { _, payload in
            try BDKServiceCoding.decode(Event.ActionChanged.self, from: payload)
        }
    }

    /// What a click on the extension's action does to the page the user is looking at. `event` is
    /// the click: without a gesture it throws `ui.gestureRequired`. A popup or side panel follows as
    /// a request.
    public func activateAction(
        _ id: String, on page: BDKWebContents, confirmedBy event: NSEvent
    ) throws -> ActivationOutcome {
        let handle = try page.liveHandle()
        var buffer = bdk_buffer_t()
        try withBDKString(id) { lent in
            try abiCall(.contents) {
                bdk_extension_action_activate(handle, lent, gestureHandle(event), &buffer, $0)
            }
        }
        return try ActivationOutcome(json: String(decoding: takeBuffer(buffer), as: UTF8.self))
    }

    /// Runs a keyboard command an extension declared, as its shortcut would. The application owns
    /// the key bindings. Same gesture rule and result as `activateAction`.
    public func performCommand(
        _ command: BDKExtensionCommand, on page: BDKWebContents, confirmedBy event: NSEvent
    ) throws -> ActivationOutcome {
        let handle = try page.liveHandle()
        var buffer = bdk_buffer_t()
        try withBDKStrings([command.extensionId, command.name]) { lent in
            try abiCall(.contents) {
                bdk_extension_command_perform(
                    handle, lent[0], lent[1], gestureHandle(event), &buffer, $0)
            }
        }
        return try ActivationOutcome(json: String(decoding: takeBuffer(buffer), as: UTF8.self))
    }

    /// Shows BDK's default extensions menu for the window's focused page, at its `extensions`
    /// anchor. Throws `extension.surfaceDisabled` when actions are not in default mode.
    public func presentDefaultMenu(in window: BDKWindow, confirmedBy event: NSEvent) throws {
        guard window.handle != 0 else { throw BDKError.stale(.runtime) }
        try abiCall(.runtime) {
            bdk_extension_menu_present(window.handle, gestureHandle(event), $0)
        }
    }
}
