import AppKit
import CBDK

// Extension popups, side panels and the install and uninstall prompts (extensions.md 4.2, 4.3,
// 4.6; ui-requests.md 5.x). A popup and a side panel stay open while their view is on screen, as
// the find bar does; they end with a completion or a withdrawal.

extension BDKUIDelivery {
    /// The browser-managed view the request carries, or an empty view standing in for one when the
    /// runtime has no native views (the stub).
    @MainActor
    fileprivate var secureView: NSView {
        guard secureViewKind == BDK_NATIVE_NSVIEW.rawValue,
            let pointer = UnsafeRawPointer(bitPattern: secureViewAddress)
        else { return NSView() }
        return Unmanaged<NSView>.fromOpaque(pointer).takeUnretainedValue()
    }
}

public final class ExtensionPopupRequest: BDKUIRequest {
    public enum Trigger: String, Sendable {
        case userClick
        case openPopupAPI
        case command
    }

    public let extensionID: String
    public let extensionName: String
    public let url: URL?
    public let trigger: Trigger
    /// Browser-managed: the application positions and sizes its container and may not read or
    /// script the view. It must be able to become key.
    public let secureView: NSView
    /// Changes as the popup lays out; observe `sizeChanges`.
    public private(set) var preferredSize: CGSize
    private let sizes = EventBroadcaster<CGSize>()

    override init(_ delivery: BDKUIDelivery, runtime: BDKRuntime) {
        let fields = BDKUIPayload(delivery)
        extensionID = fields.string("extensionId") ?? ""
        extensionName = fields.string("extensionName") ?? ""
        url = fields.url("url")
        trigger = fields.choice("trigger", default: .userClick)
        secureView = delivery.secureView
        preferredSize = Self.size(fields.object("preferredSize"))
        super.init(delivery, runtime: runtime)
        observeUpdates { [weak self] update in self?.receive(update) }
        cancellation.onFire { [sizes] in sizes.finish() }
    }

    private static func size(_ fields: BDKUIPayload?) -> CGSize {
        CGSize(width: fields?.double("width") ?? 0, height: fields?.double("height") ?? 0)
    }

    private func receive(_ update: [String: BDKJSValue]) {
        guard let size = update["preferredSize"]?.objectValue else { return }
        preferredSize = Self.size(BDKUIPayload(size))
        sizes.send(preferredSize)
    }

    /// The popup's preferred size, each time it changes.
    public var sizeChanges: BDKEventSequence<CGSize> {
        BDKEventSequence(sizes)
    }

    /// Fires when the popup closed itself (`window.close()`, blur, navigation) or BDK closed it.
    /// The application dismisses its container.
    public var closed: BDKUICancellation {
        cancellation
    }

    /// The application has put `secureView` on screen.
    public func accept() {
        try? sendUpdate(["placed": .bool(true)])
    }

    /// The application will not show this popup.
    public func reject() {
        try? submit("deny")
        sizes.finish()
    }

    /// The application closed its container.
    public func close() {
        try? submit("accept", ["action": .string("close")])
        sizes.finish()
    }
}

public final class ExtensionSidePanelRequest: BDKUIRequest {
    public enum Scope: Sendable {
        /// One panel for the window.
        case window
        /// The extension chose a panel per page.
        case page(BDKWebContents)
    }

    /// Which edge of its window the panel is on, as `chrome.sidePanel.getLayout()` reports it.
    public enum Side: String, Sendable {
        case leading
        case trailing
    }

    public let extensionID: String
    public let title: String
    public let icon: NSImage?
    public let url: URL?
    public let scope: Scope
    public let secureView: NSView
    private var currentSide = Side.trailing
    private let visibility = EventBroadcaster<Bool>()

