import AppKit
import ArcBrowserCore
import BDK

/// One window: the sidebar, the split of up to three pages, and a side-panel container on the
/// trailing edge for an extension's panel. It holds page views; it owns no page.
@MainActor
public final class BrowserWindow: NSObject, NSWindowDelegate {
    public let id: Layout.WindowID
    public let window: NSWindow
    let sidebar = SidebarView()
    /// The panes, left to right. Each holds one page's view.
    private let paneArea = NSStackView()
    /// An extension's side panel, when the application placed one; hidden otherwise.
    let sidePanel = SidePanelContainer()
    public private(set) var registered: BDKWindow?
    private(set) var panes: [PaneView] = []
    private var focusedPane = 0

    var onPaneFocused: (Int) -> Void = { _ in }
    var onClose: () -> Void = {}
    var onBecameKey: () -> Void = {}

    init(id: Layout.WindowID, runtime: BDKRuntime, subtitle: String) {
        self.id = id
        window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 1280, height: 780),
            // Not .fullSizeContentView: the sidebar's controls and the panes' headers would sit
            // under the title bar, over the window's close, minimize and zoom buttons.
            styleMask: [.titled, .closable, .miniaturizable, .resizable],
            backing: .buffered, defer: false)
        super.init()
        window.title = "Arc-style Browser"
        window.subtitle = subtitle
        window.isReleasedWhenClosed = false
        window.minSize = NSSize(width: 820, height: 420)
        window.delegate = self

        paneArea.orientation = .horizontal
        paneArea.distribution = .fillEqually
        paneArea.spacing = 8
        let split = NSSplitView()
        split.isVertical = true
        split.dividerStyle = .thin
        split.addArrangedSubview(sidebar)
        split.addArrangedSubview(paneArea)
        split.addArrangedSubview(sidePanel)
        sidebar.widthAnchor.constraint(equalToConstant: 230).isActive = true
        sidePanel.isHidden = true
        split.setHoldingPriority(.init(260), forSubviewAt: 0)
        split.setHoldingPriority(.init(250), forSubviewAt: 1)
        split.setHoldingPriority(.init(255), forSubviewAt: 2)
        window.contentView = split
        window.center()

        // Registration is optional (swift-api.md 8.1); it gives the default presenters an anchor
        // in the sidebar and lets extensions see a normal window.
        registered = try? runtime.windows.register(window, .init(kind: .normal))
        registered?.anchors[.extensions] = { [weak self] in self?.extensionsAnchor ?? .zero }
    }

    /// The sidebar's extensions area in window coordinates.
    var extensionsAnchor: CGRect {
        sidebar.extensionsArea.convert(sidebar.extensionsArea.bounds, to: nil)
    }

    /// Puts these pages in the panes, left to right, and returns the views that left the window.
    func show(_ pages: [BDKWebContents], focused: Int) {
        let kept = panes.filter { pane in pages.contains { $0 === pane.page } }
        for pane in panes where !kept.contains(where: { $0 === pane }) {
            pane.removeFromSuperview()
        }
        panes = pages.enumerated().map { index, page in
            let pane = kept.first { $0.page === page } ?? PaneView(page: page)
            pane.onFocus = { [weak self] in
                self?.focusedPane = index
                self?.updateTitle()
                self?.onPaneFocused(index)
            }
            pane.onTitleChange = { [weak self] in self?.updateTitle() }
            return pane
        }
        focusedPane = focused
        for view in paneArea.arrangedSubviews { paneArea.removeArrangedSubview(view) }
        for (index, pane) in panes.enumerated() {
            paneArea.addArrangedSubview(pane)
            pane.isFocused = index == focused
        }
        updateTitle()
        // Lay out now, so a page's view has its size before anyone asks whether it is on screen.
        window.contentView?.layoutSubtreeIfNeeded()
    }

    /// The window's title is the focused pane's: its page's title, or its address before it has one.
    private func updateTitle() {
        let title = panes.indices.contains(focusedPane) ? panes[focusedPane].title : ""
        window.title = title.isEmpty ? "Arc-style Browser" : title
    }

    /// Gives the focused pane's page the keyboard, when this is the key window.
    func focusPage(at index: Int) {
        guard panes.indices.contains(index) else { return }
        panes[index].page.view.focusContent()
    }

    /// Brings the window to the front, or with `background` puts it behind every other window
    /// without making the application active.
    func showWindow(background: Bool) {
        if background {
            window.orderBack(nil)
        } else {
            window.makeKeyAndOrderFront(nil)
        }
    }

    public func windowDidBecomeKey(_ notification: Notification) {
        onBecameKey()
    }

    public func windowWillClose(_ notification: Notification) {
        onClose()
    }
}

