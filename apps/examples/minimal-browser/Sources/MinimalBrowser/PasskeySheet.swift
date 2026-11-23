import AppKit
import BDK
import MinimalBrowserKit

/// Draws a passkey ceremony as a sheet on the page's window: the step's title and message, its error or
/// hint, a spinner while the authenticator works, the phone's QR code, a PIN field, one button per choice and
/// the step's own buttons, from `WebAuthnRequest.presentation`.
@MainActor
final class PasskeySheet: PasskeyDisplay {
    private final class Sheet {
        let panel: NSPanel
        let parent: NSWindow
        init(panel: NSPanel, parent: NSWindow) {
            self.panel = panel
            self.parent = parent
        }
    }

    /// The window a request belongs to when the runtime names none.
    var fallbackWindow: () -> NSWindow? = { NSApp.keyWindow ?? NSApp.mainWindow }

    private var sheets: [String: Sheet] = [:]

    func show(_ request: WebAuthnRequest, presenter: PasskeyPresenter) {
        let ceremony = request.ceremonyID
        if sheets[ceremony] == nil {
            guard let parent = request.window ?? fallbackWindow() else {
                // Nowhere to ask: the ceremony ends, as a closed sheet would end it.
                request.cancel()
                return
            }
            let panel = NSPanel(
                contentRect: NSRect(x: 0, y: 0, width: 380, height: 200), styleMask: [.titled],
                backing: .buffered, defer: true)
            panel.setAccessibilityLabel("Passkey")
            sheets[ceremony] = Sheet(panel: panel, parent: parent)
            parent.beginSheet(panel)
        }
        guard let sheet = sheets[ceremony] else { return }
        sheet.panel.contentView = content(for: request, ceremony: ceremony, presenter: presenter)
        sheet.panel.setContentSize(
            sheet.panel.contentView?.fittingSize ?? NSSize(width: 380, height: 200))
    }

    func close(ceremony: String) {
        guard let sheet = sheets.removeValue(forKey: ceremony) else { return }
        sheet.parent.endSheet(sheet.panel)
    }

    private func content(
        for request: WebAuthnRequest, ceremony: String, presenter: PasskeyPresenter
    ) -> NSView {
        let step = request.presentation
        var rows = textRows(step)
        if let image = request.qrCodeImage(scale: 4) {
            let view = NSImageView(image: image)
            view.setAccessibilityLabel("QR code for your phone")
            rows.append(view)
        }
        var pinField: NSSecureTextField?
        if step.pin != nil {
            let field = NSSecureTextField()
            field.placeholderString = "PIN"
            field.setAccessibilityLabel("PIN")
            pinField = field
            rows.append(field)
        }
        for choice in step.choices {
            rows.append(
                ActionButton(title: choice.title) { [weak presenter] in
                    try? presenter?.perform(choice, in: ceremony, confirmedBy: NSApp.currentEvent)
                })
        }
        rows.append(
            buttonRow(step.actions) { [weak presenter] action in
                try? presenter?.perform(action, in: ceremony, pin: pinField?.stringValue)
            })
        let stack = NSStackView(views: rows)
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 10
        stack.edgeInsets = NSEdgeInsets(top: 20, left: 20, bottom: 20, right: 20)
        stack.widthAnchor.constraint(equalToConstant: 380).isActive = true
        return stack
    }

    /// The title, the message, the error and hint, and a spinner while the authenticator works.
    private func textRows(_ step: WebAuthnRequest.Presentation) -> [NSView] {
        var rows: [NSView] = []
        let title = NSTextField(wrappingLabelWithString: step.title)
        title.font = .boldSystemFont(ofSize: NSFont.systemFontSize + 1)
        rows.append(title)
        if !step.message.isEmpty { rows.append(NSTextField(wrappingLabelWithString: step.message)) }
        for (text, color) in [
            (step.errorMessage, NSColor.systemRed), (step.hint, .secondaryLabelColor),
        ] {
            guard let text, !text.isEmpty else { continue }
            let label = NSTextField(wrappingLabelWithString: text)
            label.textColor = color
            rows.append(label)
        }
        if step.showsProgress {
            let spinner = NSProgressIndicator()
            spinner.style = .spinning
            spinner.controlSize = .small
            spinner.startAnimation(nil)
            rows.append(spinner)
        }
        return rows
    }

    /// The step's own buttons, in Chromium's order: Return presses accept, Escape cancels.
    private func buttonRow(
        _ actions: [WebAuthnRequest.Presentation.Action],
        perform: @escaping (WebAuthnRequest.Presentation.Action) -> Void
    ) -> NSView {
        let buttons = actions.map { action -> NSButton in
            let label: String
            switch action {
            case .accept(let text), .otherMechanisms(let text), .cancel(let text): label = text
            }
            let button = ActionButton(title: label) { perform(action) }
            if case .cancel = action { button.keyEquivalent = "\u{1b}" }
            if case .accept = action { button.keyEquivalent = "\r" }
            return button
        }
        let row = NSStackView(views: buttons)
        row.orientation = .horizontal
        return row
    }
}

/// A push button that runs a closure.
private final class ActionButton: NSButton {
    private var handler: () -> Void = {}

    convenience init(title: String, action: @escaping () -> Void) {
        self.init(frame: .zero)
        self.title = title
        bezelStyle = .push
        handler = action
        target = self
        self.action = #selector(run)
    }

    @objc private func run() {
        handler()
    }
}