    override init(_ delivery: BDKUIDelivery, runtime: BDKRuntime) {
        let fields = BDKUIPayload(delivery)
        extensionID = fields.string("extensionId") ?? ""
        title = fields.string("title") ?? ""
        icon = BDKImageDecoding.image(fromDataURL: fields.string("icon"))
        url = fields.url("url")
        secureView = delivery.secureView
        let page = runtime.webContents(handle: fields.handle("contents") ?? delivery.contents)
        if fields.string("scope") == "page", let page {
            scope = .page(page)
        } else {
            scope = .window
        }
        super.init(delivery, runtime: runtime)
        observeUpdates { [weak self] update in
            if let visible = update["visible"]?.boolValue { self?.visibility.send(visible) }
        }
        cancellation.onFire { [visibility] in visibility.finish() }
    }

    /// When a page-scoped panel should show (`true`) and hide. A custom host follows it.
    public var visibilityChanges: BDKEventSequence<Bool> {
        BDKEventSequence(visibility)
    }

    public var closed: BDKUICancellation {
        cancellation
    }

    /// Where the panel is. Set it when the application moves the panel.
    public var side: Side {
        get { currentSide }
        set {
            currentSide = newValue
            try? sendUpdate(["side": .string(newValue.rawValue)])
        }
    }

    /// The application has put `secureView` on screen, on `side`.
    public func accept(side: Side = .trailing) {
        currentSide = side
        try? sendUpdate(["placed": .bool(true), "side": .string(side.rawValue)])
    }

    public func reject() {
        try? submit("deny")
        visibility.finish()
    }

    public func close() {
        try? submit("accept", ["action": .string("close")])
        visibility.finish()
    }
}

public final class ExtensionInstallRequest: BDKUIRequest {
    public enum Kind: String, Sendable {
        case install
        case permissionsIncrease
        case optionalPermissions
        case reEnable
    }

    public let kind: Kind
    /// Absent before the package is unpacked.
    public let extensionID: String?
    public let extensionName: String
    public let icon: NSImage?
    public let source: BDKExtensionSourceKind
    /// Chromium's permission warnings, in its wording and order. A custom presenter shows them all.
    public let warnings: [String]
    public let hostWarnings: [String]
    public let compatibility: BDKCompatibilityReport

    override init(_ delivery: BDKUIDelivery, runtime: BDKRuntime) {
        let fields = BDKUIPayload(delivery)
        kind = fields.choice("kind", default: .install)
        extensionID = fields.string("extensionId")
        extensionName = fields.string("extensionName") ?? ""
        icon = BDKImageDecoding.image(fromDataURL: fields.string("icon"))
        source = fields.choice("source", default: .unpacked)
        warnings = fields.strings("warnings")
        hostWarnings = fields.strings("hostWarnings")
        compatibility =
            fields.fields["compatibility"]
            .flatMap {
                try? BDKServiceCoding.decode(BDKCompatibilityReport.self, from: $0.jsonString)
            }
            ?? BDKCompatibilityReport(verdict: .compatible, findings: [])
        super.init(delivery, runtime: runtime)
    }

    /// The user confirmed the install. `event` is the click: without a gesture it throws
    /// `ui.gestureRequired`.
    public func accept(confirmedBy event: NSEvent) throws {
        try submit("grant", gesture: event)
    }

    /// The install is refused with `extension.cancelled`.
    public func cancel() {
        try? submit("deny")
    }
}

public final class ExtensionUninstallRequest: BDKUIRequest {
    public enum Requester: Sendable, Equatable {
        case user
        /// Another extension, through `chrome.management`.
        case `extension`(id: String)
    }

    public let extensionID: String
    public let extensionName: String
    public let requestedBy: Requester

    override init(_ delivery: BDKUIDelivery, runtime: BDKRuntime) {
        let fields = BDKUIPayload(delivery)
        extensionID = fields.string("extensionId") ?? ""
        extensionName = fields.string("extensionName") ?? ""
        let requester = fields.object("requestedBy")
        if requester?.string("kind") == "extension", let id = requester?.string("extensionId") {
            requestedBy = .extension(id: id)
        } else {
            requestedBy = .user
        }
        super.init(delivery, runtime: runtime)
    }

    public func confirm(confirmedBy event: NSEvent) throws {
        try submit("grant", gesture: event)
    }

    public func cancel() {
        try? submit("deny")
    }
}
