import AppKit
import BDK
import MinimalBrowserCore

/// The window: a notice about the runtime on top, then the panes side by side. Its title follows the
/// focused page's title.
@MainActor
final class BrowserWindow: NSObject, NSWindowDelegate {
    static let applicationName = "Minimal Browser"

    let window: NSWindow
    let panes: [BrowserPane]
    /// Called when anything the status file reports may have changed.
    var onChange: () -> Void = {}

    private var focused: BrowserPane

    init(pages: [BDKWebContents], notice: String?) {
        panes = pages.enumerated().map { BrowserPane(index: $0.offset, page: $0.element) }
        focused = panes[0]
        window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 1200, height: 700),
            styleMask: [.titled, .closable, .miniaturizable, .resizable], backing: .buffered,
            defer: false)
        super.init()

        let row = NSStackView(views: panes)
        row.distribution = .fillEqually
        row.spacing = 12
        var rows: [NSView] = []
        if let notice {
            let label = NSTextField(wrappingLabelWithString: notice)
            label.textColor = .secondaryLabelColor
            label.alignment = .left
            label.setAccessibilityLabel("Runtime notice")
            rows.append(label)
        }
        rows.append(row)
        let content = NSStackView(views: rows)
        content.orientation = .vertical
        content.alignment = .width
        content.spacing = 8
        content.edgeInsets = NSEdgeInsets(top: 10, left: 12, bottom: 12, right: 12)
        row.setContentHuggingPriority(NSLayoutConstraint.Priority(1), for: .vertical)

        window.contentView = content
        window.minSize = NSSize(width: 720, height: 360)
        window.isReleasedWhenClosed = false
        window.delegate = self
        window.setFrameAutosaveName("MinimalBrowser")
        window.center()

        for pane in panes {
            pane.onFocus = { [weak self] in self?.focus($0) }
            pane.onChange = { [weak self] in self?.changed() }
        }
        updateTitle()
    }

    func show() {
        window.makeKeyAndOrderFront(nil)
        window.contentView?.layoutSubtreeIfNeeded()
        focus(panes[0])
        panes[0].page.view.focusContent()
        changed()
    }

    private func focus(_ pane: BrowserPane) {
        focused = pane
        updateTitle()
    }

    private func changed() {
        updateTitle()
        onChange()
    }

    private func updateTitle() {
        let title = focused.page.title
        window.title = title.isEmpty ? Self.applicationName : title
    }

    /// A PNG of the window's contents, drawn by AppKit in this process. It needs no screen-recording
    /// permission, and shows the content view only: no title bar and no engine-rendered web content.
    func snapshotPNG() -> Data? {
        guard let view = window.contentView else { return nil }
        // The window's own background is not part of the view hierarchy, so draw the view in the light
        // appearance over an opaque background and put the window's appearance back afterwards.
        let previous = window.appearance
        window.appearance = NSAppearance(named: .aqua)
        defer { window.appearance = previous }
        view.layoutSubtreeIfNeeded()
        guard
            let content = view.bitmapImageRepForCachingDisplay(in: view.bounds),
            let canvas = NSBitmapImageRep(
                bitmapDataPlanes: nil, pixelsWide: content.pixelsWide,
                pixelsHigh: content.pixelsHigh,
                bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
                colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0),
            let context = NSGraphicsContext(bitmapImageRep: canvas)
        else { return nil }
        view.cacheDisplay(in: view.bounds, to: content)
        NSGraphicsContext.saveGraphicsState()
        NSGraphicsContext.current = context
        NSColor.white.setFill()
        NSRect(x: 0, y: 0, width: canvas.pixelsWide, height: canvas.pixelsHigh).fill()
        content.draw(in: NSRect(x: 0, y: 0, width: canvas.pixelsWide, height: canvas.pixelsHigh))
        NSGraphicsContext.restoreGraphicsState()
        return canvas.representation(using: .png, properties: [:])
    }

    var status: AppStatus.Window {
        AppStatus.Window(
            title: window.title, isVisible: window.isVisible, pages: panes.map(\.status))
    }

    func windowDidResize(_ notification: Notification) {
        onChange()
    }

    func windowDidChangeOcclusionState(_ notification: Notification) {
        onChange()
    }
}
