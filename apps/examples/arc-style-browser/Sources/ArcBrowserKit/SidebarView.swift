import AppKit
import ArcBrowserCore

/// The window's sidebar: the spaces, then the shown space's pinned pages and other pages, an
/// address field for a new page, and the extensions area where actions and their popups go.
/// Application code only: BDK has no sidebar, space or tab type.
@MainActor
final class SidebarView: NSStackView, NSTextFieldDelegate {
    struct Row: Equatable {
        let id: Layout.PageID
        let title: String
        let isShown: Bool
    }

    /// What a person did in the sidebar.
    enum Request {
        case selectSpace(Layout.SpaceID)
        case show(Layout.PageID)
        case split(Layout.PageID)
        case pin(Layout.PageID, Bool)
        case moveToSpace(Layout.PageID, Layout.SpaceID)
        case moveToNewWindow(Layout.PageID)
        case close(Layout.PageID)
        case open(String)
        /// `click` is the event that clicked the button, taken when it fired: the request is
        /// handled a moment later, when `NSApp.currentEvent` may already be another one.
        case extensionAction(id: String, button: NSButton, click: NSEvent?)
    }

    var onRequest: (Request) -> Void = { _ in }

    private let spaces = NSSegmentedControl()
    private let pinned = NSStackView()
    private let pages = NSStackView()
    private let address = NSTextField()
    /// Where extension action buttons go; popups attach here (the window's `extensions` anchor).
    let extensionsArea = NSStackView()
    private var spaceIDs: [Layout.SpaceID] = []
    private var otherSpaces: [(Layout.SpaceID, String)] = []

    init() {
        super.init(frame: .zero)
        orientation = .vertical
        alignment = .leading
        spacing = 8
        edgeInsets = NSEdgeInsets(top: 12, left: 10, bottom: 12, right: 10)
        spaces.segmentStyle = .rounded
        spaces.target = self
        spaces.action = #selector(spaceSelected)
        spaces.setAccessibilityLabel("Spaces")
        address.placeholderString = "New page: type an address"
        address.target = self
        address.action = #selector(addressEntered)
        address.setAccessibilityLabel("New page address")
        for list in [pinned, pages] {
            list.orientation = .vertical
            list.alignment = .leading
            list.spacing = 2
        }
        extensionsArea.orientation = .horizontal
        extensionsArea.spacing = 4
        extensionsArea.setAccessibilityLabel("Extensions")
        let fill = NSView()
        fill.setContentHuggingPriority(.init(1), for: .vertical)
        for view in [
            spaces, Self.heading("Pinned"), pinned, Self.heading("Pages"), pages, address, fill,
            Self.heading("Extensions"), extensionsArea,
        ] {
            addArrangedSubview(view)
        }
        address.widthAnchor.constraint(equalTo: widthAnchor, constant: -20).isActive = true
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        nil
    }

    private static func heading(_ text: String) -> NSTextField {
        let label = NSTextField(labelWithString: text.uppercased())
        label.font = .systemFont(ofSize: 10, weight: .semibold)
        label.textColor = .tertiaryLabelColor
        return label
    }

    /// Shows `space` of `layout` with these rows.
    func update(layout: Layout, space: Layout.Space, pinnedRows: [Row], pageRows: [Row]) {
        spaceIDs = layout.spaces.map(\.id)
        otherSpaces = layout.spaces.filter { $0.id != space.id }.map { ($0.id, $0.name) }
        spaces.segmentCount = layout.spaces.count
        for (index, each) in layout.spaces.enumerated() {
            spaces.setLabel(each.name, forSegment: index)
            spaces.setWidth(0, forSegment: index)
        }
        spaces.selectedSegment = spaceIDs.firstIndex(of: space.id) ?? -1
        fill(pinned, with: pinnedRows, pinned: true)
        fill(pages, with: pageRows, pinned: false)
    }

