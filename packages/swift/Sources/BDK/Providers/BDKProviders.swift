import Foundation

// The provider protocols of swift-api.md section 11.2. A provider answers one service when
// `provider = "app"` in `bdk.toml`; BDK checks every answer against the contract in
// packages/core/docs/conventions.md ("Providers"), so a provider that breaks it fails the call with
// `provider.contractViolation`. Operations a provider may leave out are refining protocols: BDK
// offers an operation only when the provider conforms.

/// What a translation request carries besides the text.
public struct BDKTranslationContext: Sendable, Equatable {
    /// The page the text came from, when it has an address.
    public let url: URL?

    public init(url: URL? = nil) {
        self.url = url
    }
}

/// The language of a sample of text, as a provider guessed it.
public struct BDKLanguageGuess: Sendable, Equatable {
    public let language: String
    public let isReliable: Bool

    public init(language: String, isReliable: Bool) {
        self.language = language
        self.isReliable = isReliable
    }
}

public protocol BDKTranslationProvider: Sendable {
    var descriptor: BDKProviderDescriptor { get }
    /// The pairs the provider translates. `installed` is false for a pair whose model must be
    /// prepared first (`BDKTranslationModelManaging`).
    func supportedPairs() async throws -> [BDKLanguagePair]
    /// Translates `texts` and returns as many strings, in the same order.
    func translate(
        _ texts: [String], pair: BDKLanguagePair, context: BDKTranslationContext
    ) async throws -> [String]
}

/// A provider with models to download and remove.
public protocol BDKTranslationModelManaging: BDKTranslationProvider {
    /// Downloads the model of `pair`. `progress` takes a fraction from 0 to 1.
    func prepare(_ pair: BDKLanguagePair, progress: @Sendable (Double) -> Void) async throws
    func removeModel(_ pair: BDKLanguagePair) async throws
}

/// A provider that detects the language of a page's text. Without one, the engine's detector runs.
public protocol BDKLanguageDetecting: BDKTranslationProvider {
    /// The language of `sample`, or nil when it cannot tell.
    func detectLanguage(of sample: String) async throws -> BDKLanguageGuess?
}

public enum BDKVerdict: String, Sendable, Equatable, CaseIterable {
    case safe
    case unknown
    case malware
    case phishing
    case unwantedSoftware
    /// A download only.
    case dangerousDownload
}

/// What a checked address is for.
public struct BDKReputationContext: Sendable, Equatable {
    public enum Kind: String, Sendable, Equatable {
        case navigation
        case subresource
    }

    public let kind: Kind

    public init(kind: Kind) {
        self.kind = kind
    }
}

/// A download about to start.
public struct BDKDownloadCandidate: Sendable, Equatable {
    /// The address and the redirects that led to it.
    public let urls: [URL]
    public let fileName: String

    public init(urls: [URL], fileName: String) {
        self.urls = urls
        self.fileName = fileName
    }
}

public protocol BDKReputationProvider: Sendable {
    var descriptor: BDKProviderDescriptor { get }
    /// Navigations get `safe`, `unknown`, `malware`, `phishing` or `unwantedSoftware`. The
    /// provider reports; BDK shows the interstitial.
    func check(_ url: URL, context: BDKReputationContext) async -> BDKVerdict
}

public protocol BDKDownloadChecking: BDKReputationProvider {
    /// Downloads get `safe`, `unknown`, `malware`, `unwantedSoftware` or `dangerousDownload`.
    func check(_ download: BDKDownloadCandidate) async -> BDKVerdict
}

/// A list-based provider that refreshes its list when BDK asks.
public protocol BDKReputationUpdating: BDKReputationProvider {
    func update() async throws
}

/// A position of the device.
public struct BDKPosition: Sendable, Equatable {
    public var latitude: Double
    public var longitude: Double
    /// In metres.
    public var accuracy: Double
    public var altitude: Double?
    public var altitudeAccuracy: Double?
    /// Degrees clockwise from true north.
    public var heading: Double?
    /// In metres per second.
    public var speed: Double?
    public var timestamp: Date

    public init(
        latitude: Double, longitude: Double, accuracy: Double, altitude: Double? = nil,
        altitudeAccuracy: Double? = nil, heading: Double? = nil, speed: Double? = nil,
        timestamp: Date = Date()
    ) {
        self.latitude = latitude
        self.longitude = longitude
        self.accuracy = accuracy
        self.altitude = altitude
        self.altitudeAccuracy = altitudeAccuracy
        self.heading = heading
        self.speed = speed
        self.timestamp = timestamp
    }
}

/// How a position goes to the runtime: `timestamp` in milliseconds, by `BDKServiceCoding`.
extension BDKPosition: Encodable {}

public struct BDKPositionOptions: Sendable, Equatable {
    public var highAccuracy: Bool

    public init(highAccuracy: Bool = false) {
        self.highAccuracy = highAccuracy
    }
}

public protocol BDKGeolocationProvider: Sendable {
    var descriptor: BDKProviderDescriptor { get }
    /// The positions of one watch, until BDK stops it. BDK ends the iteration when the page clears
    /// its watch, so the stream's `onTermination` is where to stop the hardware. Finishing the
    /// stream ends the watch. Throw a `BDKError` with the code `permissionDenied`,
    /// `positionUnavailable` or `timeout` to tell the page why there is no position.
    func positions(_ options: BDKPositionOptions) -> AsyncThrowingStream<BDKPosition, Error>
}
