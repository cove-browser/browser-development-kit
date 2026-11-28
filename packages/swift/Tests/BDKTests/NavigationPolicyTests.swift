import BDKTestSupport
import CBDK
import Foundation
import Testing

@testable import BDK

private let first = URL(string: "https://example.com/")!
private let asked = URL(string: "https://example.com/asked")!
private let instead = URL(string: "https://example.org/instead")!

/// A policy that records what it is asked and answers when the test says so.
@MainActor
private final class HeldPolicy {
    private(set) var actions: [BDKWebContents.NavigationAction] = []
    private(set) var pages: [BDKWebContents?] = []
    private var answer: CheckedContinuation<BDKWebContents.NavigationDecision, Never>?
    private var askedWaiters: [CheckedContinuation<Void, Never>] = []

    var isWaiting: Bool {
        answer != nil
    }

    var policy: BDKRuntime.NavigationPolicy {
        { [self] page, action in
            pages.append(page)
            actions.append(action)
            return await withCheckedContinuation { continuation in
                answer = continuation
                let waiters = askedWaiters
                askedWaiters = []
                for waiter in waiters { waiter.resume() }
            }
        }
    }

    /// Returns once a navigation is waiting for its answer.
    func untilAsked() async {
        if answer != nil { return }
        await withCheckedContinuation { askedWaiters.append($0) }
    }

    func decide(_ decision: BDKWebContents.NavigationDecision) {
        let pending = answer
        answer = nil
        pending?.resume(returning: decision)
    }
}

extension BDKStubTests {
    @MainActor
    @Suite("Navigation policy")
    struct NavigationPolicyTests {
        @Test func withNoPolicyNothingChanges() async throws {
            try await withProfile { runtime, profile in
                #expect(runtime.navigationPolicy == nil)
                let page = try profile.webContents.create()
                let events = EventReader(page.events)
                let commit = try await page.navigate(to: first)
                #expect(commit.url == first)
                let seen = await events.drain {
                    if case .navigationFinished = $0 { true } else { false }
                }
                #expect(
                    !seen.contains {
                        if case .navigationFailed = $0 { true } else { false }
                    })
            }
        }

        @Test func allowAtOnceLoadsThePage() async throws {
            try await withProfile { runtime, profile in
                var actions: [BDKWebContents.NavigationAction] = []
                var pages: [BDKWebContents?] = []
                runtime.navigationPolicy = { page, action in
                    pages.append(page)
                    actions.append(action)
                    return .allow
                }
                let page = try profile.webContents.create()
                let commit = try await page.navigate(to: first)
                #expect(commit.url == first)
                #expect(actions.count == 1)
                #expect(pages.count == 1 && pages.first??.id == page.id)
                let action = try #require(actions.first)
                #expect(action.url == first)
                #expect(action.kind == .load && action.method == "GET")
                #expect(action.initiator == .application && action.isUserInitiated)
                #expect(!action.isRedirect && action.redirectedFrom == nil && !action.isPrerender)
            }
        }

        @Test func allowLaterWaitsForTheAnswer() async throws {
            try await withProfile { runtime, profile in
                let held = HeldPolicy()
                runtime.navigationPolicy = held.policy
                let page = try profile.webContents.create()
                let load = Task { try await page.navigate(to: first) }
                await held.untilAsked()
                #expect(page.isLoading)
                #expect(page.url != first)
                held.decide(.allow)
                let commit = try await load.value
                #expect(commit.url == first)
                #expect(page.url == first)
            }
        }

        @Test func cancelAtOnceThrowsItsOwnError() async throws {
            try await withProfile { runtime, profile in
                let page = try profile.webContents.create()
                try await page.navigate(to: first)
                runtime.navigationPolicy = { _, _ in .cancel }
                await expectError(.navigation, "cancelledByPolicy") {
                    try await page.navigate(to: asked)
                }
                #expect(page.url == first)
                #expect(try page.navigation.entries.map(\.url) == [first])
            }
        }

        @Test func cancelLaterThrowsItsOwnError() async throws {
            try await withProfile { runtime, profile in
                let held = HeldPolicy()
                runtime.navigationPolicy = held.policy
                let page = try profile.webContents.create()
                let load = Task { try await page.navigate(to: asked) }
                await held.untilAsked()
                held.decide(.cancel)
                await expectError(.navigation, "cancelledByPolicy") { _ = try await load.value }
                #expect(!page.isLoading)
                #expect(try page.navigation.entries.isEmpty)
            }
        }

