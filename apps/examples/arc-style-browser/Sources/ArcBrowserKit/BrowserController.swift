import AppKit
import ArcBrowserCore
import BDK
import os

/// The application: its layout, the pages, and the windows that show them. Every change goes
/// through the layout model and then `apply()`, which puts page views where the layout says.
/// Moving a view is all BDK needs: a view entering another window moves the page there through
/// the tab model, and a view leaving every window leaves the page a background tab
/// (swift-api.md section 7). There is no BDK call for spaces, splits or tabs.
@MainActor
public final class BrowserController {
    public internal(set) var layout: Layout
    public let registry: PageRegistry
    public private(set) var windows: [Layout.WindowID: BrowserWindow] = [:]
    /// The window the person last used; actions apply to it.
    public private(set) var focusedWindow: Layout.WindowID
    public private(set) var restored: Bool
    /// The page the bundled settings page was opened in, once it was.
    public internal(set) var settingsPage: Layout.PageID?
    /// The last automation task's result, as JSON.
    public internal(set) var automationResult: String?
    /// Windows open behind the others and never take focus (`--background`).
    public var showsWindowsInBackground = false
    /// Shown under each window's title; the application says there which runtime it runs on when
    /// that is the stub, which renders nothing.
    public var windowSubtitle = ""
    /// Called after anything the status report shows may have changed.
    public var onChange: () -> Void = {}
    let extensions: ExtensionPlacement
    private let states: [Layout.PageID: Data]
    private var observers: [Layout.PageID: Task<Void, Never>] = [:]
    let log = Logger(subsystem: "dev.bdk.examples.arc-style-browser", category: "controller")

    /// `session` is what the last run saved; nil starts from the initial layout.
    public init(
        runtime: BDKRuntime, session: SessionFile?, initial: Layout = .initial(),
        extensions: ExtensionPlacement
    ) {
        layout = session?.layout ?? initial
        states = Dictionary(
            uniqueKeysWithValues: (session?.states ?? [:]).map { (Layout.PageID($0.key), $0.value) }
        )
        restored = session != nil
        registry = PageRegistry(runtime: runtime)
        focusedWindow = layout.windows[0].id
        self.extensions = extensions
        extensions.controller = self
    }

    public var runtime: BDKRuntime { registry.runtime }

    /// Makes every page of every space, then the windows.
    public func start() async throws {
        for space in layout.spaces {
            for id in space.allPages { try await makePage(id, in: space) }
        }
        apply()
        windows[focusedWindow]?.showWindow(background: showsWindowsInBackground)
        focusPage()
    }

    private func makePage(_ id: Layout.PageID, in space: Layout.Space) async throws {
        let record = layout.pages[id] ?? Layout.Page(url: "about:blank")
        let page = try await registry.page(
            id, profile: space.profile, url: record.url, state: states[id])
        observe(page, id: id)
    }

    // MARK: Applying the layout

    /// Puts each window's panes in it, takes every other page's view out of every window, and
    /// opens and closes windows to match the layout.
    public func apply() {
        for window in layout.windows where windows[window.id] == nil {
            let browserWindow = BrowserWindow(
                id: window.id, runtime: runtime, subtitle: windowSubtitle)
            wire(browserWindow)
            windows[window.id] = browserWindow
            browserWindow.showWindow(background: showsWindowsInBackground)
        }
        for (id, browserWindow) in windows where !layout.windows.contains(where: { $0.id == id }) {
            windows[id] = nil
            browserWindow.onClose = {}
            browserWindow.show([], focused: 0)
            browserWindow.window.close()
        }
        // A view leaves a window before it enters another, so a page moved between windows is
        // in exactly one of them at any time.
        let visible = layout.visiblePages
        for (id, page) in registry.pages where !visible.contains(id) {
            page.view.removeFromSuperview()
        }
        for window in layout.windows {
            guard let browserWindow = windows[window.id] else { continue }
            browserWindow.show(
                window.panes.compactMap { registry.pages[$0] }, focused: window.focusedPane)
            updateSidebar(of: browserWindow, for: window)
        }
        onChange()
    }

