import ArcBrowserCore
import BDK
import BDKTestSupport
import Foundation
import Testing

@testable import ArcBrowserKit

/// Robustness of the application on the stub (T-REL-03): many pages opened and closed, and a page
/// whose renderer crashed. `BDK_ROBUSTNESS_ITERATIONS` sets the rounds (default 40, about two
/// seconds); the pre-release pass (`nx run arc-style-browser:test-release`) runs 500. Part of the
/// serialized M8 suite: the stub runs one runtime at a time.
extension ScenarioTests {
    private static var rounds: Int {
        let value = ProcessInfo.processInfo.environment["BDK_ROBUSTNESS_ITERATIONS"].flatMap {
            Int($0)
        }
        return max(1, value ?? 40)
    }

    @Test("pages opened and closed many times leave the layout and the registry as they were")
    func manyPagesOpenAndClose() async throws {
        try await withBrowser { browser in
            let controller = browser.controller
            let before = (
                pages: controller.registry.pages.count, layout: controller.layout.pages.count
            )
            let focused = try controller.focusedPage()
            for round in 0..<Self.rounds {
                let url = try #require(URL(string: "https://round-\(round % 7).example/"))
                let id = try await controller.open(url)
                #expect(try controller.focusedPage() == id)
                try await controller.close(id)
            }
            #expect(controller.registry.pages.count == before.pages)
            #expect(controller.layout.pages.count == before.layout)
            #expect(try controller.focusedPage() == focused)
            #expect(try browser.page(focused).isOnScreen)
        }
    }

    @Test(
        "a page whose renderer crashed reloads from the Go menu, and the other pages are unaffected"
    )
    func crashedPageReloads() async throws {
        try await withBrowser { browser in
            let controller = browser.controller
            let id = try controller.focusedPage()
            let page = try browser.page(id)
            let url = try #require(page.url)
            let others = controller.registry.pages.filter { $0.key != id }.map(\.value)
            for _ in 0..<max(1, Self.rounds / 10) {
                var events = page.events.makeAsyncIterator()
                // Chromium's debug URL kills the renderer and commits nothing: the load never returns.
                let crash = Task {
                    try await page.navigation.load(try #require(URL(string: "chrome://crash")))
                }
                var gone = false
                while !gone, let event = await events.next() {
                    if case .rendererGone(let reason) = event {
                        #expect(reason == "crashed")
                        gone = true
                    }
                }
                // A cancelled load stops the page's navigation: let it finish before the reload, or
                // its stop aborts the reload.
                crash.cancel()
                _ = await crash.result
                #expect(gone)
                try controller.perform(BrowserController.HistoryCommand.reload)
                var finished = false
                while !finished, let event = await events.next() {
                    if case .navigationFinished(let commit) = event { finished = commit.url == url }
                }
                #expect(page.url == url)
            }
            #expect(controller.registry.pages[id] === page)
            #expect(others.allSatisfy { !$0.isClosed })
        }
    }
}
