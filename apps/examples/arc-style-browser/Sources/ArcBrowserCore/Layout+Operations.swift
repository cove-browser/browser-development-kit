import Foundation

// What a person does with the layout. Every operation keeps the invariants `validate()` checks: a
// page belongs to exactly one space, a window shows only pages of its own space, and no page is
// shown in two places at once (one view per page, swift-api.md section 7).
extension Layout {
    // MARK: Spaces and windows

    @discardableResult
    public mutating func addSpace(name: String, profile: String) -> SpaceID {
        let space = Space(name: name, profile: profile)
        spaces.append(space)
        return space.id
    }

    /// A new window showing `space`: what it showed last, or its first page no window shows.
    @discardableResult
    public mutating func addWindow(showing space: SpaceID) throws -> WindowID {
        _ = try self.space(space)
        windows.append(Window(space: space))
        let id = windows[windows.count - 1].id
        try switchSpace(of: id, to: space)
        return id
    }

    public mutating func closeWindow(_ id: WindowID) throws {
        guard windows.count > 1 else { throw Failure.lastWindow }
        let index = try windowIndex(id)
        windows.remove(at: index)
    }

    /// The window shows another space. The space it leaves remembers its panes for next time.
    public mutating func switchSpace(of window: WindowID, to space: SpaceID) throws {
        let index = try windowIndex(window)
        let target = try self.space(space)
        let elsewhere = Set(windows.filter { $0.id != window }.flatMap(\.panes))
        var panes = target.lastShown.filter {
            target.allPages.contains($0) && !elsewhere.contains($0)
        }
        if panes.isEmpty, let first = firstUnshown(in: target, except: window) {
            panes = [first]
        }
        windows[index].space = space
        windows[index].focusedPane = 0
        setPanes(Array(panes.prefix(Self.maximumPanes)), of: index)
    }

    // MARK: Pages

    /// A new page at the end of the space's list (or of its pinned pages), made in `profile`, or
    /// in the space's profile when that is nil.
    @discardableResult
    public mutating func addPage(
        url: String, title: String = "", to space: SpaceID, pinned: Bool = false,
        profile: String? = nil
    ) throws -> PageID {
        let index = try spaceIndex(space)
        let id = PageID.make()
        pages[id] = Page(url: url, title: title, profile: profile ?? spaces[index].profile)
        if pinned { spaces[index].pinned.append(id) } else { spaces[index].pages.append(id) }
        return id
    }

    public mutating func update(_ page: PageID, url: String? = nil, title: String? = nil) throws {
        guard var record = pages[page] else { throw Failure.unknownPage(page) }
        if let url { record.url = url }
        if let title { record.title = title }
        pages[page] = record
    }

    /// Forgets the page everywhere.
    public mutating func close(_ page: PageID) throws {
        guard pages[page] != nil else { throw Failure.unknownPage(page) }
        pages[page] = nil
        for index in spaces.indices {
            spaces[index].pinned.removeAll { $0 == page }
            spaces[index].pages.removeAll { $0 == page }
            spaces[index].lastShown.removeAll { $0 == page }
        }
        for index in windows.indices where windows[index].panes.contains(page) {
            var panes = windows[index].panes.filter { $0 != page }
            // A window whose last pane closed shows the space's next page no window shows.
            if panes.isEmpty, let space = try? self.space(windows[index].space),
                let next = firstUnshown(in: space, except: windows[index].id)
            {
                panes = [next]
            }
            setPanes(panes, of: index)
        }
    }

    public mutating func setPinned(_ page: PageID, _ pinned: Bool) throws {
        let index = try spaceIndex(containing: page)
        spaces[index].pinned.removeAll { $0 == page }
        spaces[index].pages.removeAll { $0 == page }
        if pinned {
            spaces[index].pinned.append(page)
        } else {
            spaces[index].pages.insert(page, at: 0)
        }
    }

    // MARK: Showing pages

    /// Shows the page in the window's focused pane, switching the window to the page's space and
    /// taking the page from any other window that shows it.
    public mutating func show(_ page: PageID, in window: WindowID) throws {
        let space = try spaceIndex(containing: page)
        var index = try windowIndex(window)
        if windows[index].space != spaces[space].id {
            try switchSpace(of: window, to: spaces[space].id)
            index = try windowIndex(window)
        }
        removeFromOtherWindows(page, keeping: window)
        var panes = windows[index].panes
        if let position = panes.firstIndex(of: page) {
            windows[index].focusedPane = position
            return
        }
        let focused = min(windows[index].focusedPane, max(panes.count - 1, 0))
        if panes.isEmpty { panes = [page] } else { panes[focused] = page }
        windows[index].focusedPane = focused
        setPanes(panes, of: index)
    }

    /// Shows the page as the window's only pane, taking it from any other window.
    public mutating func showAlone(_ page: PageID, in window: WindowID) throws {
        try show(page, in: window)
        let index = try windowIndex(window)
        windows[index].focusedPane = 0
        setPanes([page], of: index)
    }

    /// Adds the page as a new pane on the right of the window's split and focuses it. A page of
    /// another space moves into the window's space first.
    public mutating func split(_ page: PageID, in window: WindowID) throws {
        var index = try windowIndex(window)
        if windows[index].panes.contains(page) { return }
        guard windows[index].panes.count < Self.maximumPanes else {
            throw Failure.splitFull(limit: Self.maximumPanes)
        }
        if spaceOf(page)?.id != windows[index].space {
            try move(page, toSpace: windows[index].space)
            index = try windowIndex(window)
        }
        removeFromOtherWindows(page, keeping: window)
        setPanes(windows[index].panes + [page], of: index)
        windows[index].focusedPane = windows[index].panes.count - 1
    }

