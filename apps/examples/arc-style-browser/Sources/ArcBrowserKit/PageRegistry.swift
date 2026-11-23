import ArcBrowserCore
import BDK
import Foundation
import os

/// The runtime's pages for the layout's page ids, and the profile of each space. Every page of
/// every space exists for as long as the layout has it, on screen or not: a page in a hidden space
/// is a background tab of the window it was last in and keeps running (decision D1 (a)).
@MainActor
public final class PageRegistry {
    public let runtime: BDKRuntime
    public private(set) var pages: [Layout.PageID: BDKWebContents] = [:]
    private var profiles: [String: BDKProfile] = [:]
    private let log = Logger(subsystem: "dev.bdk.examples.arc-style-browser", category: "pages")

    public init(runtime: BDKRuntime) {
        self.runtime = runtime
    }

    /// The profile named `name`, opened (and created) on first use.
    public func profile(named name: String) async throws -> BDKProfile {
        if let profile = profiles[name] { return profile }
        let profile = try await runtime.profiles.open(name)
        profiles[name] = profile
        return profile
    }

    public var openProfiles: [BDKProfile] {
        profiles.keys.sorted().compactMap { profiles[$0] }
    }

    /// The page for `id`, made on first use in `profile`: restored from `state` when the runtime
    /// accepts it, otherwise loading `url`.
    @discardableResult
    public func page(
        _ id: Layout.PageID, profile name: String, url: String, state: Data?
    ) async throws -> BDKWebContents {
        if let page = pages[id] { return page }
        let profile = try await profile(named: name)
        var page: BDKWebContents?
        if let state {
            do {
                page = try profile.webContents.create(
                    .init(restoring: BDKSessionState(data: state)))
            } catch {
                // State from another engine, say: start the page again from its URL.
                log.error("page \(id) not restored: \(error)")
            }
        }
        let created = try page ?? profile.webContents.create()
        pages[id] = created
        if page == nil, let target = URL(string: url) {
            // Not awaited: a slow or failing site must not hold up the others or the window.
            let log = log
            Task { @MainActor in
                do {
                    try await created.navigate(to: target)
                } catch {
                    log.error("page \(id) could not load \(url): \(error)")
                }
            }
        }
        return created
    }

    /// A page the runtime made (a new-window request), under the layout's new id.
    public func adopt(_ page: BDKWebContents, as id: Layout.PageID) {
        pages[id] = page
    }

    /// Forgets a page that closed itself, and destroys it.
    public func forget(_ id: Layout.PageID) {
        pages.removeValue(forKey: id)?.destroy()
    }

    public func id(of page: BDKWebContents) -> Layout.PageID? {
        pages.first { $0.value === page }?.key
    }

    /// Closes the page and forgets it.
    public func close(_ id: Layout.PageID) async {
        guard let page = pages.removeValue(forKey: id) else { return }
        do {
            try await page.close()
        } catch {
            log.error("page \(id) did not close: \(error)")
        }
    }

    /// Each live page's session state, for the session file.
    public func states() -> [Layout.PageID: Data] {
        var states: [Layout.PageID: Data] = [:]
        for (id, page) in pages where page.isAlive {
            if let state = try? page.sessionState() { states[id] = state.data }
        }
        return states
    }
}
