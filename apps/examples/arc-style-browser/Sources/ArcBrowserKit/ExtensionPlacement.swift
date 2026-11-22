import AppKit
import ArcBrowserCore
import BDK

/// Puts extension popups and side panels in the application's layout instead of BDK's defaults
/// (M8, "Extensions in the layout"; extensions.md 4.2 and 4.3): a popup in a popover beside the
/// sidebar button that opened it, a side panel in the window's trailing container. BDK keeps the
/// secure view, focus and close behaviour; the application only chooses the container.
@MainActor
public final class ExtensionPlacement {
    /// A popup on screen, for the status report and the tests.
    @MainActor
    public struct Popup {
        public let extensionID: String
        public let secureView: NSView
        public let window: Layout.WindowID
        public var isShown: Bool { popover.isShown }
        let request: ExtensionPopupRequest
        let popover: NSPopover
        let delegate: PopoverDelegate
    }

    weak var controller: BrowserController?
    public private(set) var popups: [Popup] = []
    public private(set) var sidePanels: [Layout.WindowID: ExtensionSidePanelRequest] = [:]
    private var anchors: [Layout.WindowID: NSView] = [:]
    private var sizeObservers: [ObjectIdentifier: Task<Void, Never>] = [:]

    public init() {}

    /// Registers the handlers. Before the runtime starts: a surface gains no handler after.
    public func install(in configuration: BDKConfiguration) throws {
        try configuration.ui.extensionPopups.setHandler { [weak self] request in
            self?.place(request) ?? .useDefault
        }
        try configuration.ui.extensionSidePanels.setHandler { [weak self] request in
            self?.place(request) ?? .useDefault
        }
    }

    /// The button a person clicked, where that window's next popup attaches.
    func noteAnchor(_ view: NSView, in window: Layout.WindowID) {
        anchors[window] = view
    }

    // MARK: Popups

    func place(_ request: ExtensionPopupRequest) -> BDKUIResponse {
        guard let (id, browserWindow) = browserWindow(for: request), browserWindow.window.isVisible
        else { return .useDefault }
        let anchor = anchors[id] ?? browserWindow.sidebar.extensionsArea
        let content = NSViewController()
        let container = NSView(
            frame: NSRect(origin: .zero, size: Self.clamp(request.preferredSize)))
        request.secureView.frame = container.bounds
        request.secureView.autoresizingMask = [.width, .height]
        container.addSubview(request.secureView)
        content.view = container
        let popover = NSPopover()
        popover.behavior = .transient
        popover.contentViewController = content
        popover.contentSize = container.frame.size
        let delegate = PopoverDelegate { [weak self, weak request] in
            // The person dismissed it: tell BDK, which closes the extension's view.
            guard let self, let request else { return }
            if !request.closed.isFired { request.close() }
            self.forget(request)
        }
        popover.delegate = delegate
        popover.animates = false
        popups.append(
            Popup(
                extensionID: request.extensionID, secureView: request.secureView, window: id,
                request: request, popover: popover, delegate: delegate))
        // An empty extensions area (no action shown yet) has no size to point at: use the sidebar.
        let target = anchor.bounds.isEmpty ? browserWindow.sidebar : anchor
        browserWindow.window.contentView?.layoutSubtreeIfNeeded()
        popover.show(relativeTo: target.bounds, of: target, preferredEdge: .maxX)
        request.accept()
        request.closed.onFire { [weak self, weak request, weak popover] in
            // The extension, a navigation or BDK withdrew it.
            popover?.close()
            if let self, let request { self.forget(request) }
        }
        let sizes = request.sizeChanges
        sizeObservers[ObjectIdentifier(request)] = Task { @MainActor [weak popover] in
            for await size in sizes { popover?.contentSize = Self.clamp(size) }
        }
        controller?.onChange()
        return .handled
    }

    private func forget(_ request: ExtensionPopupRequest) {
        popups.removeAll { $0.request === request }
        sizeObservers.removeValue(forKey: ObjectIdentifier(request))?.cancel()
        controller?.onChange()
    }

    /// Chromium's popup bounds: at least 25 by 25, at most 800 by 600; 320 by 240 before the
    /// popup has said.
    static func clamp(_ size: CGSize) -> CGSize {
        guard size.width > 0, size.height > 0 else { return CGSize(width: 320, height: 240) }
        return CGSize(width: min(max(size.width, 25), 800), height: min(max(size.height, 25), 600))
    }

    // MARK: Side panels

    func place(_ request: ExtensionSidePanelRequest) -> BDKUIResponse {
        guard let (id, browserWindow) = browserWindow(for: request) else { return .useDefault }
        if let previous = sidePanels[id], !previous.closed.isFired { previous.close() }
        sidePanels[id] = request
        browserWindow.sidePanel.show(request.secureView, title: request.title)
        request.accept(side: .trailing)
        request.closed.onFire { [weak self, weak browserWindow, weak request] in
            guard let self, let request, self.sidePanels[id] === request else { return }
            self.sidePanels[id] = nil
            browserWindow?.sidePanel.clear()
            self.controller?.onChange()
        }
        let visibility = request.visibilityChanges
        Task { @MainActor [weak browserWindow] in
            // A page's own panel goes while another page is the active tab.
            for await visible in visibility { browserWindow?.sidePanel.isHidden = !visible }
        }
        controller?.onChange()
        return .handled
    }

    /// The window's close button for its panel: BDK closes the extension's view, and the
    /// container empties. Completing the request is not a withdrawal, so `closed` does not fire.
    func closeSidePanel(in window: Layout.WindowID) {
        guard let request = sidePanels.removeValue(forKey: window) else { return }
        request.close()
        controller?.windows[window]?.sidePanel.clear()
        controller?.onChange()
    }

    // MARK: Windows

    /// The window the request is for: the one it names, else the one showing its page, else the
    /// focused one.
    private func browserWindow(for request: BDKUIRequest) -> (Layout.WindowID, BrowserWindow)? {
        guard let controller else { return nil }
        if let native = request.window,
            let match = controller.windows.first(where: { $0.value.window === native })
        {
            return (match.key, match.value)
        }
        if let page = request.contents, let pageID = controller.registry.id(of: page),
            let window = controller.layout.windowShowing(pageID),
            let browserWindow = controller.windows[window.id]
        {
            return (window.id, browserWindow)
        }
        let id = controller.focusedWindow
        return controller.windows[id].map { (id, $0) }
    }
}

/// Reports a popover the person closed.
final class PopoverDelegate: NSObject, NSPopoverDelegate {
    private let closed: @MainActor () -> Void

    init(closed: @escaping @MainActor () -> Void) {
        self.closed = closed
    }

    func popoverDidClose(_ notification: Notification) {
        MainActor.assumeIsolated { closed() }
    }
}
