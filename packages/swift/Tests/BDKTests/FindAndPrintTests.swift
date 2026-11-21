import BDKTestSupport
import CBDK
import Foundation
import Testing

@testable import BDK

extension BDKStubTests {
    /// `page.find` and `page.print()` (T-SWF-07). The stub renders nothing, so every search finds no
    /// match; a real engine's counts are covered by the engine scenarios.
    @MainActor
    @Suite("Find and print")
    struct FindAndPrintTests {
        @Test func aSearchReportsItsResultsUntilStopped() async throws {
            try await withProfile { runtime, profile in
                let page = try profile.webContents.create()
                let results = try page.find.start("needle", options: .init(matchCase: true))
                var iterator = results.makeAsyncIterator()
                runtime.stub.pump()
                let first = await iterator.next()
                #expect(first == .init(matchCount: 0, activeIndex: 0, rect: .zero, isFinal: true))

                try page.find.next()
                runtime.stub.pump()
                #expect(await iterator.next()?.matchCount == 0)
                try page.find.previous()
                runtime.stub.pump()
                #expect(await iterator.next()?.isFinal == true)

                try page.find.stop(.keepSelection)
                #expect(await iterator.next() == nil)
            }
        }

        @Test func startingAgainEndsThePreviousSequence() async throws {
            try await withProfile { runtime, profile in
                let page = try profile.webContents.create()
                let first = try page.find.start("a")
                let second = try page.find.start("b")
                runtime.stub.pump()
                var firstIterator = first.makeAsyncIterator()
                var secondIterator = second.makeAsyncIterator()
                // The first sequence ended when the second began; the second sees its own result.
                #expect(await firstIterator.next() == nil)
                #expect(await secondIterator.next() != nil)
                try page.find.stop()
            }
        }

        @Test func aClosedPageThrowsStale() async throws {
            try await withProfile { _, profile in
                let page = try profile.webContents.create()
                try await page.close()
                await expectError(.contents, "stale") { _ = try page.find.start("x") }
                await expectError(.contents, "stale") { try page.find.next() }
                await expectError(.contents, "stale") { try page.find.previous() }
                await expectError(.contents, "stale") { try page.find.stop() }
                await expectError(.contents, "stale") { try page.find.present() }
                await expectError(.contents, "stale") { try page.print() }
            }
        }

        @Test func presentRaisesAFindRequestWithTheLastSearchAsItsText() async throws {
            try await withUIFixture { fixture in
                let page = fixture.page
                _ = try page.find.start("needle")
                fixture.runtime.stub.pump()
                fixture.box.request = nil
                try page.find.present()
                fixture.runtime.stub.pump()
                let request = try #require(fixture.box.request as? FindRequest)
                #expect(request.initialText == "needle")
                #expect(request.session === page.find)
                #expect(fixture.box.count == 1)
                // One find UI is open per page: asking again raises nothing.
                try page.find.present()
                fixture.runtime.stub.pump()
                #expect(fixture.box.count == 1)
                request.close()
                fixture.runtime.stub.pump()
                try page.find.present()
                fixture.runtime.stub.pump()
                #expect(fixture.box.count == 2)
            }
        }

        @Test func printRaisesAUserInitiatedPrintRequest() async throws {
            try await withUIFixture { fixture in
                fixture.box.request = nil
                try fixture.page.print()
                fixture.runtime.stub.pump()
                let request = try #require(fixture.box.request as? PrintRequest)
                #expect(request.isUserInitiated)
                #expect(request.contents === fixture.page)
                request.cancel()
            }
        }
    }
}
