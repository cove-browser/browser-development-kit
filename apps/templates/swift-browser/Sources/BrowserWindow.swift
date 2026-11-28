import AppKit
import BDK

/// One window, one page, an address field. The window's title follows the page's title.
@MainActor
final class BrowserWindow: NSObject, NSTextFieldDelegate {
    let window: NSWindow
    let page: BDKWebContents

    private let addressField = NSTextField()
    private var observer: Task<Void, Never>?

    init(page: BDKWebContents, notice: String?) {
        self.page = page
        window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 1100, height: 760),
            styleMask: [.titled, .closable, .miniaturizable, .resizable], backing: .buffered,
            defer: false)
        super.init()

        addressField.placeholderString = "Enter a web address"
        addressField.target = self
        addressField.action = #selector(addressEntered)
        addressField.setAccessibilityLabel("Address")

        var rows: [NSView] = []
        if let notice {
            let label = NSTextField(wrappingLabelWithString: notice)
            label.textColor = .secondaryLabelColor
            rows.append(label)
        }
        rows.append(addressField)
        let view = page.view
        view.setContentHuggingPriority(NSLayoutConstraint.Priority(1), for: .vertical)
        rows.append(view)
        let content = NSStackView(views: rows)
        content.orientation = .vertical
        content.alignment = .width
        content.spacing = 8
        content.edgeInsets = NSEdgeInsets(top: 10, left: 12, bottom: 12, right: 12)

        window.contentView = content
        window.minSize = NSSize(width: 480, height: 320)
        window.isReleasedWhenClosed = false
        window.title = "{{name}}"
        window.center()
        observe()
    }

    isolated deinit {
        observer?.cancel()
    }

    func show() {
        window.makeKeyAndOrderFront(nil)
    }

    /// Loads the URL; a failure is logged as a beep, and the page shows what the runtime made of it.
    func navigate(to url: URL) async {
        addressField.stringValue = url.absoluteString
        do {
            try await page.navigate(to: url)
        } catch {
            NSSound.beep()
        }
    }

    private func observe() {
        observer = Task { @MainActor [weak self] in
            guard let events = self?.page.events else { return }
            for await event in events {
                guard let self else { return }
                switch event {
                case .rendererGone:
                    // The renderer crashed or was killed; a browser decides what to do. This one reloads.
                    try? page.navigation.reload()
                default:
                    break
                }
                window.title = page.title.isEmpty ? "{{name}}" : page.title
                if addressField.currentEditor() == nil {
                    addressField.stringValue = page.url?.absoluteString ?? ""
                }
            }
        }
    }

    @objc private func addressEntered() {
        guard let url = AddressInput.url(from: addressField.stringValue) else {
            NSSound.beep()
            return
        }
        window.makeFirstResponder(page.view)
        Task { await navigate(to: url) }
    }
}
