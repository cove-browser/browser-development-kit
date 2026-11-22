import ArcBrowserCore
import Foundation
import Testing

/// The layout model alone, with no runtime: spaces, pinned pages, splits, moves and their rules.
@Suite("Layout")
struct LayoutTests {
    /// The initial layout, with the ids of its two spaces.
    private func start() -> (layout: Layout, spaces: [Layout.SpaceID]) {
        let layout = Layout.initial()
        return (layout, layout.spaces.map(\.id))
    }

    @Test("the first launch has two spaces with a profile each, a pinned page each, one window")
    func initial() throws {
        let (layout, spaces) = start()
        let (personal, work) = (spaces[0], spaces[1])
        try layout.validate()
        #expect(layout.spaces.map(\.name) == ["Personal", "Work"])
        #expect(layout.spaces.map(\.profile) == ["Personal", "Work"])
        #expect(layout.spaces.allSatisfy { $0.pinned.count == 1 && $0.pages.count == 1 })
        #expect(layout.windows.count == 1)
        let window = try #require(layout.windows.first)
        #expect(window.space == personal)
        #expect(window.panes == (try layout.space(personal)).pinned)
        #expect(layout.visiblePages.count == 1)
        #expect(layout.hiddenPages.count == 3)
        #expect(layout.hiddenPages.isSuperset(of: try layout.space(work).allPages))
    }

    @Test("a split holds up to three pages of the window's space, and each can be focused")
    func split() throws {
        var (layout, spaces) = start()
        let personal = spaces[0]
        let window = layout.windows[0].id
        let third = try layout.addPage(url: "https://three.example/", to: personal)
        let fourth = try layout.addPage(url: "https://four.example/", to: personal)
        let second = try layout.space(personal).pages[0]
        try layout.split(second, in: window)
        try layout.split(third, in: window)
        #expect(try layout.window(window).panes.count == 3)
        #expect(try layout.window(window).focusedPage == third)
        #expect(throws: Layout.Failure.splitFull(limit: 3)) { try layout.split(fourth, in: window) }
        try layout.focusPane(0, in: window)
        try layout.movePane(from: 0, to: 2, in: window)
        #expect(try layout.window(window).panes.last == (try layout.space(personal)).pinned[0])
        #expect(try layout.window(window).focusedPane == 2)
        try layout.closePane(0, in: window)
        #expect(try layout.window(window).panes.count == 2)
        #expect(layout.hiddenPages.contains(second))
        try layout.validate()
    }

    @Test("switching spaces hides one space's pages and brings back what the other showed")
    func switchSpaces() throws {
        var (layout, spaces) = start()
        let (personal, work) = (spaces[0], spaces[1])
        let window = layout.windows[0].id
        try layout.split(try layout.space(personal).pages[0], in: window)
        let shownBefore = try layout.window(window).panes
        try layout.switchSpace(of: window, to: work)
        #expect(try layout.window(window).panes == [try layout.space(work).pinned[0]])
        #expect(layout.hiddenPages.isSuperset(of: shownBefore))
        try layout.switchSpace(of: window, to: personal)
        #expect(try layout.window(window).panes == shownBefore)
        try layout.validate()
    }

    @Test("a page moved to another space leaves a window that does not show that space")
    func moveToSpace() throws {
        var (layout, spaces) = start()
        let (personal, work) = (spaces[0], spaces[1])
        let window = layout.windows[0].id
        let page = try layout.space(personal).pages[0]
        try layout.split(page, in: window)
        try layout.move(page, toSpace: work)
        #expect(layout.spaceOf(page)?.id == work)
        #expect(try !layout.window(window).panes.contains(page))
        #expect(layout.hiddenPages.contains(page))
        // A pinned page stays pinned in its new space.
        let pinned = try layout.space(personal).pinned[0]
        try layout.move(pinned, toSpace: work)
        #expect(try layout.space(work).pinned.contains(pinned))
        try layout.validate()
    }

    @Test(
        "a page moved to another window joins that window's space and split, and leaves the first")
    func moveToWindow() throws {
        var (layout, spaces) = start()
        let (personal, work) = (spaces[0], spaces[1])
        let first = layout.windows[0].id
        let second = try layout.addWindow(showing: work)
        let page = try layout.space(personal).pinned[0]
        #expect(try layout.window(first).panes == [page])
        try layout.move(page, toWindow: second)
        #expect(try layout.window(first).panes.isEmpty)
        #expect(try layout.window(second).panes.contains(page))
        #expect(layout.spaceOf(page)?.id == work)
        #expect(layout.windowShowing(page)?.id == second)
        try layout.validate()
    }

