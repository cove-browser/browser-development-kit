import AppKit
import UniformTypeIdentifiers

// Permissions, JavaScript dialogs, authentication and file pickers (ui-requests.md 5.1 to 5.4).
//
// Completions are "grant" (needs the confirming event), "deny" and "accept"; the arguments carry
// the rest. The arguments of each completion are documented in packages/swift/README.md.

public struct BDKPermissionKind: RawRepresentable, Hashable, Sendable {
    public let rawValue: String

    public init(rawValue: String) {
        self.rawValue = rawValue
    }

    public static let camera = BDKPermissionKind(rawValue: "camera")
    public static let microphone = BDKPermissionKind(rawValue: "microphone")
    public static let geolocation = BDKPermissionKind(rawValue: "geolocation")
    public static let notifications = BDKPermissionKind(rawValue: "notifications")
    public static let clipboardRead = BDKPermissionKind(rawValue: "clipboardRead")
    public static let midi = BDKPermissionKind(rawValue: "midi")
    public static let storageAccess = BDKPermissionKind(rawValue: "storageAccess")
    public static let localFonts = BDKPermissionKind(rawValue: "localFonts")
    public static let windowManagement = BDKPermissionKind(rawValue: "windowManagement")
    public static let idleDetection = BDKPermissionKind(rawValue: "idleDetection")
}

public final class PermissionRequest: BDKUIRequest {
    public enum GrantDuration: String, Sendable {
        case persistent
        case thisVisit
    }

    /// The requesting origin.
    public let origin: URL
    /// The top-level origin, when different.
    public let embeddingOrigin: URL?
    public let kinds: Set<BDKPermissionKind>
    /// The wording the default presenter shows.
    public let text: BDKUIText
    public let offersOneTime: Bool

    override init(_ delivery: BDKUIDelivery, runtime: BDKRuntime) {
        let fields = BDKUIPayload(delivery)
        origin = fields.requiredURL("origin")
        embeddingOrigin = fields.url("embeddingOrigin")
        // `permission` is the single-kind spelling.
        let names = fields.strings("kinds") + (fields.string("permission").map { [$0] } ?? [])
        kinds = Set(names.map(BDKPermissionKind.init(rawValue:)))
        text = BDKUIText(fields.object("text"))
        offersOneTime = fields.bool("offersOneTime")
        super.init(delivery, runtime: runtime)
    }

    /// Grants `kinds` (all that were asked when nil). A grant may cover fewer kinds than were
    /// asked, never more: other kinds throw `ui.kindsNotRequested`.
    public func grant(
        _ kinds: Set<BDKPermissionKind>? = nil, duration: GrantDuration = .persistent,
        confirmedBy event: NSEvent
    ) throws {
        let granted = kinds ?? self.kinds
        guard granted.isSubset(of: self.kinds) else {
            throw BDKError(
                domain: .ui, code: "kindsNotRequested",
                message: "a grant cannot cover kinds the request did not ask for")
        }
        try submit(
            "grant",
            [
                "kinds": .array(granted.map(\.rawValue).sorted().map(BDKJSValue.string)),
                "duration": .string(duration.rawValue),
            ], gesture: event)
    }

    public func deny(persist: Bool = true) {
        try? submit("deny", ["persist": .bool(persist)])
    }

    /// No decision; the site may ask again.
    public func dismiss() {
        try? submit("deny", ["persist": .bool(false), "dismissed": .bool(true)])
    }
}

public final class JavaScriptDialogRequest: BDKUIRequest {
    public enum Kind: String, Sendable {
        case alert
        case confirm
        case prompt
        case beforeUnload
    }

    public let kind: Kind
    public let origin: URL
    public let message: String
    public let defaultPromptText: String?
    /// For `.beforeUnload`.
    public let isReload: Bool
    /// Chromium offers "prevent further dialogs" after repeats.
    public let offersSuppression: Bool