    private func fill(_ list: NSStackView, with rows: [Row], pinned: Bool) {
        for view in list.arrangedSubviews { view.removeFromSuperview() }
        for row in rows {
            let button = PageButton(row: row, pinned: pinned)
            button.target = self
            button.action = #selector(rowClicked(_:))
            button.menu = menu(for: row, pinned: pinned)
            button.setContentCompressionResistancePriority(.init(1), for: .horizontal)
            let close = RowCloseButton(
                page: row.id, target: self, action: #selector(closeClicked(_:)))
            close.setAccessibilityLabel("Close \(button.title)")
            let line = NSStackView(views: [button, close])
            line.spacing = 2
            list.addArrangedSubview(line)
            line.widthAnchor.constraint(equalTo: list.widthAnchor).isActive = true
        }
    }

    private func menu(for row: Row, pinned: Bool) -> NSMenu {
        let menu = NSMenu()
        func item(_ title: String, _ request: Request) {
            let item = NSMenuItem(
                title: title, action: #selector(menuChosen(_:)), keyEquivalent: "")
            item.target = self
            item.representedObject = RequestBox(request)
            menu.addItem(item)
        }
        item("Show", .show(row.id))
        item("Open in Split View", .split(row.id))
        item(pinned ? "Unpin" : "Pin", .pin(row.id, !pinned))
        for (space, name) in otherSpaces { item("Move to \(name)", .moveToSpace(row.id, space)) }
        item("Move to New Window", .moveToNewWindow(row.id))
        menu.addItem(.separator())
        item("Close", .close(row.id))
        return menu
    }

    /// An extension's action as the sidebar shows it.
    struct ExtensionAction {
        let id: String
        let title: String
        let icon: NSImage?
    }

    /// One button per extension action, in the order given.
    func setExtensionActions(_ actions: [ExtensionAction]) {
        for view in extensionsArea.arrangedSubviews { view.removeFromSuperview() }
        for action in actions {
            let image =
                action.icon
                ?? NSImage(systemSymbolName: "puzzlepiece", accessibilityDescription: nil)
            let button = NSButton(
                image: image ?? NSImage(), target: self, action: #selector(actionClicked(_:)))
            button.bezelStyle = .texturedRounded
            button.toolTip = action.title
            button.identifier = NSUserInterfaceItemIdentifier(action.id)
            button.setAccessibilityLabel(action.title)
            extensionsArea.addArrangedSubview(button)
        }
    }

    /// The extension's action button, while the extensions area shows one.
    func extensionButton(for id: String) -> NSView? {
        extensionsArea.arrangedSubviews.first { $0.identifier?.rawValue == id }
    }

    @objc private func spaceSelected() {
        let index = spaces.selectedSegment
        guard spaceIDs.indices.contains(index) else { return }
        onRequest(.selectSpace(spaceIDs[index]))
    }

    @objc private func rowClicked(_ sender: PageButton) {
        // Option-click opens the page beside the others, as split view.
        let split = NSApp.currentEvent?.modifierFlags.contains(.option) == true
        onRequest(split ? .split(sender.row.id) : .show(sender.row.id))
    }

    @objc private func closeClicked(_ sender: RowCloseButton) {
        onRequest(.close(sender.page))
    }

    @objc private func menuChosen(_ sender: NSMenuItem) {
        if let box = sender.representedObject as? RequestBox { onRequest(box.request) }
    }

    @objc private func addressEntered() {
        let text = address.stringValue.trimmingCharacters(in: .whitespaces)
        guard !text.isEmpty else { return }
        address.stringValue = ""
        onRequest(.open(text))
    }

    @objc private func actionClicked(_ sender: NSButton) {
        guard let id = sender.identifier?.rawValue else { return }
        onRequest(.extensionAction(id: id, button: sender, click: NSApp.currentEvent))
    }
}

/// A sidebar row.
@MainActor
private final class PageButton: NSButton {
    let row: SidebarView.Row

    init(row: SidebarView.Row, pinned: Bool) {
        self.row = row
        super.init(frame: .zero)
        title = (pinned ? "📌 " : "") + (row.title.isEmpty ? "New page" : row.title)
        bezelStyle = .recessed
        showsBorderOnlyWhileMouseInside = !row.isShown
        lineBreakMode = .byTruncatingTail
        alignment = .left
        font = .systemFont(ofSize: 12, weight: row.isShown ? .semibold : .regular)
        setAccessibilityLabel(title)
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        nil
    }
}

/// Carries a request through `NSMenuItem.representedObject`.
private final class RequestBox: NSObject {
    let request: SidebarView.Request

    init(_ request: SidebarView.Request) {
        self.request = request
    }
}

/// The × at the end of a sidebar row, which closes that page.
@MainActor
private final class RowCloseButton: NSButton {
    let page: Layout.PageID

    init(page: Layout.PageID, target: AnyObject, action: Selector) {
        self.page = page
        super.init(frame: .zero)
        image = NSImage(systemSymbolName: "xmark", accessibilityDescription: nil)
        imageScaling = .scaleProportionallyDown
        bezelStyle = .recessed
        showsBorderOnlyWhileMouseInside = true
        toolTip = "Close Page"
        self.target = target
        self.action = action
        setContentHuggingPriority(.required, for: .horizontal)
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        nil
    }
}