    @Test("showing a page another window shows takes it from that window; a page is in one place")
    func onePlace() throws {
        var (layout, spaces) = start()
        let personal = spaces[0]
        let first = layout.windows[0].id
        let second = try layout.addWindow(showing: personal)
        let pinned = try layout.space(personal).pinned[0]
        // The second window got the page the first does not show.
        #expect(try layout.window(second).panes == [try layout.space(personal).pages[0]])
        try layout.show(pinned, in: second)
        #expect(try layout.window(first).panes.isEmpty)
        #expect(try layout.window(second).panes == [pinned])
        try layout.validate()
        try layout.showAlone(try layout.space(personal).pages[0], in: second)
        #expect(try layout.window(second).panes.count == 1)
    }

    @Test("pinning, closing and the last window")
    func pinCloseWindows() throws {
        var (layout, spaces) = start()
        let personal = spaces[0]
        let page = try layout.space(personal).pages[0]
        try layout.setPinned(page, true)
        #expect(try layout.space(personal).pinned.contains(page))
        try layout.setPinned(page, false)
        #expect(try layout.space(personal).pages.first == page)
        try layout.close(page)
        #expect(layout.pages[page] == nil && layout.spaceOf(page) == nil)
        #expect(throws: Layout.Failure.lastWindow) { try layout.closeWindow(layout.windows[0].id) }
        #expect(throws: Layout.Failure.unknownPage(page)) { try layout.close(page) }
        try layout.validate()
    }

    @Test("a session file round-trips, and damaged ones are refused")
    func sessionFile() throws {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("arc-layout-\(UUID().uuidString)", isDirectory: true)
        defer { try? FileManager.default.removeItem(at: directory) }
        #expect(try SessionFile.load(from: directory) == nil)
        var (layout, spaces) = start()
        let personal = spaces[0]
        try layout.split(try layout.space(personal).pages[0], in: layout.windows[0].id)
        let page = try layout.space(personal).pinned[0]
        let file = SessionFile(layout: layout, states: [page: Data([1, 2, 3])])
        try file.save(to: directory)
        let loaded = try #require(try SessionFile.load(from: directory))
        #expect(loaded == file)
        #expect(loaded.state(of: page) == Data([1, 2, 3]))
        // Page ids are the keys of a readable table.
        let text = try String(contentsOf: SessionFile.fileURL(in: directory), encoding: .utf8)
        #expect(text.contains("\"\(page.rawValue)\" : {"))

        try Data(#"{"version":2}"#.utf8).write(to: SessionFile.fileURL(in: directory))
        #expect(throws: (any Error).self) { try SessionFile.load(from: directory) }
        var broken =
            try JSONSerialization.jsonObject(with: JSONEncoder().encode(file)) as? [String: Any]
        var brokenLayout = broken?["layout"] as? [String: Any]
        brokenLayout?["windows"] = []
        broken?["layout"] = brokenLayout
        try JSONSerialization.data(withJSONObject: broken ?? [:]).write(
            to: SessionFile.fileURL(in: directory))
        #expect(throws: SessionFile.Failure.invalidLayout("invalid(\"there is no window\")")) {
            try SessionFile.load(from: directory)
        }
    }

    @Test("launch options")
    func options() throws {
        let options = try AppOptions.parse([
            "app", "--support-dir", "/tmp/a", "--do", "split-next", "--do", "switch-space=Work",
            "--do", "move-to-window=1", "--fresh", "-NSDocumentRevisionsDebugMode", "YES",
            "--quit-after-actions",
        ])
        #expect(options.supportDirectory?.path == "/tmp/a")
        #expect(options.actions == [.splitNext, .switchSpace("Work"), .moveToWindow(1)])
        #expect(options.fresh && options.quitAfterActions)
        #expect(throws: AppOptions.Failure.unknownAction("fly")) {
            try AppOptions.parse(["app", "--do", "fly"])
        }
        #expect(throws: AppOptions.Failure.missingValue("--status-file")) {
            try AppOptions.parse(["app", "--status-file"])
        }
    }
}
