import AppKit
import ArcBrowserCore
import BDK
import BDKTestSupport
import Foundation
import Testing

@testable import ArcBrowserKit

/// The M8 scenarios (roadmap.md, M8) as the application performs them, on the stub runtime: what
/// the application does with its views, pages, requests and sessions. What Chromium makes of the
/// same layouts (foreground panes, tab ids, renderers, activeTab, visibility, relaunch) is checked
/// on the engine by packages/core/tests/tests_engine_arc.c, which CI cannot run.
@Suite("M8 scenarios", .serialized, .timeLimit(.minutes(2)))
@MainActor
struct ScenarioTests {
    // The stub's surface numbers (bdk_ids.h), which this package cannot import.
    private static let newWindows: UInt32 = 7
    private static let extensionPopups: UInt32 = 26
    private static let extensionSidePanels: UInt32 = 27

    @Test("sidebar, pinned pages and two spaces, each with a profile of its own")
    func spaces() async throws {
        try await withBrowser { browser in
            let personal = try browser.space("Personal")
            let work = try browser.space("Work")
            #expect(personal.pinned.count == 1 && work.pinned.count == 1)
            // Every page of both spaces exists; only the window's pane is on screen.
            #expect(browser.controller.registry.pages.count == 4)
            let shown = try browser.page(personal.pinned[0])
            #expect(shown.isOnScreen)
            for id in personal.pages + work.allPages {
                #expect(try !browser.page(id).isOnScreen)
                #expect(try browser.page(id).view.window == nil)
            }
            // Each space's pages are in that space's profile.
            #expect(try browser.page(personal.pages[0]).profile === shown.profile)
            #expect(try browser.page(work.pinned[0]).profile !== shown.profile)
            #expect(try browser.page(work.pinned[0]).profile.info().name == "Work")
            #expect(browser.controller.layout.windows.count == 1)
        }
    }

