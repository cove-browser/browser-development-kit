import AppKit
import CBDK

// Context menus, new windows, DevTools, find, fullscreen and printing (ui-requests.md 5.5, 5.6,
// 5.9, 5.11, 5.12, 5.15).

public struct BDKMenuItem: Sendable, Equatable, Identifiable {
    public struct ID: Hashable, Sendable {
        public let rawValue: String

        public init(rawValue: String) {
            self.rawValue = rawValue
        }
    }

    public enum Kind: String, Sendable {
        case command
        case separator
        case submenu
        case checkbox
        case radio
    }

    public enum Source: Sendable, Equatable {
        case browser
        case `extension`(id: String)
        case application
    }

    public let id: ID
    public let kind: Kind
    /// What the item does, as a name ("openLinkInNewPage", "copy", "paste", "inspect", ...).
    public let role: String?
    public let title: String
    public let isEnabled: Bool
    public let isChecked: Bool
    public let children: [BDKMenuItem]
    public let source: Source

    public init(
        id: ID, kind: Kind = .command, role: String? = nil, title: String, isEnabled: Bool = true,
        isChecked: Bool = false, children: [BDKMenuItem] = [], source: Source = .application
    ) {
        self.id = id
        self.kind = kind
        self.role = role
        self.title = title
        self.isEnabled = isEnabled
        self.isChecked = isChecked
        self.children = children
        self.source = source
    }

    init(_ payload: BDKUIPayload) {
        id = ID(rawValue: payload.string("id") ?? "")
        kind = payload.choice("kind", default: .command)
        role = payload.string("role")
        title = payload.string("title") ?? ""
        isEnabled = payload.bool("isEnabled", default: true)
        isChecked = payload.bool("isChecked")
        children = payload.objects("children").map(BDKMenuItem.init)
        if payload.string("source") == "extension", let id = payload.string("extensionId") {
            source = .extension(id: id)
        } else {
            source = payload.string("source") == "application" ? .application : .browser
        }
    }

    /// This item and everything under it.
    var flattened: [BDKMenuItem] {
        [self] + children.flatMap(\.flattened)
    }

    var json: BDKJSValue {
        var fields: [String: BDKJSValue] = [
            "id": .string(id.rawValue), "kind": .string(kind.rawValue), "title": .string(title),
            "isEnabled": .bool(isEnabled), "isChecked": .bool(isChecked),
            "children": .array(children.map(\.json)),
        ]
        if let role { fields["role"] = .string(role) }
        return .object(fields)
    }
}

public final class ContextMenuRequest: BDKUIRequest {
    /// What the menu is about: the facts the default menu is built from.
    public struct Context: Sendable, Equatable {
        public let linkURL: URL?
        public let imageURL: URL?
        public let selectionText: String?
        public let isEditable: Bool
        public let misspelledWord: String?
        public let spellingSuggestions: [String]
        public let frameURL: URL?
        public let pageURL: URL?
    }

    /// In the page view.
    public let location: CGPoint
    public let context: Context
    /// The complete default menu, extension items included.
    public let items: [BDKMenuItem]

    override init(_ delivery: BDKUIDelivery, runtime: BDKRuntime) {
        let fields = BDKUIPayload(delivery)
        location = fields.point("location")
        let facts = fields.object("context") ?? BDKUIPayload([:])
        context = Context(
            linkURL: facts.url("linkURL"), imageURL: facts.url("imageURL"),
            selectionText: facts.string("selectionText"), isEditable: facts.bool("isEditable"),
            misspelledWord: facts.string("misspelledWord"),
            spellingSuggestions: facts.strings("spellingSuggestions"),
            frameURL: facts.url("frameURL"), pageURL: facts.url("pageURL"))
        items = fields.objects("items").map(BDKMenuItem.init)
        super.init(delivery, runtime: runtime)
    }

    /// BDK builds the menu from `items`: reorder, remove and add, then show.
    public func show(_ items: [BDKMenuItem], at point: CGPoint? = nil) {
        var arguments: [String: BDKJSValue] = [
            "action": .string("show"), "items": .array(items.map(\.json)),
        ]
        if let point { arguments["at"] = .object(["x": .number(point.x), "y": .number(point.y)]) }
        try? submit("accept", arguments)
    }

