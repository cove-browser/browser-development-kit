import CoreGraphics
import Foundation

/// Reads the payload of a request. The runtime's payload schema is provisional (ui-requests.md
/// lists the fields, not their encoding), so every accessor has a default: a missing or malformed
/// field never prevents a handler from seeing the request.
struct BDKUIPayload {
    let fields: [String: BDKJSValue]

    init(_ fields: [String: BDKJSValue]) {
        self.fields = fields
    }

    init(_ delivery: BDKUIDelivery) {
        fields = BDKJSValue(jsonString: delivery.payloadJSON)?.objectValue ?? [:]
    }

    func string(_ key: String) -> String? {
        fields[key]?.stringValue
    }

    func bool(_ key: String, default value: Bool = false) -> Bool {
        fields[key]?.boolValue ?? value
    }

    func double(_ key: String) -> Double? {
        fields[key]?.doubleValue
    }

    func int(_ key: String) -> Int? {
        double(key).map { Int($0) }
    }

    func url(_ key: String) -> URL? {
        string(key).flatMap { URL(string: $0) }
    }

    /// A URL the request cannot do without; `about:blank` when the runtime sent none.
    func requiredURL(_ key: String) -> URL {
        url(key) ?? URL(string: "about:blank") ?? URL(fileURLWithPath: "/")
    }

    func strings(_ key: String) -> [String] {
        fields[key]?.arrayValue?.compactMap(\.stringValue) ?? []
    }

    func objects(_ key: String) -> [BDKUIPayload] {
        fields[key]?.arrayValue?.compactMap { $0.objectValue.map(BDKUIPayload.init) } ?? []
    }

    func object(_ key: String) -> BDKUIPayload? {
        fields[key]?.objectValue.map(BDKUIPayload.init)
    }

    func point(_ key: String) -> CGPoint {
        guard let object = object(key) else { return .zero }
        return CGPoint(x: object.double("x") ?? 0, y: object.double("y") ?? 0)
    }

    func rect(_ key: String) -> CGRect {
        guard let object = object(key) else { return .zero }
        return CGRect(
            x: object.double("x") ?? 0, y: object.double("y") ?? 0,
            width: object.double("width") ?? 0, height: object.double("height") ?? 0)
    }

    /// A handle the runtime put in the payload (a page created for the request). Handles exceed
    /// 2^53, so the payload carries them as decimal strings; a JSON number is accepted too.
    func handle(_ key: String) -> UInt64? {
        BDKUIPayload.handle(fields[key])
    }

    static func handle(_ value: BDKJSValue?) -> UInt64? {
        switch value {
        case .string(let text): UInt64(text)
        case .number(let number): UInt64(exactly: number)
        default: nil
        }
    }

    /// An enum encoded as its case name; `fallback` when the name is unknown.
    func choice<Value: RawRepresentable>(_ key: String, default fallback: Value) -> Value
    where Value.RawValue == String {
        string(key).flatMap(Value.init(rawValue:)) ?? fallback
    }
}

/// A title and a message the runtime wrote for a presenter (permission text, warnings,
/// interstitials). Custom presenters show the same facts the default one does.
public struct BDKUIText: Sendable, Equatable {
    public let title: String
    public let message: String

    public init(title: String = "", message: String = "") {
        self.title = title
        self.message = message
    }

    init(_ payload: BDKUIPayload?, fallbackTitle: String? = nil) {
        title = payload?.string("title") ?? fallbackTitle ?? ""
        message = payload?.string("message") ?? ""
    }
}

/// The provider behind a request, as the request's payload names it: the descriptor of
/// `translation.provider` (swift-api.md 11.2). A payload from an older runtime that names only the
/// provider reads as an on-device provider of that name.
extension BDKProviderDescriptor {
    /// Whether page content or a URL leaves the device; default UI says so.
    public var isRemote: Bool { privacy == .remote }

    init?(_ payload: BDKUIPayload, key: String = "provider") {
        if let object = payload.object(key) {
            let name = object.string("name") ?? ""
            let remote =
                object.string("privacy") == BDKProviderPrivacy.remote.rawValue
                || object.bool("isRemote")
            self.init(
                id: object.string("id") ?? name, name: name,
                privacy: remote ? .remote : .onDevice, host: object.string("host"),
                policyUrl: object.string("policyUrl").flatMap(URL.init(string:)),
                protection: object.string("protection"))
        } else if let text = payload.string(key) {
            self.init(id: text, name: text, privacy: .onDevice)
        } else {
            return nil
        }
    }
}

/// A certificate as a chooser or an interstitial shows it.
public struct BDKCertificateSummary: Sendable, Equatable, Identifiable {
    public let id: String
    public let subject: String
    public let issuer: String

    init(_ payload: BDKUIPayload) {
        id = payload.string("id") ?? ""
        subject = payload.string("subject") ?? ""
        issuer = payload.string("issuer") ?? ""
    }
}
