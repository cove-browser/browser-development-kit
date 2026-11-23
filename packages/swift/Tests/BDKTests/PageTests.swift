import BDKTestSupport
import CBDK
import Foundation
import Testing

@testable import BDK

/// A resolved configuration with routes, as in the C conformance suite: settings and agents are
/// bundled pages, history is Chromium's, downloads is native, flags is disabled, prefs is an alias.
private let routesJSON = """
    {"browser":{"name":"BDK Conformance","internal_scheme":"conf"},
    "routes":{
    "agents":{"mode":"bundled","path":"Pages/Agents","host":"bdk-agents","capabilities":[]},
    "downloads":{"mode":"native"},
    "flags":{"mode":"disabled"},
    "history":{"mode":"chromium"},
    "settings":{"mode":"bundled","path":"Pages/Settings","host":"bdk-settings",
    "capabilities":["settings.read"]}},
    "aliases":{"prefs":"settings"}}
    """

private typealias NativeHandler = @MainActor (NativePageRequest) -> NativePageResult

/// The configuration of `routesJSON` with a handler for downloads (none when nil) and the
/// registrations `register` adds after it.
@MainActor
private func routesConfiguration(
    downloads handler: NativeHandler?, _ register: (inout PageRegistrations) -> Void = { _ in }
) -> BDKConfiguration {
    var configuration = BDKConfiguration(resolvedJSON: routesJSON)
    if let handler { configuration.pages.register("downloads", .native(handler)) }
    register(&configuration.pages)
    return configuration
}

/// `routesJSON` with downloads handled, and the registrations `register` adds.
@MainActor
private func routesConfiguration(
    _ register: (inout PageRegistrations) -> Void = { _ in }
) -> BDKConfiguration {
    routesConfiguration(downloads: { _ in .handled }, register)
}

/// Starts a navigation without waiting for it. A native route commits nothing, so `load` has no
/// outcome to return.
@MainActor
private func startLoad(_ page: BDKWebContents, _ url: String) throws {
    try abiCall(.contents) { error in
        withBDKString(url) { bdk_contents_load(page.handle, $0, error) }
    }
}

/// What a native handler was given.
@MainActor
private final class Recorder {
    var requests: [NativePageRequest] = []
}

/// A start that fails after the native runtime started shuts it down on the main actor, as
/// `BDKRuntime.start` promises; the next start must wait for that to finish.
@MainActor
private func awaitFailedStartShutdown() async throws {
    for _ in 0..<200 {
        if let runtime = try? BDKRuntime.startStub() {
            await runtime.shutdown()
            return
        }
        try await Task.sleep(for: .milliseconds(5))
    }
    Issue.record("the runtime of a failed start did not stop")
}

private func isCommit(_ event: BDKWebContents.Event) -> Bool {
    if case .navigationCommitted = event { true } else { false }
}

private func isFailure(_ event: BDKWebContents.Event) -> Bool {
    if case .navigationFailed = event { true } else { false }
}

extension BDKStubTests {
    /// Page registrations and native routes (T-SWF-06, the Swift side of T-WUI-02 and T-WUI-05).
    @MainActor
    @Suite("Page registrations")
    struct PageRegistrationTests {
        @Test func chromiumAndDisabledRegistrationsChangeHowARouteIsServed() async throws {
            let configuration = routesConfiguration { pages in
                pages.register("settings", .chromium)
                pages.register("agents", .disabled)
            }
            try await withStubRuntime(configuration) { runtime in
                let profile = try await runtime.profiles.open("Test")
                let page = try profile.webContents.create()
                let events = EventReader(page.events)
                // settings is served by Chromium although a bundled page exists.
                try startLoad(page, "conf://settings")
                runtime.stub.pump()
                guard case .navigationCommitted(let commit) = try await events.next(where: isCommit)
                else { return }
                #expect(commit.url == URL(string: "conf://settings/"))
                // agents is disabled: the standard error page.
                try startLoad(page, "conf://agents")
                runtime.stub.pump()
                guard case .navigationFailed(let failure) = try await events.next(where: isFailure)
                else { return }
                #expect(failure.netError == -20 && failure.isErrorPage)
            }
        }

        @Test func aBundledRegistrationNeedsABundledPage() async throws {
            let bundled = routesConfiguration { $0.register("agents", .bundled) }
            try await withStubRuntime(bundled) { _ in }
            let unknown = routesConfiguration { $0.register("history", .bundled) }
            await expectError(.config, "routeUnknown") {
                try await withStubRuntime(unknown) { _ in }
            }
            try await awaitFailedStartShutdown()
        }

