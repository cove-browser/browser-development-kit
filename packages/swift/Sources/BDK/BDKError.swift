import CBDK

/// The one error type every throwing call throws (swift-api.md section 16). Domains and codes are
/// strings so that an older binding can report an error from a newer runtime.
public struct BDKError: Error, Sendable, Equatable, CustomStringConvertible {
    public struct Domain: RawRepresentable, Hashable, Sendable {
        public let rawValue: String

        public init(rawValue: String) {
            self.rawValue = rawValue
        }

        public static let abi = Domain(rawValue: "abi")
        public static let runtime = Domain(rawValue: "runtime")
        public static let config = Domain(rawValue: "config")
        public static let profile = Domain(rawValue: "profile")
        public static let contents = Domain(rawValue: "contents")
        public static let navigation = Domain(rawValue: "navigation")
        public static let broker = Domain(rawValue: "broker")
        public static let capability = Domain(rawValue: "capability")
        public static let ui = Domain(rawValue: "ui")
        public static let provider = Domain(rawValue: "provider")
        public static let automation = Domain(rawValue: "automation")
        public static let platform = Domain(rawValue: "platform")
    }

    public let domain: Domain
    /// Stable, lower camel case.
    public let code: String
    /// For developers; not localised.
    public let message: String
    public let details: [String: BDKJSValue]

    public init(
        domain: Domain, code: String, message: String, details: [String: BDKJSValue] = [:]
    ) {
        self.domain = domain
        self.code = code
        self.message = message
        self.details = details
    }

    public var description: String {
        "\(domain.rawValue).\(code): \(message)"
    }
}

extension BDKError {
    /// What a stale handle was a handle to. It picks the domain of the `stale` error.
    enum Subject {
        case runtime
        case profile
        case contents
        case request

        var domain: Domain {
            switch self {
            case .runtime: .runtime
            case .profile: .profile
            case .contents: .contents
            case .request: .ui
            }
        }

        var noun: String {
            switch self {
            case .runtime: "runtime"
            case .profile: "profile"
            case .contents: "contents"
            case .request: "request"
            }
        }
    }

    static let staleCode = "stale"

    static func stale(_ subject: Subject) -> BDKError {
        BDKError(
            domain: subject.domain, code: staleCode,
            message: "the \(subject.noun) was destroyed or its runtime shut down")
    }

    var isStale: Bool {
        code == Self.staleCode
    }

    /// Reads an error handle. The handle is not released here: a result callback only borrows it.
    init(handle: bdk_error_t) {
        self.init(
            domain: Domain(rawValue: swiftString(bdk_error_domain(handle))),
            code: swiftString(bdk_error_code(handle)),
            message: swiftString(bdk_error_message(handle)),
            details: {
                guard
                    case .object(let object) = BDKJSValue(
                        jsonString: swiftString(bdk_error_details_json(handle)))
                else { return [:] }
                return object
            }())
    }

    /// Maps the status of a synchronous call. `error` is the out-parameter, which this releases.
    static func check(_ status: bdk_status_t, error: bdk_error_t, subject: Subject) throws {
        defer {
            if error != 0 { bdk_error_release(error) }
        }
        switch status {
        case BDK_OK:
            return
        case BDK_ERR_STALE_HANDLE:
            throw stale(subject)
        case BDK_ERR_NOT_STARTED:
            throw BDKError(
                domain: .runtime, code: "notStarted",
                message: "the registrations are not frozen yet; contents cannot be created")
        case BDK_ERR_WRONG_THREAD:
            throw BDKError(
                domain: .runtime, code: "wrongThread",
                message: "the call was not made on the UI thread")
        case BDK_ERR_FAILED where error != 0:
            throw BDKError(handle: error)
        case BDK_ERR_FAILED:
            throw BDKError(domain: .runtime, code: "failed", message: "the call failed")
        default:
            throw BDKError(
                domain: .broker, code: "invalidArguments",
                message: "the runtime rejected an argument")
        }
    }
}
