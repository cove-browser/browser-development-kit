import Foundation

/// A provider the application registered, as the runtime needs it: the descriptor and the
/// operations it implements, already bridged to JSON.
struct BDKProviderRegistration: Sendable {
    let descriptor: BDKProviderDescriptor
    let operations: [BDKProviderSlot: BDKProviderBridge.Operation]
}

/// Provider keys and implementations (swift-api.md section 11.2): `config.services`. It is a value,
/// collected before start. Registering a second provider for a service replaces the first.
///
/// `setKey` is not built: no provider in this runtime takes a key that way (`bdk_provider_set_key`
/// always fails `config.keyUnused`), and the Google key is `google.apiKey`.
public struct BDKServiceRegistry: Sendable {
    public struct Google: Sendable {
        /// The Google API key, for a service whose `provider` is `google` in `bdk.toml`. The
        /// runtime refuses a key when no service uses Google (`config.keyUnused`) and a Google
        /// service without a key (`config.keyMissing`).
        public var apiKey: String?
    }

    public struct Translation: Sendable {
        var registration: BDKProviderRegistration?

        /// The provider of `translation` when `provider = "app"`. Conforming to
        /// `BDKTranslationModelManaging` adds model download and removal, and to
        /// `BDKLanguageDetecting` language detection.
        public mutating func register(_ provider: some BDKTranslationProvider) {
            registration = BDKProviderRegistration(
                descriptor: provider.descriptor, operations: provider.operations)
        }
    }

    public struct Reputation: Sendable {
        var registration: BDKProviderRegistration?

        /// The provider of `reputation` when `provider = "app"`. Conforming to
        /// `BDKDownloadChecking` adds download checks, and to `BDKReputationUpdating` updates.
        public mutating func register(_ provider: some BDKReputationProvider) {
            registration = BDKProviderRegistration(
                descriptor: provider.descriptor, operations: provider.operations)
        }
    }

    public struct Geolocation: Sendable {
        var registration: BDKProviderRegistration?

        /// The provider of `geolocation` when `provider = "app"`.
        public mutating func register(_ provider: some BDKGeolocationProvider) {
            registration = BDKProviderRegistration(
                descriptor: provider.descriptor, operations: provider.operations)
        }
    }

    public var google = Google()
    public var translation = Translation()
    public var reputation = Reputation()
    public var geolocation = Geolocation()

    public init() {}

    /// Registers each provider with the runtime, before the freeze. The runtime refuses one for a
    /// service that is not `app` (`config.providerNotApp`) and a descriptor it cannot accept
    /// (`config.descriptorInvalid`); the freeze fails for an `app` service with none
    /// (`config.providerMissing`).
    @MainActor
    func adopt(into runtime: BDKRuntime) throws {
        let registrations = [
            ("translation", translation.registration), ("reputation", reputation.registration),
            ("geolocation", geolocation.registration),
        ]
        for case (let service, let registration?) in registrations {
            try BDKProviderBridge.register(
                registration, service: service, with: try runtime.liveHandle())
        }
    }
}

// MARK: - Wire types

private struct PairInput: Decodable {
    let from: String
    let to: String

    func pair(installed: Bool) -> BDKLanguagePair {
        BDKLanguagePair(from: from, to: to, installed: installed)
    }
}

private struct TranslateInput: Decodable {
    let from: String
    let to: String
    let texts: [String]
    let url: String?
}

private struct PairsOutput: Encodable {
    let pairs: [BDKLanguagePair]
}

private struct TextsOutput: Encodable {
    let texts: [String]
}

private struct DetectInput: Decodable {
    let sample: String
}

private struct DetectOutput: Encodable {
    let language: String
    let reliable: Bool
}

private struct CheckURLInput: Decodable {
    let url: String
    let kind: String
}

private struct CheckDownloadInput: Decodable {
    let urls: [String]
    let fileName: String?
}

private struct VerdictOutput: Encodable {
    let verdict: String
}

private struct StartInput: Decodable {
    let highAccuracy: Bool?
}

private struct PositionFailure: Encodable {
    let error: String
    let message: String
}