        @Test func aRegistrationThatTheRuntimeRefusesIsTheStartError() async throws {
            struct Refusal {
                let route: String
                let mode: PageMode
                let code: String
            }
            let refused = [
                Refusal(route: "agents", mode: .chromium, code: "noBuiltin"),
                Refusal(route: "history", mode: .bundled, code: "routeUnknown"),
                Refusal(route: "prefs", mode: .disabled, code: "routeInvalid"),
                Refusal(route: "print", mode: .native { _ in .handled }, code: "routeInvalid"),
                Refusal(route: "Not_A_Route", mode: .disabled, code: "routeInvalid"),
                Refusal(route: "credits", mode: .disabled, code: "routeMode"),
            ]
            for refusal in refused {
                let configuration = routesConfiguration { $0.register(refusal.route, refusal.mode) }
                await expectError(.config, refusal.code) {
                    try await withStubRuntime(configuration) { _ in }
                }
                #expect(!BDKRuntime.isRunning, "\(refusal.route) left a runtime running")
                try await awaitFailedStartShutdown()
            }
        }

        @Test func aNativeRouteWithoutAHandlerFailsAtTheFreeze() async throws {
            await expectError(.config, "handlerMissing") {
                try await withStubRuntime(routesConfiguration(downloads: nil)) { _ in }
            }
            #expect(!BDKRuntime.isRunning)
            try await awaitFailedStartShutdown()
        }

        @Test func registeringARouteAgainReplacesTheEarlierRegistration() async throws {
            let recorder = Recorder()
            let configuration = routesConfiguration(
                downloads: { _ in .blocked },
                { pages in
                    // Two registrations of one route would be `config.duplicateName` in the runtime;
                    // Swift keeps the last, so only it reaches the ABI.
                    pages.register(
                        "downloads",
                        .native { request in
                            recorder.requests.append(request)
                            return .handled
                        })
                    pages.register("settings", .disabled)
                    pages.register("settings", .chromium)
                })
            #expect(configuration.pages.entries.map(\.route) == ["downloads", "settings"])
            try await withStubRuntime(configuration) { runtime in
                let profile = try await runtime.profiles.open("Test")
                let page = try profile.webContents.create()
                try startLoad(page, "conf://downloads")
                runtime.stub.pump()
                #expect(recorder.requests.count == 1)
            }
        }

        @Test func aNativeHandlerIsReleasedWithTheRuntime() async throws {
            weak var released: Recorder?
            do {
                let recorder = Recorder()
                released = recorder
                let configuration = routesConfiguration { pages in
                    pages.register("downloads", .native { _ in .handled })
                    pages.register(
                        "flags", .native { _ in recorder.requests.isEmpty ? .handled : .blocked })
                }
                try await withStubRuntime(configuration) { _ in }
            }
            #expect(released == nil)
        }

        @Test func aNativeRouteDeliversItsRequestAndLoadsNothingWhenHandled() async throws {
            let recorder = Recorder()
            let configuration = routesConfiguration(downloads: {
                recorder.requests.append($0)
                return .handled
            })
            try await withStubRuntime(configuration) { runtime in
                let profile = try await runtime.profiles.open("Test")
                let other = try await runtime.profiles.open("Other")
                let page = try profile.webContents.create()
                let elsewhere = try other.webContents.create()
                let events = EventReader(page.events)
                try startLoad(page, "conf://downloads/recent?x=1&y=a%20b")
                runtime.stub.pump()
                try startLoad(elsewhere, "conf://downloads")
                runtime.stub.pump()

                #expect(recorder.requests.count == 2)
                let request = try #require(recorder.requests.first)
                #expect(request.route == "downloads")
                #expect(request.url == URL(string: "conf://downloads/recent?x=1&y=a%20b"))
                #expect(request.path == "/recent")
                #expect(
                    request.query == [
                        URLQueryItem(name: "x", value: "1"), .init(name: "y", value: "a b"),
                    ])
                #expect(request.source === page)
                #expect(request.profile === profile)
                let second = recorder.requests[1]
                #expect(second.query.isEmpty && second.source === elsewhere)
                #expect(second.profile === other)

                // The navigation was cancelled: the page shows nothing and committed nothing.
                #expect(page.url?.absoluteString == "about:blank")
                try await page.close()
                let seen = await events.drain { $0 == .closed }
                #expect(!seen.contains(where: isCommit))
                try await elsewhere.close()
            }
        }

        @Test func navigatingToANativeRouteThrowsHandledNatively() async throws {
            try await withStubRuntime(routesConfiguration()) { runtime in
                let profile = try await runtime.profiles.open("Test")
                let page = try profile.webContents.create()
                // It returns, with the reason, instead of waiting for a commit that never comes.
                let url = try #require(URL(string: "conf://downloads"))
                do {
                    _ = try await page.navigate(to: url)
                    Issue.record("navigate(to:) returned a commit")
                } catch let error as BDKError {
                    #expect(error.domain == .navigation && error.code == "handledNatively")
                }
                #expect(page.url?.absoluteString == "about:blank")
                try await page.close()
            }
        }

