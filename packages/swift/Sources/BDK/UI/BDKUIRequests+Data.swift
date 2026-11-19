import AppKit

// Downloads, passwords, autofill, notifications, interstitials, external protocols and the
// surfaces that have no typed request yet (ui-requests.md 5.7, 5.8, 5.13, 5.14, 5.17).

public struct BDKDownloadInfo: Sendable, Equatable {
    public let url: URL
    public let suggestedName: String
    public let mimeType: String?
    public let size: Int64?
    public let referrer: URL?

    init(_ payload: BDKUIPayload?) {
        let fields = payload ?? BDKUIPayload([:])
        url = fields.requiredURL("url")
        suggestedName = fields.string("suggestedName") ?? ""
        mimeType = fields.string("mimeType")
        size = fields.double("size").map { Int64($0) }
        referrer = fields.url("referrer")
    }
}

public final class DownloadRequest: BDKUIRequest {
    public let download: BDKDownloadInfo
    /// The "ask where to save" setting, or a save-as.
    public let needsLocation: Bool

    override init(_ delivery: BDKUIDelivery, runtime: BDKRuntime) {
        let fields = BDKUIPayload(delivery)
        download = BDKDownloadInfo(fields.object("download"))
        needsLocation = fields.bool("needsLocation")
        super.init(delivery, runtime: runtime)
    }

    public func proceed(to destination: URL? = nil) {
        var arguments: [String: BDKJSValue] = [:]
        if let destination { arguments["destination"] = .string(destination.path) }
        try? submit("accept", arguments)
    }

    public func cancel() {
        try? submit("deny")
    }
}

public final class DangerousDownloadRequest: BDKUIRequest {
    public enum Danger: String, Sendable {
        case dangerousFileType
        case dangerousURL
        case dangerousContent
        case uncommon
        case insecure
    }

    public let download: BDKDownloadInfo
    public let danger: Danger
    public let text: BDKUIText

    override init(_ delivery: BDKUIDelivery, runtime: BDKRuntime) {
        let fields = BDKUIPayload(delivery)
        download = BDKDownloadInfo(fields.object("download"))
        danger = fields.choice("danger", default: .dangerousContent)
        text = BDKUIText(fields.object("text"))
        super.init(delivery, runtime: runtime)
    }

    public func keep(confirmedBy event: NSEvent) throws {
        try submit("grant", gesture: event)
    }

    public func discard() {
        try? submit("deny")
    }
}

public final class PasswordSaveRequest: BDKUIRequest {
    public let origin: URL
    public let username: String
    public let isUpdate: Bool

    override init(_ delivery: BDKUIDelivery, runtime: BDKRuntime) {
        let fields = BDKUIPayload(delivery)
        origin = fields.requiredURL("origin")
        username = fields.string("username") ?? ""
        isUpdate = fields.bool("isUpdate")
        super.init(delivery, runtime: runtime)
    }

    public func save(confirmedBy event: NSEvent) throws {
        try submit("grant", gesture: event)
    }

    /// Never for this site.
    public func never() {
        try? submit("deny", ["never": .bool(true)])
    }

    public func dismiss() {
        try? submit("deny", ["dismissed": .bool(true)])
    }
}

/// Informs; it grants nothing and has no accepting completion.
public final class LeakedPasswordRequest: BDKUIRequest {
    public let origin: URL
    public let username: String
    /// The login is in the store.
    public let isSaved: Bool
    public let provider: BDKProviderDescriptor?

    override init(_ delivery: BDKUIDelivery, runtime: BDKRuntime) {
        let fields = BDKUIPayload(delivery)
        origin = fields.requiredURL("origin")
        username = fields.string("username") ?? ""
        isSaved = fields.bool("isSaved")
        provider = BDKProviderDescriptor(fields)
        super.init(delivery, runtime: runtime)
    }

    public func dismiss() {
        try? submit("deny", ["dismissed": .bool(true)])
    }
}

public final class AutofillSaveRequest: BDKUIRequest {
    public enum Kind: String, Sendable {
        case address
        case card
    }

    public let kind: Kind
    /// What would be saved, masked for cards.
    public let summary: String

    override init(_ delivery: BDKUIDelivery, runtime: BDKRuntime) {
        let fields = BDKUIPayload(delivery)
        kind = fields.choice("kind", default: .address)
        summary = fields.string("summary") ?? ""
        super.init(delivery, runtime: runtime)
    }

    public func save(confirmedBy event: NSEvent) throws {
        try submit("grant", gesture: event)
    }

    public func dismiss() {
        try? submit("deny", ["dismissed": .bool(true)])
    }
}

public final class AutofillSuggestionsRequest: BDKUIRequest {
    public struct Suggestion: Sendable, Equatable, Identifiable {
        public enum Kind: String, Sendable {
            case password
            case address
            case card
            case generatedPassword
        }

        public let id: String
        public let label: String
        public let sublabel: String?
        public let kind: Kind
    }

    public let fieldRect: CGRect
    public let suggestions: [Suggestion]

    override init(_ delivery: BDKUIDelivery, runtime: BDKRuntime) {
        let fields = BDKUIPayload(delivery)
        fieldRect = fields.rect("fieldRect")
        suggestions = fields.objects("suggestions").map {
            Suggestion(
                id: $0.string("id") ?? "", label: $0.string("label") ?? "",
                sublabel: $0.string("sublabel"), kind: $0.choice("kind", default: .address))
        }
        super.init(delivery, runtime: runtime)
    }