    @Test("split view: up to three pages side by side in one window")
    func splitView() async throws {
        try await withBrowser { browser in
            let controller = browser.controller
            try await controller.open(try #require(URL(string: "https://third.example/")))
            try await controller.perform(AppOptions.Action.splitNext)
            try await controller.perform(AppOptions.Action.splitNext)
            let window = try controller.layout.window(controller.focusedWindow)
            #expect(window.panes.count == 3)
            let pages = try window.panes.map { try browser.page($0) }
            let nsWindow = try #require(browser.focusedNSWindow)
            nsWindow.contentView?.layoutSubtreeIfNeeded()
            #expect(pages.allSatisfy { $0.view.window === nsWindow && $0.isOnScreen })
            let origins = pages.map { $0.view.convert(NSPoint.zero, to: nil).x }
            #expect(origins == origins.sorted() && Set(origins).count == 3)
            // A fourth does not fit.
            try await controller.open(try #require(URL(string: "https://fourth.example/")))
            #expect(try controller.layout.window(controller.focusedWindow).panes.count == 3)
            await #expect(throws: Layout.Failure.splitFull(limit: 3)) {
                try await controller.perform(AppOptions.Action.splitNext)
            }
        }
    }

    @Test("a page moves between panes, spaces and windows as the same page with its history")
    func movePages() async throws {
        try await withBrowser { browser in
            let controller = browser.controller
            let id = try controller.focusedPage()
            let page = try browser.page(id)
            try await page.navigate(to: try #require(URL(string: "https://second.example/")))
            let entries = try page.navigation.entries.map(\.url)
            try await controller.perform(AppOptions.Action.splitNext)
            // Between the panes of the split.
            try controller.change { try $0.movePane(from: 0, to: 1, in: controller.focusedWindow) }
            #expect(try controller.layout.window(controller.focusedWindow).panes.last == id)
            // To the other space: the window shows Personal, so the page leaves it.
            try await controller.perform(.moveToSpace("Work"))
            #expect(page.view.window == nil)
            #expect(controller.layout.spaceOf(id)?.name == "Work")
            // To a window showing Work.
            try await controller.perform(.newWindow("Work"))
            let second = controller.focusedWindow
            try controller.change { try $0.show(id, in: second) }
            #expect(page.view.window === controller.windows[second]?.window)
            #expect(try browser.page(id) === page)
            #expect(try page.navigation.entries.map(\.url) == entries)
            // And back to the first window, in its split.
            try await controller.perform(.focusWindow(1))
            try await controller.perform(.moveToWindow(0))
            #expect(
                page.view.window === controller.windows[controller.layout.windows[0].id]?.window)
            #expect(try page.navigation.entries.map(\.url) == entries)
        }
    }

    @Test("closing a page closes its contents and shows the space's next page in its pane")
    func closePage() async throws {
        try await withBrowser { browser in
            let controller = browser.controller
            let id = try controller.focusedPage()
            let page = try browser.page(id)
            try await controller.perform(.closePage)
            #expect(!page.isAlive && page.view.window == nil)
            #expect(controller.registry.pages[id] == nil && controller.layout.pages[id] == nil)
            // The window shows Personal's other page, on screen, rather than an empty pane.
            let next = try controller.focusedPage()
            #expect(next != id && controller.layout.spaceOf(next)?.name == "Personal")
            #expect(try browser.page(next).isOnScreen)
        }
    }

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

    @Test("a page in a hidden space leaves the window, keeps running, and comes back")
    func hideAndRestore() async throws {
        try await withBrowser { browser in
            let controller = browser.controller
            let id = try controller.focusedPage()
            let page = try browser.page(id)
            let view = page.view
            try await controller.perform(.switchSpace("Work"))
            #expect(view.window == nil && page.isAlive)
            #expect(!controller.layout.visiblePages.contains(id))
            // It still navigates while hidden.
            try await page.navigate(to: try #require(URL(string: "https://while-hidden.example/")))
            try await controller.perform(.switchSpace("Personal"))
            #expect(page.view === view && page.isOnScreen)
            #expect(page.url?.absoluteString == "https://while-hidden.example/")
        }
    }

    @Test("the application saves its layout and each page's state, and restores both")
    func sessionRestoration() async throws {
        var saved: SessionFile?
        var entries: [Layout.PageID: [URL?]] = [:]
        try await withBrowser { browser in
            let controller = browser.controller
            let page = try browser.page(try controller.focusedPage())
            try await page.navigate(to: try #require(URL(string: "https://later.example/")))
            try await controller.perform(.splitNext)
            try await controller.perform(.pin)
            try await controller.perform(.newWindow("Work"))
            for (id, page) in controller.registry.pages {
                entries[id] = try page.navigation.entries.map(\.url)
            }
            saved = controller.sessionFile()
        }
        let session = try #require(saved)
        try await withBrowser(session: session) { browser in
            let controller = browser.controller
            #expect(controller.restored)
            #expect(controller.layout.spaces == session.layout.spaces)
            #expect(controller.layout.windows == session.layout.windows)
            #expect(controller.windows.count == 2)
            for (id, urls) in entries {
                let restored = try browser.page(id).navigation.entries.map(\.url)
                #expect(restored == urls)
            }
            for id in controller.layout.visiblePages {
                let page = try browser.page(id)
                #expect(page.isOnScreen)
            }
        }
    }

    @Test("an extension's popup and side panel appear in the application's containers")
    func extensionsInTheLayout() async throws {
        try await withBrowser { browser in
            let page = try browser.page(try browser.controller.focusedPage())
            let stub = browser.runtime.stub
            try stub.raiseUIRequest(
                surface: Self.extensionPopups, on: page,
                payloadJSON: #"{"extensionId":"abc","trigger":"userClick"}"#)
            try await waitUntil("the popup") { !browser.placement.popups.isEmpty }
            let popup = try #require(browser.placement.popups.first)
            #expect(popup.isShown && popup.extensionID == "abc")
            #expect(popup.secureView.window != nil)
            #expect(try stub.lastUIUpdate == #"{"placed":true}"#)
            try stub.withdrawUIRequests()
            try await waitUntil("the popup to close") { browser.placement.popups.isEmpty }

            try stub.raiseUIRequest(
                surface: Self.extensionSidePanels, on: page,
                payloadJSON: #"{"extensionId":"abc","title":"Notes","scope":"window"}"#)
            try await waitUntil("the panel") { !browser.placement.sidePanels.isEmpty }
            let panel = try #require(browser.placement.sidePanels[browser.controller.focusedWindow])
            #expect(panel.secureView.window === browser.focusedNSWindow)
            #expect(try stub.lastUIUpdate.contains(#""side":"trailing""#))
            // The container's close button.
            browser.placement.closeSidePanel(in: browser.controller.focusedWindow)
            #expect(browser.placement.sidePanels.isEmpty)
            #expect(
                browser.controller.windows[browser.controller.focusedWindow]?.sidePanel.shownView
                    == nil)
            let outcome = try stub.lastUIOutcome
            #expect(
                outcome.completion == "accept" && outcome.argumentsJSON == #"{"action":"close"}"#)
        }
    }

    /// The stub serves no bundled page, so the page commits only on the engine (the end-to-end
    /// test and tests_engine_pages.c); here the application asks for the branded URL and shows it.
    @Test("the bundled settings page is opened at its branded URL from bdk.toml")
    func settingsPage() async throws {
        try await withBrowser { browser in
            let routes = try BrowserFixture.resolvedJSON
            #expect(routes.contains(#""internal_scheme": "arc""#))
            #expect(routes.contains(#""settings.write""#))
            try await browser.controller.openSettings()
            let id = try #require(browser.controller.settingsPage)
            #expect(browser.controller.layout.pages[id]?.url == "arc://settings")
            #expect(try browser.page(id).isOnScreen)
            #expect(try browser.controller.focusedPage() == id)
        }
    }

    @Test("a script drives one page in each space, and sees no other page")
    func automation() async throws {
        try await withBrowser { browser in
            let script = try browser.runtime.stub.defineScript(AutomationRunner.script)
            let runner = AutomationRunner(controller: browser.controller)
            let pages = runner.defaultPages
            #expect(pages.count == 2)
            let run = Task { @MainActor in try await runner.run() }
            try await waitUntil("the start frame") { !script.frames.isEmpty }
            try script.send(#"{"type":"call","id":1,"command":"contents.list","args":{}}"#)
            try await waitUntil("the list") { script.frames.count >= 2 }
            guard case .object(let frame) = try #require(script.last),
                case .object(let value) = frame["value"], case .array(let items) = value["items"]
            else { throw BrowserFixture.Failure(description: "no contents.list result") }
            #expect(items.count == 2)
            try script.send(.object(["type": .string("done"), "result": .object(value)]))
            _ = try await run.value
            let result = try #require(browser.controller.automationResult)
            for id in pages {
                let url = try #require(browser.page(id).url?.absoluteString)
                #expect(result.contains(url))
            }
        }
    }
}