    private func wire(_ browserWindow: BrowserWindow) {
        let id = browserWindow.id
        browserWindow.sidebar.onRequest = { [weak self] request in
            self?.handle(request, from: id)
        }
        browserWindow.onPaneFocused = { [weak self] pane in
            guard let self else { return }
            try? layout.focusPane(pane, in: id)
            focusedWindow = id
            onChange()
        }
        browserWindow.onBecameKey = { [weak self] in self?.focusedWindow = id }
        browserWindow.onClose = { [weak self] in self?.windowClosed(id) }
        browserWindow.sidePanel.onClose = { [weak self] in self?.extensions.closeSidePanel(in: id) }
    }

    private func windowClosed(_ id: Layout.WindowID) {
        // The last window's close quits the application, which saves the layout as it is.
        guard layout.windows.count > 1 else { return }
        try? layout.closeWindow(id)
        windows[id] = nil
        if focusedWindow == id { focusedWindow = layout.windows[0].id }
        apply()
    }

    private func updateSidebar(of browserWindow: BrowserWindow, for window: Layout.Window) {
        guard let space = try? layout.space(window.space) else { return }
        func rows(_ ids: [Layout.PageID]) -> [SidebarView.Row] {
            ids.map { id in
                SidebarView.Row(
                    id: id, title: registry.pages[id]?.title ?? layout.pages[id]?.title ?? "",
                    isShown: window.panes.contains(id))
            }
        }
        browserWindow.sidebar.update(
            layout: layout, space: space, pinnedRows: rows(space.pinned),
            pageRows: rows(space.pages))
    }

    /// Gives the keyboard to the focused page of the focused window.
    func focusPage() {
        guard let window = try? layout.window(focusedWindow) else { return }
        windows[focusedWindow]?.focusPage(at: window.focusedPane)
    }

    // MARK: Pages

    /// Keeps the layout's record of the page (what the sidebar shows) up to date.
    private func observe(_ page: BDKWebContents, id: Layout.PageID) {
        let events = page.events
        observers[id] = Task { @MainActor [weak self] in
            for await event in events {
                guard let self else { return }
                switch event {
                case .titleChanged(let title): try? layout.update(id, title: title)
                case .navigationCommitted:
                    try? layout.update(id, url: page.url?.absoluteString ?? "")
                case .closed: return
                default: break
                }
                refreshSidebars()
            }
        }
    }

    func refreshSidebars() {
        for window in layout.windows {
            if let browserWindow = windows[window.id] {
                updateSidebar(of: browserWindow, for: window)
            }
        }
        onChange()
    }

    /// A new page in the focused window's space, shown in its focused pane.
    @discardableResult
    public func open(_ url: URL) async throws -> Layout.PageID {
        let window = try layout.window(focusedWindow)
        let space = try layout.space(window.space)
        let id = try layout.addPage(url: url.absoluteString, to: space.id)
        try await makePage(id, in: try layout.space(space.id))
        try layout.show(id, in: focusedWindow)
        apply()
        focusPage()
        return id
    }

    public func close(_ id: Layout.PageID) async throws {
        try layout.close(id)
        observers.removeValue(forKey: id)?.cancel()
        apply()
        await registry.close(id)
    }

    /// Runs a layout operation, then applies it.
    func change(_ operation: (inout Layout) throws -> Void) throws {
        try operation(&layout)
        apply()
        focusPage()
    }

    func setFocusedWindow(_ id: Layout.WindowID) {
        focusedWindow = id
    }

    /// Closes every window without changing the layout: what a quit does, after the session is
    /// saved.
    public func closeWindows() {
        for browserWindow in windows.values {
            browserWindow.onClose = {}
            browserWindow.window.close()
        }
        windows = [:]
    }

    // MARK: Session

    /// What to save: the layout with the current titles and URLs, and each page's state.
    public func sessionFile() -> SessionFile {
        var saved = layout
        for (id, page) in registry.pages where page.isAlive {
            try? saved.update(id, url: page.url?.absoluteString, title: page.title)
        }
        return SessionFile(layout: saved, states: registry.states())
    }
}
