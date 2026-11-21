import BDKTestSupport
import Foundation
import Testing

@testable import BDK

/// Starts the stub runtime, runs `body` and always shuts the runtime down.
@MainActor
func withStubRuntime(
    _ configuration: BDKConfiguration = .stub, freezingRegistrations: Bool = true,
    _ body: (BDKRuntime) async throws -> Void
) async throws {
    let runtime = try BDKRuntime.startStub(
        configuration, freezingRegistrations: freezingRegistrations)
    do {
        try await body(runtime)
    } catch {
        await runtime.shutdown()
        throw error
    }
    await runtime.shutdown()
}

/// A runtime with one open profile.
@MainActor
func withProfile(_ body: (BDKRuntime, BDKProfile) async throws -> Void) async throws {
    try await withStubRuntime { runtime in
        let profile = try await runtime.profiles.open("Test")
        try await body(runtime, profile)
    }
}

struct EventsEnded: Error {}

/// Reads one event sequence. Creating it subscribes, so create it before the action under test.
/// The suites carry a time limit, so a missing event fails the run instead of hanging it.
@MainActor
final class EventReader<Element: Sendable> {
    private nonisolated(unsafe) var iterator: AsyncStream<Element>.Iterator

    init<Sequence: AsyncSequence>(_ sequence: Sequence)
    where Sequence.Element == Element, Sequence.AsyncIterator == AsyncStream<Element>.Iterator {
        iterator = sequence.makeAsyncIterator()
    }

    /// The next event, or nil when the sequence ended.
    nonisolated func next() async -> Element? {
        await iterator.next()
    }

    /// Reads events until one matches.
    func next(where matches: (Element) -> Bool) async throws -> Element {
        while let element = await next() {
            if matches(element) { return element }
        }
        throw EventsEnded()
    }

    /// Reads events up to and including the first one `last` accepts, or until the sequence ends.
    func drain(until last: (Element) -> Bool) async -> [Element] {
        var seen: [Element] = []
        while let element = await next() {
            seen.append(element)
            if last(element) { break }
        }
        return seen
    }
}

@MainActor
func expectError(
    _ domain: BDKError.Domain, _ code: String, sourceLocation: SourceLocation = #_sourceLocation,
    _ body: () async throws -> Void
) async {
    do {
        try await body()
        Issue.record("expected \(domain.rawValue).\(code)", sourceLocation: sourceLocation)
    } catch let error as BDKError {
        #expect(
            error.domain == domain && error.code == code, "\(error)", sourceLocation: sourceLocation
        )
    } catch {
        Issue.record("unexpected error \(error)", sourceLocation: sourceLocation)
    }
}

/// Every test shares the process-wide runtime, so every suite is nested here and runs one at a time.
@Suite("BDK against the stub", .serialized, .timeLimit(.minutes(1)))
struct BDKStubTests {}
