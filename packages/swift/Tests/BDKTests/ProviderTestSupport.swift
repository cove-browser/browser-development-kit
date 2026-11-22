import BDKTestSupport
import CBDK
import Foundation
import Testing

@testable import BDK

/// A value a provider sets from another thread and a test reads.
final class ProviderFlag: @unchecked Sendable {
    private let lock = NSLock()
    private var stored = false

    var value: Bool {
        get { lock.withLock { stored } }
        set { lock.withLock { stored = newValue } }
    }
}

/// Translates by upper-casing, for French and German into English.
struct FakeTranslator: BDKTranslationProvider {
    var failure: (any Error)?
    var descriptor = BDKProviderDescriptor(id: "app.fake", name: "Fake", privacy: .onDevice)

    func supportedPairs() async throws -> [BDKLanguagePair] {
        [
            BDKLanguagePair(from: "fr", to: "en", installed: true),
            BDKLanguagePair(from: "de", to: "en", installed: true),
        ]
    }

    func translate(
        _ texts: [String], pair: BDKLanguagePair, context: BDKTranslationContext
    ) async throws -> [String] {
        if let failure { throw failure }
        return texts.map { $0.uppercased() }
    }
}

/// A translator with every optional operation: it reports progress, detects French and, for the
/// translation of "stuck", waits until it is cancelled.
struct FakeFullTranslator: BDKTranslationModelManaging, BDKLanguageDetecting {
    var cancelled: ProviderFlag
    var removed: ProviderFlag
    var descriptor = BDKProviderDescriptor(id: "app.full", name: "Full", privacy: .onDevice)

    func supportedPairs() async throws -> [BDKLanguagePair] {
        [BDKLanguagePair(from: "fr", to: "en", installed: false)]
    }

    func translate(
        _ texts: [String], pair: BDKLanguagePair, context: BDKTranslationContext
    ) async throws -> [String] {
        if texts == ["stuck"] {
            do {
                try await Task.sleep(for: .seconds(60))
            } catch {
                cancelled.value = true
                throw error
            }
        }
        return texts.map { $0.uppercased() }
    }

    func prepare(_ pair: BDKLanguagePair, progress: @Sendable (Double) -> Void) async throws {
        progress(0.25)
        progress(1)
    }

    func removeModel(_ pair: BDKLanguagePair) async throws {
        removed.value = true
    }

    func detectLanguage(of sample: String) async throws -> BDKLanguageGuess? {
        sample.contains("bonjour") ? BDKLanguageGuess(language: "fr", isReliable: true) : nil
    }
}

/// Answers `malware` for any address on a host that contains `risky`.
struct FakeReputation: BDKReputationProvider {
    var risky = "malware.test"
    var descriptor = BDKProviderDescriptor(
        id: "app.rep", name: "Rep", privacy: .remote, host: "rep.example.com",
        policyUrl: URL(string: "https://rep.example.com/privacy"), protection: "full")

    func check(_ url: URL, context: BDKReputationContext) async -> BDKVerdict {
        url.host()?.contains(risky) == true ? .malware : .safe
    }
}

/// Gives one position, then waits until the watch is stopped.
struct FakeGeolocation: BDKGeolocationProvider {
    var terminated: ProviderFlag
    var failure: BDKError?
    var descriptor = BDKProviderDescriptor(id: "app.geo", name: "Geo", privacy: .onDevice)

    func positions(_ options: BDKPositionOptions) -> AsyncThrowingStream<BDKPosition, Error> {
        let terminated = terminated
        let failure = failure
        return AsyncThrowingStream { continuation in
            if let failure {
                continuation.finish(throwing: failure)
                return
            }
            continuation.yield(
                BDKPosition(
                    latitude: 51.5, longitude: -0.12, accuracy: options.highAccuracy ? 5 : 50))
            continuation.onTermination = { _ in terminated.value = true }
        }
    }
}

/// The resolved configuration with these `services` members, and the providers `body` registers.
func providerConfiguration(
    services: String = "", registering body: (inout BDKConfiguration) throws -> Void = { _ in }
) rethrows -> BDKConfiguration {
    var configuration = BDKConfiguration(
        resolvedJSON: services.isEmpty ? "{}" : "{\"services\":{\(services)}}")
    try body(&configuration)
    return configuration
}

let appTranslation = "\"translation\":{\"provider\":\"app\"}"

/// Pumps the stub until `condition` holds; providers answer from tasks, not from the pump.
@MainActor
func settle(
    _ runtime: BDKRuntime, sourceLocation: SourceLocation = #_sourceLocation,
    until condition: () throws -> Bool
) async throws {
    for _ in 0..<300 {
        runtime.stub.pump()
        if try condition() { return }
        try await Task.sleep(for: .milliseconds(10))
    }
    Issue.record("the condition never held", sourceLocation: sourceLocation)
}