private let null = "null"

// MARK: - Operations

extension BDKTranslationProvider {
    fileprivate var operations: [BDKProviderSlot: BDKProviderBridge.Operation] {
        var operations: [BDKProviderSlot: BDKProviderBridge.Operation] = [
            .supportedPairs: { _, _ in
                try BDKJSON.encode(PairsOutput(pairs: try await self.supportedPairs()))
            },
            .translate: { _, input in
                let request = try BDKJSON.decode(TranslateInput.self, from: input)
                let texts = try await self.translate(
                    request.texts,
                    pair: BDKLanguagePair(from: request.from, to: request.to, installed: true),
                    context: BDKTranslationContext(url: request.url.flatMap { URL(string: $0) }))
                return try BDKJSON.encode(TextsOutput(texts: texts))
            },
        ]
        if let manager = self as? any BDKTranslationModelManaging {
            operations[.prepare] = { call, input in
                let pair = try BDKJSON.decode(PairInput.self, from: input).pair(installed: false)
                try await manager.prepare(pair) { fraction in
                    Task { @MainActor in call.progress(fraction) }
                }
                return null
            }
            operations[.removeModel] = { _, input in
                try await manager.removeModel(
                    try BDKJSON.decode(PairInput.self, from: input).pair(installed: true))
                return null
            }
        }
        if let detector = self as? any BDKLanguageDetecting {
            operations[.detect] = { _, input in
                let sample = try BDKJSON.decode(DetectInput.self, from: input).sample
                guard let guess = try await detector.detectLanguage(of: sample) else { return null }
                return try BDKJSON.encode(
                    DetectOutput(language: guess.language, reliable: guess.isReliable))
            }
        }
        return operations
    }
}

extension BDKReputationProvider {
    fileprivate var operations: [BDKProviderSlot: BDKProviderBridge.Operation] {
        var operations: [BDKProviderSlot: BDKProviderBridge.Operation] = [
            .checkURL: { _, input in
                let request = try BDKJSON.decode(CheckURLInput.self, from: input)
                // An address this version cannot read is not one to warn about.
                guard let url = URL(string: request.url) else {
                    return try BDKJSON.encode(VerdictOutput(verdict: BDKVerdict.unknown.rawValue))
                }
                let kind = BDKReputationContext.Kind(rawValue: request.kind) ?? .navigation
                let verdict = await self.check(url, context: BDKReputationContext(kind: kind))
                return try BDKJSON.encode(VerdictOutput(verdict: verdict.rawValue))
            }
        ]
        if let checker = self as? any BDKDownloadChecking {
            operations[.checkDownload] = { _, input in
                let request = try BDKJSON.decode(CheckDownloadInput.self, from: input)
                let candidate = BDKDownloadCandidate(
                    urls: request.urls.compactMap { URL(string: $0) },
                    fileName: request.fileName ?? "")
                return try BDKJSON.encode(
                    VerdictOutput(verdict: await checker.check(candidate).rawValue))
            }
        }
        if let updater = self as? any BDKReputationUpdating {
            operations[.update] = { _, _ in
                try await updater.update()
                return null
            }
        }
        return operations
    }
}

extension BDKGeolocationProvider {
    /// The errors a position stream may end a watch with; any other error fails the call.
    private static var positionErrors: Set<String> {
        ["permissionDenied", "positionUnavailable", "timeout"]
    }

    fileprivate var operations: [BDKProviderSlot: BDKProviderBridge.Operation] {
        [
            .start: { call, input in
                let options = BDKPositionOptions(
                    highAccuracy: try BDKJSON.decode(StartInput.self, from: input).highAccuracy
                        ?? false)
                do {
                    for try await position in self.positions(options) {
                        if Task.isCancelled { break }
                        call.report(try BDKServiceCoding.encode(position))
                    }
                } catch let error as BDKError where Self.positionErrors.contains(error.code) {
                    call.report(
                        try BDKServiceCoding.encode(
                            PositionFailure(error: error.code, message: error.message)))
                }
                return null
            }
        ]
    }
}
