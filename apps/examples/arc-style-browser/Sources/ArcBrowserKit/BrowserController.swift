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
    /// Set when the application quits: pages that close then are not removed from the layout.
    public var isQuitting = false
    let extensions: ExtensionPlacement
    private let states: [Layout.PageID: Data]
    private var observers: [Layout.PageID: Task<Void, Never>] = [:]
    /// One per open profile: refreshes the extensions areas when its extensions change.
    var extensionObservers: [Task<Void, Never>] = []
    /// The pending refresh of the extensions areas; a newer one replaces it.
    var extensionRefresh: Task<Void, Never>?
    /// The page each adopted page was opened by, shown again if the new one closes itself.
    private var openers: [Layout.PageID: Layout.PageID] = [:]
    let log = Logger(subsystem: "dev.bdk.examples.arc-style-browser", category: "controller")

    /// `session` is what the last run saved; nil starts from the initial layout.
    public init(
        runtime: BDKRuntime, session: SessionFile?, initial: Layout = .initial(),
        extensions: ExtensionPlacement, pages: PagePlacement
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
        pages.controller = self
        registry.onOpen = { [weak self] in self?.observeExtensions(of: $0) }
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
        // A window may now show a page of another profile, or another page with its own badges.
        scheduleExtensionRefresh()
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
        // Subscribed now, not when the task first runs, so no early event is missed. The iterator
        // only moves from the main actor to this main-actor task.
        nonisolated(unsafe) var iterator = page.events.makeAsyncIterator()
        observers[id] = Task { @MainActor [weak self] in
            while let event = await iterator.next() {
                guard let self else { return }
                switch event {
                case .titleChanged(let title): try? layout.update(id, title: title)
                case .navigationCommitted:
                    try? layout.update(id, url: page.url?.absoluteString ?? "")
                case .closed:
                    // The application's own close stops observing first, so this is the page
                    // closing itself (window.close()).
                    closedItself(id)
                    return
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
        openers[id] = nil
        apply()
        await registry.close(id)
    }

    /// Where `adopt` puts a page.
    public enum NewPagePlacement: Sendable {
        /// In the focused pane of the opener's window.
        case foreground
        /// In the sidebar only.
        case background
        /// Alone in a new window of the opener's space.
        case newWindow
    }

    /// A page the runtime made because another page (or an extension) asked: a new page of the
    /// opener's space, or of the focused window's, placed as asked. It keeps its opener
    /// relationship, so `window.opener` works in it.
    @discardableResult
    public func adopt(
        _ page: BDKWebContents, url: URL?, openedBy opener: BDKWebContents?,
        placement: NewPagePlacement
    ) throws -> Layout.PageID {
        let openerID = opener.flatMap { registry.id(of: $0) }
        let space =
            try openerID.flatMap { layout.spaceOf($0)?.id } ?? layout.window(focusedWindow).space
        let address = url?.absoluteString ?? page.url?.absoluteString ?? ""
        let id = try layout.addPage(url: address, title: page.title, to: space)
        registry.adopt(page, as: id)
        observe(page, id: id)
        openers[id] = openerID
        switch placement {
        case .background:
            apply()
        case .foreground:
            let window = openerID.flatMap { layout.windowShowing($0)?.id } ?? focusedWindow
            setFocusedWindow(window)
            try change { try $0.show(id, in: window) }
        case .newWindow:
            let window = try layout.addWindow(showing: space)
            setFocusedWindow(window)
            try change { try $0.showAlone(id, in: window) }
        }
        return id
    }

    /// The page closed itself (`window.close()`, as a sign-in page does when it is done): it leaves
    /// the layout, and the page that opened it comes back to its pane.
    private func closedItself(_ id: Layout.PageID) {
        guard !isQuitting, layout.pages[id] != nil else { return }
        let window = layout.windowShowing(id)?.id
        let opener = openers.removeValue(forKey: id)
        observers[id] = nil
        try? layout.close(id)
        if let window, let opener, layout.pages[opener] != nil {
            try? layout.show(opener, in: window)
        }
        apply()
        focusPage()
        registry.forget(id)
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
