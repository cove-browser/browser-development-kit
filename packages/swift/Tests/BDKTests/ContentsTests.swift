import BDKTestSupport
import CBDK
import Foundation
import Testing

@testable import BDK

private let example = URL(string: "https://example.com/")!
private let other = URL(string: "https://example.org/a")!

extension BDKStubTests {
    @MainActor
    @Suite("WebContents")
    struct ContentsTests {
        @Test func newPageIsBlankAndAlive() async throws {
            try await withProfile { runtime, profile in
                let page = try profile.webContents.create()
                #expect(page.isAlive && !page.isClosed)
                #expect(page.profile === profile)
                #expect(runtime.webContents.map(\.id) == [page.id])
                #expect(profile.webContents.all.map(\.id) == [page.id])
                let state = try page.state()
                #expect(state.url?.absoluteString == "about:blank")
                #expect(state.title == "")
                #expect(!state.isLoading && !state.isMuted && !state.isFocused)
                #expect(state.zoom == 1.0)
                #expect(try page.navigation.entries.isEmpty)
                #expect(try page.navigation.currentIndex == -1)
            }
        }

        @Test func initialURLLoads() async throws {
            try await withProfile { runtime, profile in
                try runtime.stub.scriptNavigation(to: example, title: "Example")
                let page = try profile.webContents.create(.init(initialURL: example))
                let events = EventReader(page.events)
                _ = try await events.next {
                    if case .navigationFinished = $0 { true } else { false }
                }
                #expect(page.url == example)
                #expect(page.title == "Example")
                #expect(page.security == .secure)
                #expect(!page.isLoading && page.estimatedProgress == 1.0)
            }
        }

        @Test func loadReturnsTheCommit() async throws {
            try await withProfile { runtime, profile in
                try runtime.stub.scriptNavigation(to: example, title: "Example")
                let page = try profile.webContents.create()
                let commit = try await page.navigate(to: example)
                #expect(commit.url == example)
                #expect(commit.isMainFrame && commit.isUserInitiated && !commit.isErrorPage)
                #expect(page.url == example)
            }
        }

        @Test func failedLoadThrowsTheNetError() async throws {
            try await withProfile { runtime, profile in
                try runtime.stub.scriptNavigation(to: example, netError: -105)
                let page = try profile.webContents.create()
                do {
                    try await page.navigate(to: example)
                    Issue.record("load should fail")
                } catch let error as BDKError {
                    #expect(error.domain == .navigation && error.code == "netError")
                    #expect(error.details["netError"] == .number(-105))
                }
                // The error page commits as an entry for the URL that failed, as in Chromium.
                #expect(try page.navigation.entries.map(\.url) == [example])
            }
        }

        @Test func historyNavigation() async throws {
            try await withProfile { _, profile in
                let page = try profile.webContents.create()
                try await page.navigate(to: example)
                try await page.navigate(to: other)
                let third = URL(string: "https://example.net/")!
                try await page.navigate(to: third)
                #expect(try page.navigation.entries.map(\.url) == [example, other, third])
                #expect(try page.navigation.currentIndex == 2)
                #expect(try page.navigation.canGoBack)
                #expect(try !page.navigation.canGoForward)
                try await settle(page) { try page.navigation.goBack() }
                #expect(page.url == other)
                #expect(try page.navigation.canGoForward)
                try await settle(page) { try page.navigation.go(toIndex: 0) }
                #expect(page.url == example)
                try await settle(page) { try page.navigation.goForward() }
                #expect(page.url == other)
                try await settle(page) { try page.navigation.reload() }
                #expect(page.url == other)
                await expectError(.broker, "invalidArguments") {
                    try page.navigation.go(toIndex: 9)
                }
                await expectError(.broker, "invalidArguments") {
                    try page.navigation.go(toOffset: 0)
                }
            }
        }

        @Test func eventsReachEveryConsumerInOrder() async throws {
            try await withProfile { runtime, profile in
                try runtime.stub.scriptNavigation(to: example, title: "Example")
                let page = try profile.webContents.create()
                let first = EventReader(page.events)
                let second = EventReader(page.events)
                try page.navigation.stop()  // nothing is loading: no event
                try await page.navigate(to: example)
                runtime.stub.pump()
                let isFinished: (BDKWebContents.Event) -> Bool = {
                    if case .navigationFinished = $0 { true } else { false }
                }
                let one = await first.drain(until: isFinished)
                let two = await second.drain(until: isFinished)
                #expect(one == two)
                #expect(one.count == 4)
                guard case .navigationStarted = one[0], case .navigationCommitted = one[1],
                    case .titleChanged("Example") = one[2], case .navigationFinished = one[3]
                else {
                    Issue.record("unexpected order \(one)")
                    return
                }
            }
        }

        @Test func stateSettersRoundTrip() async throws {
            try await withProfile { _, profile in
                let page = try profile.webContents.create()
                try page.setMuted(true)
                #expect(page.isMuted && !page.isAudible)
                try page.setMuted(false)
                #expect(!page.isMuted)
                try page.setZoom(1.5)
                #expect(page.zoom.factor == 1.5)
                await expectError(.broker, "invalidArguments") { try page.setZoom(0) }
                try page.setUserAgent("Test/1.0")
                try page.discard()
                #expect(page.isAlive)  // a discarded page keeps its handle
                try page.setAutoDiscardable(false)
                try page.focus()
                #expect(page.isFocused)
            }
        }