    /// Filling a credential or a card needs the gesture on the suggestion. Throws
    /// `ui.unknownSuggestion` for one the request did not offer.
    public func accept(_ suggestion: Suggestion, confirmedBy event: NSEvent) throws {
        guard suggestions.contains(suggestion) else {
            throw BDKError(
                domain: .ui, code: "unknownSuggestion",
                message: "the request did not offer this suggestion")
        }
        try submit("grant", ["suggestion": .string(suggestion.id)], gesture: event)
    }

    public func dismiss() {
        try? submit("deny")
    }
}

public final class NotificationRequest: BDKUIRequest {
    public struct WebNotification: Sendable, Equatable {
        public let title: String
        public let body: String
        public let icon: URL?
        public let origin: URL
        public let tag: String?
        public let actions: [String]
        public let requireInteraction: Bool
    }

    public let notification: WebNotification
    /// Set by `shown()`. The ABI has no way to report it, so it stays in the application.
    public private(set) var isShown = false

    override init(_ delivery: BDKUIDelivery, runtime: BDKRuntime) {
        let fields = BDKUIPayload(delivery)
        let details = fields.object("notification") ?? fields
        notification = WebNotification(
            title: details.string("title") ?? "", body: details.string("body") ?? "",
            icon: details.url("icon"), origin: details.requiredURL("origin"),
            tag: details.string("tag"), actions: details.strings("actions"),
            requireInteraction: details.bool("requireInteraction"))
        super.init(delivery, runtime: runtime)
    }

    /// The application displayed it. This does not complete the request.
    public func shown() {
        isShown = true
    }

    public func clicked(action: String? = nil) {
        var arguments: [String: BDKJSValue] = ["event": .string("clicked")]
        if let action { arguments["action"] = .string(action) }
        try? submit("accept", arguments)
    }

    public func closed(byUser: Bool) {
        try? submit("deny", ["event": .string("closed"), "byUser": .bool(byUser)])
    }
}

/// What a certificate or reputation interstitial offers. Both refuse `proceed` where Chromium
/// refuses it, with `ui.notOverridable`.
public class BDKInterstitialRequest: BDKUIRequest {
    public let url: URL
    public let isOverridable: Bool
    public let text: BDKUIText

    override init(_ delivery: BDKUIDelivery, runtime: BDKRuntime) {
        let fields = BDKUIPayload(delivery)
        url = fields.requiredURL("url")
        isOverridable = fields.bool("isOverridable")
        text = BDKUIText(fields.object("text"))
        super.init(delivery, runtime: runtime)
    }

    /// `.reject` and a dropped request mean this as well.
    public func goBack() {
        try? submit("go_back")
    }

    public func proceed(confirmedBy event: NSEvent) throws {
        guard isOverridable else {
            throw BDKError(
                domain: .ui, code: "notOverridable",
                message: "Chromium does not allow proceeding past this warning")
        }
        try submit("proceed", gesture: event)
    }
}

public final class CertificateErrorRequest: BDKInterstitialRequest {
    /// "dateInvalid", "authorityInvalid", "commonNameInvalid", "revoked", "pinningFailure", ...
    public let error: String
    public let certificate: BDKCertificateSummary

    override init(_ delivery: BDKUIDelivery, runtime: BDKRuntime) {
        let fields = BDKUIPayload(delivery)
        error = fields.string("error") ?? ""
        certificate = BDKCertificateSummary(fields.object("certificate") ?? BDKUIPayload([:]))
        super.init(delivery, runtime: runtime)
    }
}

public final class ReputationWarningRequest: BDKInterstitialRequest {
    public enum Verdict: String, Sendable {
        case malware
        case phishing
        case unwantedSoftware
    }

    public let verdict: Verdict
    public let provider: BDKProviderDescriptor?

    override init(_ delivery: BDKUIDelivery, runtime: BDKRuntime) {
        let fields = BDKUIPayload(delivery)
        verdict = fields.choice("verdict", default: .malware)
        provider = BDKProviderDescriptor(fields)
        super.init(delivery, runtime: runtime)
    }
}

public final class ExternalProtocolRequest: BDKUIRequest {
    public let url: URL
    public let initiatingOrigin: URL?
    /// The application macOS would open.
    public let handlerName: String?
    public let isUserGesture: Bool

    override init(_ delivery: BDKUIDelivery, runtime: BDKRuntime) {
        let fields = BDKUIPayload(delivery)
        url = fields.requiredURL("url")
        initiatingOrigin = fields.url("initiatingOrigin")
        handlerName = fields.string("handlerName")
        isUserGesture = fields.bool("isUserGesture")
        super.init(delivery, runtime: runtime)
    }

    public func open(remember: Bool = false, confirmedBy event: NSEvent) throws {
        try submit("grant", ["remember": .bool(remember)], gesture: event)
    }

    public func cancel() {
        try? submit("deny")
    }
}

/// A surface with no typed request yet (translation, device choosers, screen capture, WebAuthn,
/// and any surface a newer runtime adds). It exposes the payload and the three completions of the
/// ABI.
public final class BDKRawUIRequest: BDKUIRequest {
    /// `completion` is "grant", "deny" or "accept". "grant" needs `event`.
    public func complete(
        _ completion: String, arguments: [String: BDKJSValue] = [:],
        confirmedBy event: NSEvent? = nil
    ) throws {
        try super.submit(completion, arguments, gesture: event)
    }
}