        @Test func aBlockedNativeRouteShowsTheErrorPage() async throws {
            let configuration = routesConfiguration(downloads: { _ in .blocked })
            try await withStubRuntime(configuration) { runtime in
                let profile = try await runtime.profiles.open("Test")
                let page = try profile.webContents.create()
                let events = EventReader(page.events)
                try startLoad(page, "conf://downloads")
                runtime.stub.pump()
                guard case .navigationFailed(let failure) = try await events.next(where: isFailure)
                else { return }
                #expect(failure.netError == -20 && failure.isErrorPage)
                #expect(failure.url == URL(string: "conf://downloads/"))
            }
        }

        @Test func profileExposesTheSearchService() async throws {
            try await withProfile { _, profile in
                let search: BDKSearch = profile.search
                _ = search
            }
        }
    }

    /// Page commands and page controls (T-SWF-06).
    @MainActor
    @Suite("Page commands")
    struct PageCommandTests {
        private let first = URL(string: "https://a.test/one")!
        private let second = URL(string: "https://a.test/two")!

        @Test func everyCommandMapsToItsOwnABIValue() {
            let values = PageCommand.allCases.map(\.abiValue)
            #expect(
                values.sorted()
                    == Array(
                        UInt32(BDK_PAGE_COMMAND_BACK)...UInt32(BDK_PAGE_COMMAND_TOGGLE_FULLSCREEN)))
            #expect(PageCommand.back.abiValue == UInt32(BDK_PAGE_COMMAND_BACK))
            #expect(
                PageCommand.toggleFullscreen.abiValue == UInt32(BDK_PAGE_COMMAND_TOGGLE_FULLSCREEN))
            #expect(ZoomScope.page.abiValue == UInt32(BDK_ZOOM_SCOPE_PAGE))
            #expect(ZoomScope.site.abiValue == UInt32(BDK_ZOOM_SCOPE_SITE))
        }

        @Test func commandsFollowThePage() async throws {
            try await withProfile { runtime, profile in
                try runtime.stub.scriptNavigation(to: first, title: "One")
                try runtime.stub.scriptNavigation(to: second, title: "Two")
                let page = try profile.webContents.create()
                try await page.navigate(to: first)
                #expect(!page.canPerform(.back) && !page.canPerform(.forward))
                #expect(page.canPerform(.reload) && !page.canPerform(.stop))
                #expect(page.canPerform(.zoomIn) && !page.canPerform(.zoomReset))
                // A disabled command throws instead of doing nothing.
                await expectError(.contents, "commandDisabled") { try page.perform(.back) }

                try await page.navigate(to: second)
                #expect(page.canPerform(.back))
                try page.perform(.back)
                runtime.stub.pump()
                #expect(page.url == first)
                #expect(page.canPerform(.forward))
                try page.perform(.forward)
                runtime.stub.pump()
                #expect(page.url == second)

                try page.perform(.zoomIn)
                #expect(page.zoom.factor > 1)
                #expect(page.canPerform(.zoomReset))
                try page.perform(.zoomReset)
                #expect(page.zoom.factor == 1)

                try page.perform(.toggleMute)
                #expect(page.isMuted)
                try page.perform(.toggleMute)
                #expect(!page.isMuted)
            }
        }

        @Test func zoomHasAPageAndASiteScope() async throws {
            try await withProfile { runtime, profile in
                try runtime.stub.scriptNavigation(to: first, title: "One")
                try runtime.stub.scriptNavigation(to: second, title: "Two")
                let one = try profile.webContents.create()
                let two = try profile.webContents.create()
                try await one.navigate(to: first)
                try await two.navigate(to: second)
                try one.zoom.set(1.5, scope: .page)
                #expect(one.zoom.factor == 1.5 && two.zoom.factor == 1)
                try one.zoom.set(1.25, scope: .site)
                runtime.stub.pump()
                #expect(two.zoom.factor == 1.25)
                await expectError(.broker, "invalidArguments") {
                    try one.zoom.set(0, scope: .page)
                }
            }
        }

        @Test func mutingThroughMediaIsReadBackByThePage() async throws {
            try await withProfile { _, profile in
                let page = try profile.webContents.create()
                page.media.isMuted = true
                #expect(page.isMuted && page.media.isMuted)
                page.media.isMuted = false
                #expect(!page.isMuted)
                try page.media.exitPictureInPicture()
            }
        }

        @Test func aClosedPageCanPerformNothing() async throws {
            try await withProfile { _, profile in
                let page = try profile.webContents.create()
                try await page.close()
                #expect(PageCommand.allCases.allSatisfy { !page.canPerform($0) })
                await expectError(.contents, "stale") { try page.perform(.reload) }
                await expectError(.contents, "stale") { try page.zoom.set(1.5, scope: .site) }
                await expectError(.contents, "stale") { try page.media.exitPictureInPicture() }
                page.media.isMuted = true  // ignored: the page is gone
                #expect(!page.isMuted)
            }
        }
    }
}