/// One pane of the split: a header with the page's title over the page's view.
@MainActor
final class PaneView: NSStackView {
    let page: BDKWebContents
    var onFocus: () -> Void = {}
    /// Called when the header's text changes.
    var onTitleChange: () -> Void = {}
    var isFocused = false {
        didSet { header.textColor = isFocused ? .labelColor : .secondaryLabelColor }
    }
    private let header = NSTextField(labelWithString: "")
    private var observer: Task<Void, Never>?

    init(page: BDKWebContents) {
        self.page = page
        super.init(frame: .zero)
        orientation = .vertical
        alignment = .width
        spacing = 4
        header.font = .systemFont(ofSize: 11, weight: .medium)
        header.lineBreakMode = .byTruncatingTail
        let view = page.view
        view.setContentHuggingPriority(.init(1), for: .vertical)
        view.setContentHuggingPriority(.init(1), for: .horizontal)
        view.wantsLayer = true
        view.layer?.cornerRadius = 6
        view.layer?.masksToBounds = true
        addArrangedSubview(header)
        addArrangedSubview(view)
        let click = NSClickGestureRecognizer(target: self, action: #selector(clicked))
        header.addGestureRecognizer(click)
        refresh()
        // Subscribed now, not when the task first runs: a new page's first title can arrive in
        // between. The iterator only moves from the main actor to this main-actor task.
        nonisolated(unsafe) var iterator = page.events.makeAsyncIterator()
        observer = Task { @MainActor [weak self] in
            while let event = await iterator.next() {
                if case .focusChanged(true) = event { self?.onFocus() }
                self?.refresh()
            }
        }
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        nil
    }

    isolated deinit {
        observer?.cancel()
    }

    /// The page's title, or its address before it has one.
    var title: String {
        header.stringValue
    }

    private func refresh() {
        guard page.isAlive else { return }
        let text = page.title.isEmpty ? (page.url?.absoluteString ?? "") : page.title
        guard text != header.stringValue else { return }
        header.stringValue = text
        onTitleChange()
    }

    @objc private func clicked() {
        page.view.focusContent()
        onFocus()
    }
}

/// Where the application puts an extension's side panel: the panel's secure view under a header
/// with its title and a close button.
@MainActor
final class SidePanelContainer: NSStackView {
    private let title = NSTextField(labelWithString: "")
    private let close = NSButton(title: "Close", target: nil, action: nil)
    private var content: NSView?
    var onClose: () -> Void = {}

    init() {
        super.init(frame: .zero)
        orientation = .vertical
        alignment = .width
        spacing = 6
        edgeInsets = NSEdgeInsets(top: 10, left: 8, bottom: 10, right: 8)
        close.target = self
        close.action = #selector(closeClicked)
        close.bezelStyle = .texturedRounded
        title.font = .systemFont(ofSize: 12, weight: .semibold)
        addArrangedSubview(NSStackView(views: [title, close]))
        widthAnchor.constraint(greaterThanOrEqualToConstant: 280).isActive = true
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        nil
    }

    /// The view the container shows, nil when it is empty.
    var shownView: NSView? { content }

    func show(_ view: NSView, title text: String) {
        content?.removeFromSuperview()
        content = view
        title.stringValue = text
        view.setContentHuggingPriority(.init(1), for: .vertical)
        addArrangedSubview(view)
        isHidden = false
    }

    func clear() {
        content?.removeFromSuperview()
        content = nil
        isHidden = true
    }

    @objc private func closeClicked() {
        onClose()
    }
}