    override init(_ delivery: BDKUIDelivery, runtime: BDKRuntime) {
        let fields = BDKUIPayload(delivery)
        kind = fields.choice("kind", default: .alert)
        origin = fields.requiredURL("origin")
        message = fields.string("message") ?? ""
        defaultPromptText = fields.string("defaultPromptText")
        isReload = fields.bool("isReload")
        offersSuppression = fields.bool("offersSuppression")
        super.init(delivery, runtime: runtime)
    }

    public func accept(text: String? = nil, suppressFurther: Bool = false) {
        var arguments: [String: BDKJSValue] = ["suppressFurther": .bool(suppressFurther)]
        if let text { arguments["text"] = .string(text) }
        try? submit("accept", arguments)
    }

    public func cancel(suppressFurther: Bool = false) {
        try? submit("deny", ["suppressFurther": .bool(suppressFurther)])
    }
}

public final class HTTPAuthRequest: BDKUIRequest {
    /// The host asking.
    public let challenger: URL
    public let realm: String?
    public let isProxy: Bool
    /// "basic", "digest", "ntlm" or "negotiate".
    public let scheme: String
    public let savedCredentialsAvailable: Bool

    override init(_ delivery: BDKUIDelivery, runtime: BDKRuntime) {
        let fields = BDKUIPayload(delivery)
        challenger = fields.requiredURL("challenger")
        realm = fields.string("realm")
        isProxy = fields.bool("isProxy")
        scheme = fields.string("scheme") ?? "basic"
        savedCredentialsAvailable = fields.bool("savedCredentialsAvailable")
        super.init(delivery, runtime: runtime)
    }

    public func provide(username: String, password: String) {
        try? submit("accept", ["username": .string(username), "password": .string(password)])
    }

    public func cancel() {
        try? submit("deny")
    }
}

public final class ClientCertificateRequest: BDKUIRequest {
    public let host: String
    public let certificates: [BDKCertificateSummary]

    override init(_ delivery: BDKUIDelivery, runtime: BDKRuntime) {
        let fields = BDKUIPayload(delivery)
        host = fields.string("host") ?? ""
        certificates = fields.objects("certificates").map(BDKCertificateSummary.init)
        super.init(delivery, runtime: runtime)
    }

    /// Throws `ui.certificateNotOffered` for a certificate the request did not list.
    public func select(_ certificate: BDKCertificateSummary) throws {
        guard certificates.contains(certificate) else {
            throw BDKError(
                domain: .ui, code: "certificateNotOffered",
                message: "the certificate is not one the request offered")
        }
        try submit("accept", ["certificate": .string(certificate.id)])
    }

    public func cancel() {
        try? submit("deny")
    }
}

public final class FilePickerRequest: BDKUIRequest {
    public enum Mode: String, Sendable {
        case open
        case openMultiple
        case directory
        case save
    }

    public let mode: Mode
    public let acceptedTypes: [UTType]
    public let suggestedName: String?
    public let isFileSystemAccessAPI: Bool

    override init(_ delivery: BDKUIDelivery, runtime: BDKRuntime) {
        let fields = BDKUIPayload(delivery)
        mode = fields.choice("mode", default: .open)
        // A type is an identifier ("public.png"), a MIME type or a file extension.
        acceptedTypes = fields.strings("acceptedTypes").compactMap {
            UTType($0) ?? UTType(mimeType: $0)
                ?? UTType(filenameExtension: $0.hasPrefix(".") ? String($0.dropFirst()) : $0)
        }
        suggestedName = fields.string("suggestedName")
        isFileSystemAccessAPI = fields.bool("isFileSystemAccessAPI")
        super.init(delivery, runtime: runtime)
    }

    /// Throws `ui.tooManyFiles` when `urls` has several entries and the mode is not
    /// `.openMultiple`.
    public func choose(_ urls: [URL]) throws {
        guard mode == .openMultiple || urls.count <= 1 else {
            throw BDKError(
                domain: .ui, code: "tooManyFiles",
                message: "the request takes one path, not \(urls.count)")
        }
        try submit("accept", ["paths": .array(urls.map { .string($0.path) })])
    }

    public func cancel() {
        try? submit("deny")
    }
}