    /// Runs an item of BDK's own menu from the application's menu. Throws `ui.unknownItem` for an
    /// item the request did not offer.
    public func perform(_ item: BDKMenuItem) throws {
        guard items.flatMap(\.flattened).contains(where: { $0.id == item.id }) else {
            throw BDKError(
                domain: .ui, code: "unknownItem", message: "the request did not offer this item")
        }
        try submit("accept", ["action": .string("perform"), "item": .string(item.id.rawValue)])
    }

    public func dismiss() {
        try? submit("deny")
    }
}

public final class NewWindowRequest: BDKUIRequest {
    public enum Kind: String, Sendable {
        case popup
        case newPage
        case devTools
        case pictureInPicture
        case extensionWindow
        case application
        /// An installed web app being launched, or opening a window inside its scope.
        case webApp
        /// A payment handler's own page, while it takes a payment (`PaymentRequest`). It closes
        /// when the payment ends.
        case paymentHandler
        /// An identity provider's sign-in or other page during FedCM (`IdentityRequest`). It
        /// closes when the sign-in moves on.
        case identityProvider
    }

    public enum Disposition: String, Sendable {
        case foreground
        case background
        case newWindow
        case popup
        case privateWindow
    }

    /// The size and position `window.open` asked for; nil where it asked for none.
    public struct WindowFeatures: Sendable, Equatable {
        public let x: Double?
        public let y: Double?
        public let width: Double?
        public let height: Double?
    }

    public let kind: Kind
    public let disposition: Disposition
    public let targetURL: URL?
    public let opener: BDKWebContents?
    public let features: WindowFeatures
    public let isUserGesture: Bool
    /// The extension that asked for the page (its options page, a link in its popup), or nil when a
    /// page asked.
    public let initiatorExtensionID: String?
    /// Kind `.webApp` only: the installed app being launched. Its page is `newContents`, in the
    /// app's profile, and reports `webApp` until it is closed (web-apps.md 4.2).
    public let webApp: BDKWebAppInfo?
    private let newContentsHandle: UInt64?
    private var adoptedContents: BDKWebContents?

    override init(_ delivery: BDKUIDelivery, runtime: BDKRuntime) {
        let fields = BDKUIPayload(delivery)
        kind = fields.choice("kind", default: .newPage)
        disposition = fields.choice("disposition", default: .foreground)
        targetURL = fields.url("targetURL")
        opener = fields.handle("opener").flatMap { runtime.webContents(handle: $0) }
        let size = fields.object("features") ?? BDKUIPayload([:])
        features = WindowFeatures(
            x: size.double("x"), y: size.double("y"), width: size.double("width"),
            height: size.double("height"))
        // The payload names it "userGesture" (engine_contents.cc, tests_engine.c).
        isUserGesture = fields.bool("userGesture")
        initiatorExtensionID = fields.object("initiator")?.string("extensionId")
        webApp = fields.fields["webApp"].flatMap {
            try? BDKServiceCoding.decode(BDKWebAppInfo.self, from: $0.jsonString)
        }
        newContentsHandle = fields.handle("newContents")
        super.init(delivery, runtime: runtime)
        // A page an extension opened is announced as such (amendments 2.18, T-EXT-12).
        if let initiatorExtensionID, let page = newContents {
            runtime.publish(.webContentsCreated(page, by: .extension(initiatorExtensionID)))
        }
    }

    /// The page the runtime already created for the request, which has an opener relationship where
    /// the web requires one. Nil when the runtime named no page.
    public var newContents: BDKWebContents? {
        if adoptedContents == nil, let newContentsHandle, let owner = profile ?? opener?.profile {
            adoptedContents = adoptPage(handle: newContentsHandle, in: owner)
            if let webApp { adoptedContents?.noteWebApp(webApp) }
        }
        return adoptedContents
    }

    /// The application has placed `newContents.view`, or will.
    public func accept() {
        try? submit("accept")
    }

    /// The runtime destroys `newContents`.
    public func reject() {
        try? submit("deny")
        adoptedContents?.invalidate()
    }
}