        @Test func redirectAtOnceLoadsTheTarget() async throws {
            try await withProfile { runtime, profile in
                var asks = 0
                runtime.navigationPolicy = { _, _ in
                    asks += 1
                    return .redirect(instead)
                }
                let page = try profile.webContents.create()
                let events = EventReader(page.events)
                let commit = try await page.navigate(to: asked)
                #expect(commit.url == instead)
                #expect(page.url == instead)
                // The target's start is the application's choice and is not asked again.
                #expect(asks == 1)
                let failure = try await events.next {
                    if case .navigationFailed = $0 { true } else { false }
                }
                guard case .navigationFailed(let failed) = failure else { return }
                #expect(failed.reason == "redirectedByPolicy")
                #expect(failed.redirectURL == instead)
            }
        }

        @Test func redirectLaterLoadsTheTarget() async throws {
            try await withProfile { runtime, profile in
                let held = HeldPolicy()
                runtime.navigationPolicy = held.policy
                let page = try profile.webContents.create()
                let load = Task { try await page.navigate(to: asked) }
                await held.untilAsked()
                held.decide(.redirect(instead))
                let commit = try await load.value
                #expect(commit.url == instead)
                #expect(try page.navigation.entries.map(\.url) == [instead])
                #expect(held.actions.count == 1)
            }
        }

        @Test func aPageClosedWhileADecisionIsPending() async throws {
            try await withProfile { runtime, profile in
                let held = HeldPolicy()
                runtime.navigationPolicy = held.policy
                let page = try profile.webContents.create()
                let load = Task { try await page.navigate(to: asked) }
                await held.untilAsked()
                try await page.close()
                // The decision went stale with the navigation: answering it does nothing.
                held.decide(.redirect(instead))
                await #expect(throws: BDKError.self) { _ = try await load.value }
                #expect(!page.isAlive)
                // The runtime still asks about other pages.
                let other = try profile.webContents.create()
                let next = Task { try await other.navigate(to: first) }
                await held.untilAsked()
                held.decide(.allow)
                #expect(try await next.value.url == first)
            }
        }

        @Test func aServerRedirectIsAskedAgain() async throws {
            try await withProfile { runtime, profile in
                let moved = URL(string: "https://example.com/moved")!
                try runtime.stub.scriptRedirect(from: asked, to: moved)
                var actions: [BDKWebContents.NavigationAction] = []
                runtime.navigationPolicy = { _, action in
                    actions.append(action)
                    return .allow
                }
                let page = try profile.webContents.create()
                try await page.navigate(to: asked)
                #expect(page.url == moved)
                #expect(actions.map(\.url) == [asked, moved])
                #expect(actions.map(\.isRedirect) == [false, true])
                #expect(actions.last?.redirectedFrom == asked)

                runtime.navigationPolicy = { _, action in action.isRedirect ? .cancel : .allow }
                await expectError(.navigation, "cancelledByPolicy") {
                    try await page.navigate(to: asked)
                }
                #expect(page.url == moved)
            }
        }

        @Test func aPageNavigationIsAsked() async throws {
            try await withProfile { runtime, profile in
                let page = try profile.webContents.create()
                try await page.navigate(to: first)
                let held = HeldPolicy()
                runtime.navigationPolicy = held.policy
                let link = URL(string: "https://example.com/link")!
                let events = EventReader(page.events)
                try runtime.stub.navigateAsPage(page, to: link)
                await held.untilAsked()
                let action = try #require(held.actions.first)
                #expect(action.url == link)
                #expect(action.initiator == .page(origin: nil) && !action.isUserInitiated)
                held.decide(.cancel)
                let failure = try await events.next {
                    if case .navigationFailed = $0 { true } else { false }
                }
                guard case .navigationFailed(let failed) = failure else { return }
                #expect(failed.reason == "cancelledByPolicy")
                #expect(failed.error.code == "cancelledByPolicy")
                #expect(page.url == first)
            }
        }

        @Test func removingThePolicyAllowsEverything() async throws {
            try await withProfile { runtime, profile in
                var asks = 0
                runtime.navigationPolicy = { _, _ in
                    asks += 1
                    return .cancel
                }
                runtime.navigationPolicy = nil
                let page = try profile.webContents.create()
                #expect(try await page.navigate(to: first).url == first)
                #expect(asks == 0)
                // Replacing a policy takes effect for the next navigation.
                runtime.navigationPolicy = { _, _ in .redirect(instead) }
                runtime.navigationPolicy = { _, _ in .allow }
                #expect(try await page.navigate(to: asked).url == asked)
            }
        }
    }
}
