import AppKit
import ArcBrowserCore
import BDK

// The status report: what the layout says, what is on screen, and what the runtime reports.
extension BrowserController {
    public func status(
        phase: AppStatus.Phase, runtimeName: String, actionsDone: Bool, errors: [String] = []
    ) -> AppStatus {
        var pages: [AppStatus.Page] = []
        for space in layout.spaces {
            for id in space.allPages {
                pages.append(pageStatus(id, in: space))
            }
        }
        let windows = layout.windows.map { window in
            AppStatus.Window(
                space: (try? layout.space(window.space).name) ?? "", panes: window.panes.count,
                isVisible: self.windows[window.id]?.window.isVisible == true)
        }
        return AppStatus(
            phase: phase, runtime: runtimeName, restored: restored, actionsDone: actionsDone,
            windows: windows, pages: pages,
            settingsURL: settingsPage.flatMap { registry.pages[$0]?.url?.absoluteString },
            automationResult: automationResult, errors: errors)
    }

    private func pageStatus(_ id: Layout.PageID, in space: Layout.Space) -> AppStatus.Page {
        let page = registry.pages[id]
        let window = layout.windows.firstIndex { $0.panes.contains(id) }
        let view = page?.view
        let onScreen =
            view.map {
                $0.window?.isVisible == true && !$0.isHiddenOrHasHiddenAncestor
                    && $0.bounds.width > 0 && $0.bounds.height > 0
            } ?? false
        let state = page.flatMap { try? $0.state() }
        return AppStatus.Page(
            id: id.rawValue, space: space.name, pinned: space.pinned.contains(id),
            url: page?.url?.absoluteString ?? layout.pages[id]?.url ?? "",
            title: page?.title ?? layout.pages[id]?.title ?? "",
            window: window, pane: window.flatMap { layout.windows[$0].panes.firstIndex(of: id) },
            onScreen: onScreen, runtimeVisible: state?.visibility == .visible,
            entries: (try? page?.navigation.entries.count) ?? 0)
    }
}
