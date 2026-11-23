import AppKit
import ArcBrowserCore
import BDK
import BDKTestSupport
import Foundation
import Testing

@testable import ArcBrowserKit

/// What pages do to the layout and the window: links that open new pages, a page that closes
/// itself, the window's title and the Go menu. Part of the serialized M8 suite: the stub runs one
/// runtime at a time.
extension ScenarioTests {
    // The stub's surface number (bdk_ids.h), which this package cannot import.
    private static let newWindows: UInt32 = 7

    /// Raises the new-window request Chromium raises for a link the page opens, with a page the
    /// runtime made for it, and returns that page.
    private func openFromLink(
        _ browser: BrowserFixture, from opener: BDKWebContents, disposition: String,
        kind: String = "newPage"
    ) async throws -> BDKWebContents {
        let created = try opener.profile.webContents.create()
        try browser.runtime.stub.raiseUIRequest(
            surface: Self.newWindows, on: opener,
            payloadJSON: """
                {"kind":"\(kind)","disposition":"\(disposition)",\
                "targetURL":"https://linked.example/","opener":"\(opener.id.rawValue)",\
                "newContents":"\(created.id.rawValue)","userGesture":true}
                """)
        try await waitUntil("the request's outcome") {
            try browser.runtime.stub.lastUIOutcome.kind != .none
        }
        return created
    }

    @Test("a link a page opens in a new page becomes a page of its space, in its pane")
    func linksOpenNewPages() async throws {
        try await withBrowser { browser in
            let controller = browser.controller
            let openerID = try controller.focusedPage()
            let opener = try browser.page(openerID)
            let space = try #require(controller.layout.spaceOf(openerID))

            // target="_blank" or "Open link in new page": shown where the opener was.
            let foreground = try await openFromLink(
                browser, from: opener, disposition: "foreground")
            #expect(try browser.runtime.stub.lastUIOutcome.completion == "accept")
            let id = try #require(controller.registry.id(of: foreground))
            #expect(controller.layout.spaceOf(id)?.id == space.id)
            #expect(try controller.focusedPage() == id && foreground.isOnScreen)
            #expect(try browser.space(space.name).pages.contains(id))

            // A middle click: in the sidebar only; the page on screen stays.
            let background = try await openFromLink(
                browser, from: foreground, disposition: "background")
            let backgroundID = try #require(controller.registry.id(of: background))
            #expect(try browser.space(space.name).pages.contains(backgroundID))
            #expect(try controller.focusedPage() == id && !background.isOnScreen)

            // Shift-click: a new window of the space.
            let windows = controller.layout.windows.count
            let separate = try await openFromLink(
                browser, from: foreground, disposition: "newWindow")
            #expect(controller.layout.windows.count == windows + 1)
            // Focus events of the pages focused before it arrive first.
            try await waitUntil("the new window's page to have focus") {
                try controller.focusedPage() == controller.registry.id(of: separate)
            }

            // window.open with features (a sign-in page): BDK's own window, not the layout.
            let popup = try await openFromLink(
                browser, from: foreground, disposition: "popup", kind: "popup")
            #expect(try browser.runtime.stub.lastUIOutcome.kind == .defaulted)
            #expect(controller.registry.id(of: popup) == nil)
        }
    }

    @Test("a page that closes itself leaves the layout, and its opener comes back")
    func pageClosesItself() async throws {
        try await withBrowser { browser in
            let controller = browser.controller
            let openerID = try controller.focusedPage()
            let opened = try await openFromLink(
                browser, from: try browser.page(openerID), disposition: "foreground")
            let id = try #require(controller.registry.id(of: opened))
            #expect(try controller.focusedPage() == id)
            // window.close(): the page reports closed without the application asking.
            try await opened.close()
            try await waitUntil("the page to leave the layout") {
                controller.layout.pages[id] == nil
            }
            #expect(controller.registry.pages[id] == nil)
            #expect(try controller.focusedPage() == openerID)
            #expect(try browser.page(openerID).isOnScreen)
        }
    }

    @Test("the Go menu reloads the focused page and moves it through its history")
    func history() async throws {
        try await withBrowser { browser in
            let controller = browser.controller
            let page = try browser.page(try controller.focusedPage())
            let first = try #require(page.url)
            let second = try #require(URL(string: "https://second.example/"))
            try await page.navigate(to: second)
            try controller.perform(BrowserController.HistoryCommand.back)
            try await waitUntil("back") { page.url == first }
            try controller.perform(BrowserController.HistoryCommand.forward)
            try await waitUntil("forward") { page.url == second }
            // Forward again has nowhere to go and does nothing.
            try controller.perform(BrowserController.HistoryCommand.forward)
            try controller.perform(BrowserController.HistoryCommand.reload)
            try await waitUntil("the reload") { page.url == second && !page.isLoading }
            #expect(try page.navigation.entries.map(\.url) == [first, second])
        }
    }

    @Test("the window's title follows its focused page's title")
    func windowTitle() async throws {
        try await withBrowser { browser in
            let page = try browser.page(try browser.controller.focusedPage())
            try browser.runtime.stub.scriptNavigation(
                to: try #require(URL(string: "https://titled.example/")), title: "Titled page")
            try await page.navigate(to: try #require(URL(string: "https://titled.example/")))
            try await waitUntil("the title") { browser.focusedNSWindow?.title == "Titled page" }
        }
    }
}