public final class DevToolsRequest: BDKUIRequest {
    public enum Dock: String, Sendable {
        case undocked
        case bottom
        case right
        case left
    }

    public let inspected: BDKWebContents?
    public let preferredDock: Dock
    private let frontendHandle: UInt64?
    private var adoptedFrontend: BDKWebContents?

    override init(_ delivery: BDKUIDelivery, runtime: BDKRuntime) {
        let fields = BDKUIPayload(delivery)
        inspected = fields.handle("inspected").flatMap { runtime.webContents(handle: $0) }
        preferredDock = fields.choice("preferredDock", default: .undocked)
        frontendHandle = fields.handle("frontend")
        super.init(delivery, runtime: runtime)
    }

    /// The DevTools front end, as a page.
    public var frontend: BDKWebContents? {
        if adoptedFrontend == nil, let frontendHandle, let owner = profile ?? inspected?.profile {
            adoptedFrontend = adoptPage(handle: frontendHandle, in: owner)
        }
        return adoptedFrontend
    }

    /// The application has placed `frontend.view`.
    public func accept() {
        try? submit("accept")
    }

    public func reject() {
        try? submit("deny")
        adoptedFrontend?.invalidate()
    }
}

public final class PrintRequest: BDKUIRequest {
    /// False for `window.print()`.
    public let isUserInitiated: Bool
    private let previewHandle: UInt64?
    private var adoptedPreview: BDKWebContents?

    override init(_ delivery: BDKUIDelivery, runtime: BDKRuntime) {
        let fields = BDKUIPayload(delivery)
        isUserInitiated = fields.bool("isUserInitiated", default: true)
        previewHandle = fields.handle("previewContents")
        super.init(delivery, runtime: runtime)
    }

    /// Chromium's print preview, as a page the application may host.
    public var previewContents: BDKWebContents? {
        if adoptedPreview == nil, let previewHandle, let owner = profile ?? contents?.profile {
            adoptedPreview = adoptPage(handle: previewHandle, in: owner)
        }
        return adoptedPreview
    }

    public func printWithSystemDialog() {
        try? submit("accept", ["action": .string("systemDialog")])
    }

    public func savePDF(to url: URL) {
        try? submit("accept", ["action": .string("savePDF"), "path": .string(url.path)])
    }

    public func cancel() {
        try? submit("deny")
    }
}

public final class FindRequest: BDKUIRequest {
    /// The selection, or the last search.
    public let initialText: String?

    override init(_ delivery: BDKUIDelivery, runtime: BDKRuntime) {
        initialText = BDKUIPayload(delivery).string("initialText")
        super.init(delivery, runtime: runtime)
    }

    /// The page's find session, the same object as `page.find`, so a handler's own find field can
    /// search through it. Nil when the request names no page.
    public var session: FindSession? {
        contents?.find
    }

    public func close() {
        try? submit("accept", ["action": .string("close")])
    }
}

public final class FullscreenRequest: BDKUIRequest {
    public let origin: URL
    public let isEntering: Bool

    override init(_ delivery: BDKUIDelivery, runtime: BDKRuntime) {
        let fields = BDKUIPayload(delivery)
        origin = fields.requiredURL("origin")
        isEntering = fields.bool("isEntering", default: true)
        super.init(delivery, runtime: runtime)
    }

    /// The application has made, or will make, the page view fill the screen.
    public func allow() {
        try? submit("accept")
    }

    public func deny() {
        try? submit("deny")
    }
}

public final class ExclusiveAccessRequest: BDKUIRequest {
    public enum Kind: String, Sendable {
        case keyboardLock
        case pointerLock
    }

    public let kind: Kind
    public let origin: URL
    /// "Press and hold Esc to exit".
    public let exitInstruction: String

    override init(_ delivery: BDKUIDelivery, runtime: BDKRuntime) {
        let fields = BDKUIPayload(delivery)
        kind = fields.choice("kind", default: .pointerLock)
        origin = fields.requiredURL("origin")
        exitInstruction = fields.string("exitInstruction") ?? ""
        super.init(delivery, runtime: runtime)
    }

    /// The application showed the instruction.
    public func acknowledge() {
        try? submit("accept")
    }
}