        @Test func focusMovesBetweenPages() async throws {
            try await withProfile { runtime, profile in
                let first = try profile.webContents.create()
                let second = try profile.webContents.create()
                let events = EventReader(first.events)
                try first.focus()
                try second.focus()
                runtime.stub.pump()
                let seen = await events.drain { event in
                    if case .focusChanged(false) = event { true } else { false }
                }
                #expect(seen == [.focusChanged(true), .focusChanged(false)])
                #expect(!first.isFocused && second.isFocused)
            }
        }

        @Test func sessionStateRestoresInANewPage() async throws {
            try await withProfile { _, profile in
                let page = try profile.webContents.create()
                try await page.navigate(to: example)
                try await page.navigate(to: other)
                let state = try page.sessionState()
                let restored = try profile.webContents.create(.init(restoring: state))
                #expect(try restored.navigation.entries == page.navigation.entries)
                #expect(try restored.navigation.currentIndex == 1)
                #expect(restored.url == other)
                let decoded = try JSONDecoder().decode(
                    BDKSessionState.self, from: JSONEncoder().encode(state))
                #expect(decoded == state)
            }
        }

        @Test func newerSessionStateIsRefused() async throws {
            try await withProfile { _, profile in
                let page = try profile.webContents.create()
                let tooNew = BDKSessionState(data: Data("BDKSTUB2 -1 0\n".utf8))
                await expectError(.contents, "stateTooNew") { try page.restore(tooNew) }
                await expectError(.contents, "stateTooNew") {
                    _ = try profile.webContents.create(.init(restoring: tooNew))
                }
                #expect(profile.webContents.all.count == 1)  // the refused page was destroyed
                await expectError(.broker, "invalidArguments") {
                    try page.restore(BDKSessionState(data: Data("garbage".utf8)))
                }
            }
        }

        @Test func closeWaitsForTheClosedEventThenDestroys() async throws {
            try await withProfile { _, profile in
                let page = try profile.webContents.create()
                try await page.close()
                #expect(!page.isAlive && page.isClosed)
                await expectError(.contents, "stale") { try page.setMuted(true) }
                try await page.close()  // already closed: nothing to do
            }
        }

        @Test func aPageThatClosedItselfRefusesCallsUntilDestroyed() async throws {
            try await withProfile { _, profile in
                let page = try profile.webContents.create()
                let events = EventReader(page.events)
                var error: bdk_error_t = 0
                // The page closes itself, as window.close() does.
                #expect(bdk_contents_close(page.handle, &error) == BDK_OK)
                _ = try await events.next { if case .closed = $0 { true } else { false } }
                #expect(page.isClosed && page.isAlive)
                await expectError(.contents, "stale") { try page.setMuted(true) }
                _ = try page.state()  // still readable
                try await page.close()
                #expect(!page.isAlive)
            }
        }

        @Test func destroyedPageIsStaleEverywhere() async throws {
            try await withProfile { runtime, profile in
                let page = try profile.webContents.create()
                let events = EventReader(page.events)
                page.destroy()
                page.destroy()  // idempotent
                #expect(!page.isAlive)
                #expect(runtime.webContents.isEmpty)
                #expect(await events.next() == nil)
                await expectError(.contents, "stale") { _ = try page.state() }
                await expectError(.contents, "stale") { _ = try await page.navigate(to: example) }
                await expectError(.contents, "stale") { try page.navigation.reload() }
                await expectError(.contents, "stale") { _ = try page.navigation.entries }
                await expectError(.contents, "stale") { _ = try page.sessionState() }
                await expectError(.contents, "stale") { try page.setZoom(2) }
                await expectError(.contents, "stale") { try page.focus() }
                await expectError(.contents, "stale") { try await page.close() }
            }
        }

        @Test func droppingAPageDestroysIt() async throws {
            try await withProfile { runtime, profile in
                var page: BDKWebContents? = try profile.webContents.create()
                #expect(runtime.webContents.count == 1)
                page = nil
                #expect(runtime.webContents.isEmpty)
                #expect(page == nil)
                runtime.stub.pump()
            }
        }

        // MARK: Helpers

        /// Runs a navigation action and waits for the page to finish loading.
        private func settle(_ page: BDKWebContents, _ action: () throws -> Void) async throws {
            let events = EventReader(page.events)
            try action()
            _ = try await events.next { if case .navigationFinished = $0 { true } else { false } }
        }
    }

    /// Loads caught in flight. Automatic pumping is paused so the stub cannot finish a load early.
    @MainActor
    @Suite("WebContents loads in flight")
    struct LoadsInFlightTests {
        @Test func aSecondLoadSupersedesTheFirst() async throws {
            try await withProfile { runtime, profile in
                let page = try profile.webContents.create()
                // Paused, the stub cannot finish the first load before the second one starts. Main-actor
                // jobs run in the order they were queued, so both loads have been issued after the wait.
                let (first, second) = await runtime.stub.withAutomaticPumpingPaused {
                    let first = Task { @MainActor in try await page.navigate(to: example) }
                    let second = Task { @MainActor in try await page.navigate(to: other) }
                    while !page.isLoading { await Task.yield() }
                    await Task.yield()
                    return (first, second)
                }
                _ = try await second.value
                await expectError(.navigation, "superseded") { _ = try await first.value }
                #expect(page.url == other)
            }
        }

        @Test func stopAbortsALoad() async throws {
            try await withProfile { runtime, profile in
                let page = try profile.webContents.create()
                let load = try await runtime.stub.withAutomaticPumpingPaused {
                    let load = Task { @MainActor in try await page.navigate(to: example) }
                    while !page.isLoading { await Task.yield() }
                    try page.navigation.stop()
                    return load
                }
                await expectError(.navigation, "aborted") { _ = try await load.value }
                #expect(!page.isLoading)
            }
        }
    }
}
