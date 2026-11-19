import BDK
import BDKTestSupport
import Foundation
import MinimalBrowserKit
import Testing

/// All suites that start the stub runtime are nested here, because the runtime is process-wide.
@Suite("Session", .serialized, .timeLimit(.minutes(1)))
@MainActor
struct SessionTests {
    private func withDirectory(_ body: (URL) async throws -> Void) async throws {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("minimal-browser-\(UUID().uuidString)", isDirectory: true)
        defer { try? FileManager.default.removeItem(at: directory) }
        try await body(directory)
    }

    @Test("a saved session loads back as it was saved")
    func roundTrip() async throws {
        try await withDirectory { directory in
            let store = SessionStore(directory: directory)
            #expect(try store.load() == nil)
            let snapshot = SessionStore.Snapshot(pages: [
                .init(data: Data("one".utf8)), .init(data: Data([0, 255, 7])),
            ])
            try store.save(snapshot)
            #expect(try store.load() == snapshot)
            // A second save replaces the first.
            try store.save(.init(pages: []))
            #expect(try store.load()?.pages == [])
        }
    }

    @Test("a damaged file and a newer version are reported, not trusted")
    func unreadable() async throws {
        try await withDirectory { directory in
            let store = SessionStore(directory: directory)
            try FileManager.default.createDirectory(
                at: directory, withIntermediateDirectories: true)
            try Data("not json".utf8).write(to: store.fileURL)
            #expect(throws: SessionStore.Failure.self) { try store.load() }

            try Data(#"{"version":99,"pages":[]}"#.utf8).write(to: store.fileURL)
            #expect(throws: SessionStore.Failure.unsupportedVersion(99)) { try store.load() }
        }
    }

    @Test("the state of a stub page, saved and restored into a new page, gives back its entries")
    func restoresPageEntries() async throws {
        try await withDirectory { directory in
            let runtime = try BDKRuntime.startStub()
            let first = URL(string: "https://example.com/")!
            let second = URL(string: "https://example.org/")!
            do {
                let profile = try await runtime.profiles.open("Test")
                let page = try profile.webContents.create()
                try await page.navigate(to: first)
                try await page.navigate(to: second)
                let store = SessionStore(directory: directory)
                try store.save(.init(pages: [try page.sessionState()]))

                let saved = try #require(try store.load()?.pages.first)
                let restored = try profile.webContents.create(.init(restoring: saved))
                #expect(try restored.navigation.entries.map(\.url) == [first, second])
                #expect(try restored.navigation.currentIndex == 1)
                #expect(restored.url == second)
            } catch {
                await runtime.shutdown()
                throw error
            }
            await runtime.shutdown()
        }
    }
}
