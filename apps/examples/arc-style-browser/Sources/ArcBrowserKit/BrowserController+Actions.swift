import AppKit
import ArcBrowserCore
import BDK

// What a person does: the sidebar's requests, the menus, and the `--do` actions the tests use.
// Each one is a layout operation; none calls BDK beyond creating, closing and focusing pages.
extension BrowserController {
    public enum ActionFailure: Error, Equatable {
        case noSuchSpace(String)
        case noSuchWindow(Int)
        case noFocusedPage
        case nothingToSplit
    }

    /// The bundled settings page, at the branded URL from `bdk.toml` (`[browser] internal_scheme`).
    public static let settingsURL = URL(string: "arc://settings")

    func handle(_ request: SidebarView.Request, from window: Layout.WindowID) {
        setFocusedWindow(window)
        Task { @MainActor in
            do {
                try await perform(request, in: window)
            } catch {
                log.error("\(String(describing: request)) failed: \(error)")
                NSSound.beep()
            }
        }
    }

    private func perform(_ request: SidebarView.Request, in window: Layout.WindowID) async throws {
        switch request {
        case .selectSpace(let space): try change { try $0.switchSpace(of: window, to: space) }
        case .show(let page): try change { try $0.show(page, in: window) }
        case .split(let page): try change { try $0.split(page, in: window) }
        case .pin(let page, let pinned): try change { try $0.setPinned(page, pinned) }
        case .moveToSpace(let page, let space): try change { try $0.move(page, toSpace: space) }
        case .moveToNewWindow(let page): try moveToNewWindow(page)
        case .close(let page): try await close(page)
        case .open(let text):
            guard let url = AddressInput.url(from: text) else { throw ActionFailure.noFocusedPage }
            try await open(url)
        case .extensionAction(let id, let button):
            try activateExtension(id, from: button, in: window)
        }
    }

    /// Runs one `--do` action.
    public func perform(_ action: AppOptions.Action) async throws {
        switch action {
        case .open(let text):
            guard let url = AddressInput.url(from: text) else { throw ActionFailure.noFocusedPage }
            try await open(url)
        case .settings: try await openSettings()
        case .automation: _ = try await AutomationRunner(controller: self).run()
        case .installExtension(let path): try await installExtension(URL(fileURLWithPath: path))
        case .closePage: try await close(try focusedPage())
        case .newWindow, .moveToWindow, .focusWindow: try performWindowAction(action)
        default: try performLayoutAction(action)
        }
    }

    private func performLayoutAction(_ action: AppOptions.Action) throws {
        switch action {
        case .splitNext: try splitNext()
        case .switchSpace(let name):
            let space = try space(named: name)
            try change { try $0.switchSpace(of: focusedWindow, to: space) }
        case .moveToSpace(let name):
            let space = try space(named: name)
            let page = try focusedPage()
            try change { try $0.move(page, toSpace: space) }
        case .pin:
            let page = try focusedPage()
            try change { try $0.setPinned(page, true) }
        default: preconditionFailure("not a layout action: \(action)")
        }
    }

    private func performWindowAction(_ action: AppOptions.Action) throws {
        switch action {
        case .newWindow(let name):
            let space = try space(named: name)
            let window = try layout.addWindow(showing: space)
            try change { _ in }
            setFocusedWindow(window)
        case .moveToWindow(let index):
            let page = try focusedPage()
            let target = try window(at: index)
            try change { try $0.move(page, toWindow: target) }
        case .focusWindow(let index):
            setFocusedWindow(try window(at: index))
            windows[focusedWindow]?.showWindow(background: showsWindowsInBackground)
        default: preconditionFailure("not a window action: \(action)")
        }
    }

    /// Adds the next page of the focused window's space that no window shows to its split.
    public func splitNext() throws {
        let window = try layout.window(focusedWindow)
        let space = try layout.space(window.space)
        let visible = layout.visiblePages
        guard let next = space.allPages.first(where: { !visible.contains($0) }) else {
            throw ActionFailure.nothingToSplit
        }
        try change { try $0.split(next, in: focusedWindow) }
    }

    /// Opens a window showing the page's space, with the page in it.
    public func moveToNewWindow(_ page: Layout.PageID) throws {
        guard let space = layout.spaceOf(page) else { throw ActionFailure.noFocusedPage }
        let window = try layout.addWindow(showing: space.id)
        try change { try $0.showAlone(page, in: window) }
        setFocusedWindow(window)
    }

    /// Opens `arc://settings` in the focused window's space.
    public func openSettings() async throws {
        guard let url = Self.settingsURL else { return }
        settingsPage = try await open(url)
    }

    public func space(named name: String) throws -> Layout.SpaceID {
        guard let space = layout.spaces.first(where: { $0.name == name }) else {
            throw ActionFailure.noSuchSpace(name)
        }
        return space.id
    }

    func window(at index: Int) throws -> Layout.WindowID {
        guard layout.windows.indices.contains(index) else {
            throw ActionFailure.noSuchWindow(index)
        }
        return layout.windows[index].id
    }

    public func focusedPage() throws -> Layout.PageID {
        guard let page = try layout.window(focusedWindow).focusedPage else {
            throw ActionFailure.noFocusedPage
        }
        return page
    }

    // MARK: History

    /// What the Go menu does to the focused page.
    public enum HistoryCommand: Sendable {
        case reload
        case back
        case forward
    }

    /// Reloads the focused page, or moves it through its history. A page whose renderer went away
    /// comes back by reloading. Going back or forward with nowhere to go does nothing.
    public func perform(_ command: HistoryCommand) throws {
        guard let page = registry.pages[try focusedPage()] else {
            throw ActionFailure.noFocusedPage
        }
        let navigation = page.navigation
        switch command {
        case .reload: try navigation.reload()
        case .back: if try navigation.canGoBack { try navigation.goBack() }
        case .forward: if try navigation.canGoForward { try navigation.goForward() }
        }
    }

    // MARK: Extensions

    /// Installs an unpacked extension in the focused space's profile. BDK's install prompt asks
    /// the person first.
    public func installExtension(_ directory: URL) async throws {
        let space = try layout.space(try layout.window(focusedWindow).space)
        let profile = try await registry.profile(named: space.profile)
        _ = try await profile.extensions.install(.unpacked(directory))
        await refreshExtensionActions()
    }

    /// The extension's action for the focused page of the window, clicked from the sidebar: a
    /// popup or side panel it opens arrives as a request that ExtensionPlacement puts in this
    /// window.
    private func activateExtension(
        _ id: String, from button: NSButton, in window: Layout.WindowID
    ) throws {
        guard let event = NSApp.currentEvent,
            let pageID = try layout.window(window).focusedPage,
            let page = registry.pages[pageID]
        else { return }
        extensions.noteAnchor(button, in: window)
        _ = try page.profile.extensions.activateAction(id, on: page, confirmedBy: event)
    }

    /// Fills each window's extensions area with the actions of its focused page's profile.
    public func refreshExtensionActions() async {
        for window in layout.windows {
            guard let pageID = window.focusedPage, let page = registry.pages[pageID],
                let actions = try? await page.profile.extensions.actions(for: page)
            else { continue }
            windows[window.id]?.sidebar.setExtensionActions(
                actions.map { .init(id: $0.id, title: $0.title, icon: $0.iconImage) })
        }
    }
}