    /// Takes a pane out of the split; its page stays in the space.
    public mutating func closePane(_ pane: Int, in window: WindowID) throws {
        let index = try windowIndex(window)
        var panes = windows[index].panes
        guard panes.indices.contains(pane) else { throw Failure.invalid("no pane \(pane)") }
        panes.remove(at: pane)
        windows[index].focusedPane = max(0, min(windows[index].focusedPane, panes.count - 1))
        setPanes(panes, of: index)
    }

    /// Moves a pane to another position in the same split.
    public mutating func movePane(from source: Int, to destination: Int, in window: WindowID) throws
    {
        let index = try windowIndex(window)
        var panes = windows[index].panes
        guard panes.indices.contains(source), panes.indices.contains(destination) else {
            throw Failure.invalid("no pane \(source) or \(destination)")
        }
        let page = panes.remove(at: source)
        panes.insert(page, at: destination)
        windows[index].focusedPane = destination
        setPanes(panes, of: index)
    }

    public mutating func focusPane(_ pane: Int, in window: WindowID) throws {
        let index = try windowIndex(window)
        guard windows[index].panes.indices.contains(pane) else {
            throw Failure.invalid("no pane \(pane)")
        }
        windows[index].focusedPane = pane
    }

    // MARK: Moving pages

    /// Moves the page to another space. A window showing it that does not show that space stops
    /// showing it: the page is hidden, and keeps running. It keeps its profile.
    public mutating func move(_ page: PageID, toSpace space: SpaceID) throws {
        let from = try spaceIndex(containing: page)
        let to = try spaceIndex(space)
        guard from != to else { return }
        let wasPinned = spaces[from].pinned.contains(page)
        spaces[from].pinned.removeAll { $0 == page }
        spaces[from].pages.removeAll { $0 == page }
        spaces[from].lastShown.removeAll { $0 == page }
        if wasPinned { spaces[to].pinned.append(page) } else { spaces[to].pages.append(page) }
        for index in windows.indices where windows[index].panes.contains(page) {
            if windows[index].space != space {
                setPanes(windows[index].panes.filter { $0 != page }, of: index)
            }
        }
    }

    /// Moves the page to another window: into that window's space and onto a new pane of its
    /// split (or into its focused pane when the split is full).
    public mutating func move(_ page: PageID, toWindow window: WindowID) throws {
        let target = try windowIndex(window)
        try move(page, toSpace: windows[target].space)
        removeFromOtherWindows(page, keeping: window)
        if windows[target].panes.count < Self.maximumPanes {
            try split(page, in: window)
        } else {
            try show(page, in: window)
        }
    }

    // MARK: Checks

    /// Throws `invalid` for a layout no operation produces: what a damaged session file might hold.
    public func validate() throws {
        var owners: [PageID: SpaceID] = [:]
        for space in spaces {
            for page in space.allPages {
                guard pages[page] != nil else { throw Failure.invalid("\(page) has no record") }
                guard owners.updateValue(space.id, forKey: page) == nil else {
                    throw Failure.invalid("\(page) is in two spaces")
                }
            }
        }
        guard owners.count == pages.count else { throw Failure.invalid("a page is in no space") }
        try validateWindows(owners: owners)
    }

    private func validateWindows(owners: [PageID: SpaceID]) throws {
        guard !windows.isEmpty else { throw Failure.invalid("there is no window") }
        var shown = Set<PageID>()
        for window in windows {
            _ = try space(window.space)
            guard window.panes.count <= Self.maximumPanes else {
                throw Failure.invalid("\(window.id) has too many panes")
            }
            for page in window.panes {
                guard owners[page] == window.space else {
                    throw Failure.invalid("\(window.id) shows \(page) of another space")
                }
                guard shown.insert(page).inserted else {
                    throw Failure.invalid("\(page) is shown twice")
                }
            }
        }
    }

    // MARK: Private

    private func windowIndex(_ id: WindowID) throws -> Int {
        guard let index = windows.firstIndex(where: { $0.id == id }) else {
            throw Failure.unknownWindow(id)
        }
        return index
    }

    private func spaceIndex(_ id: SpaceID) throws -> Int {
        guard let index = spaces.firstIndex(where: { $0.id == id }) else {
            throw Failure.unknownSpace(id)
        }
        return index
    }

    private func spaceIndex(containing page: PageID) throws -> Int {
        guard let index = spaces.firstIndex(where: { $0.allPages.contains(page) }) else {
            throw Failure.unknownPage(page)
        }
        return index
    }

    /// The space's first page that no window other than `window` shows.
    private func firstUnshown(in space: Space, except window: WindowID) -> PageID? {
        let elsewhere = Set(windows.filter { $0.id != window }.flatMap(\.panes))
        return space.allPages.first { !elsewhere.contains($0) }
    }

    private mutating func setPanes(_ panes: [PageID], of window: Int) {
        windows[window].panes = panes
        windows[window].focusedPane = max(0, min(windows[window].focusedPane, panes.count - 1))
        if let space = spaces.firstIndex(where: { $0.id == windows[window].space }) {
            spaces[space].lastShown = panes
        }
    }

    private mutating func removeFromOtherWindows(_ page: PageID, keeping window: WindowID) {
        for index in windows.indices where windows[index].id != window {
            if windows[index].panes.contains(page) {
                setPanes(windows[index].panes.filter { $0 != page }, of: index)
            }
        }
    }
}
