import Foundation

// The translation surface (ui-requests.md 5.10).

/// An offer to translate the page, or the page's translation as the application asked to see it
/// (`page.translation.present()`). The request is the offer, not one that moves through the
/// phases: later phases are `BDKWebContents.Event.translation`. None of its completions grants
/// anything, so none needs a gesture.
public final class TranslationRequest: BDKUIRequest {
    public enum Phase: Sendable, Equatable {
        case offer
        case translating(progress: Double?)
        case translated
        case failed(BDKError)
    }

    public let phase: Phase
    /// The page's language, when it is known.
    public let detected: String?
    /// The language the user reads.
    public let target: String
    /// What the provider supports.
    public let languages: [BDKLanguagePair]
    /// Name and privacy model, to show the user. A remote provider receives the page's text.
    public let provider: BDKProviderDescriptor?

    override init(_ delivery: BDKUIDelivery, runtime: BDKRuntime) {
        let fields = BDKUIPayload(delivery)
        switch fields.string("phase") {
        case "translating": phase = .translating(progress: fields.double("progress"))
        case "translated": phase = .translated
        case "failed": phase = .failed(Self.error(fields.object("error")))
        default: phase = .offer
        }
        detected = fields.string("detected")
        target = fields.string("target") ?? ""
        languages = fields.objects("languages").map {
            BDKLanguagePair(
                from: $0.string("from") ?? "", to: $0.string("to") ?? "",
                installed: $0.bool("installed", default: true))
        }
        provider = BDKProviderDescriptor(fields)
        super.init(delivery, runtime: runtime)
    }

    private static func error(_ payload: BDKUIPayload?) -> BDKError {
        BDKError(
            domain: BDKError.Domain(rawValue: payload?.string("domain") ?? "provider"),
            code: payload?.string("code") ?? "remoteError",
            message: payload?.string("message") ?? "the translation failed")
    }

    /// Translates the page. A language left out is the detected one, or the target.
    public func translate(from: String? = nil, to: String? = nil) {
        var arguments: [String: BDKJSValue] = [:]
        if let from { arguments["from"] = .string(from) }
        if let to { arguments["to"] = .string(to) }
        try? submit("translate", arguments)
    }

    /// Shows the original text again.
    public func revert() {
        try? submit("revert")
    }

    /// Translates pages in `language` without asking.
    public func always(for language: String) {
        try? submit("always", ["language": .string(language)])
    }

    /// Never offers to translate pages in `language`.
    public func never(for language: String) {
        try? submit("never", ["language": .string(language)])
    }

    public func neverForThisSite() {
        try? submit("neverForSite")
    }

    public func dismiss() {
        try? submit("dismiss")
    }
}
