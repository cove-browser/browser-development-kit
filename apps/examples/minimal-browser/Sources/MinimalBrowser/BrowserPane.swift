import AppKit
import BDK
import MinimalBrowserCore
import os

/// One page with its own controls: back, forward, reload and an address field above the page's view.
@MainActor
final class BrowserPane: NSStackView, NSTextFieldDelegate {
    let index: Int
    let page: BDKWebContents
    /// Called when the person uses this pane or the page takes focus.
    var onFocus: (BrowserPane) -> Void = { _ in }
    /// Called after the page's state changed and the controls were refreshed.
    var onChange: () -> Void = {}

    private let backButton = BrowserPane.button("chevron.left", "Back")
    private let forwardButton = BrowserPane.button("chevron.right", "Forward")
    private let reloadButton = BrowserPane.button("arrow.clockwise", "Reload")
    private let addressField = NSTextField()
    private var observer: Task<Void, Never>?
    private let log = Logger(subsystem: "dev.bdk.examples.minimal-browser", category: "pane")

    init(index: Int, page: BDKWebContents) {
        self.index = index
        self.page = page
        super.init(frame: .zero)
        orientation = .vertical
        alignment = .width
        spacing = 6

        addressField.placeholderString = "Enter a web address"
        addressField.delegate = self
        addressField.target = self
        addressField.action = #selector(addressEntered)
        addressField.setAccessibilityLabel("Address, page \(index + 1)")
        for (button, action) in [
            (backButton, #selector(goBack)), (forwardButton, #selector(goForward)),
            (reloadButton, #selector(reload)),
        ] {
            button.target = self
            button.action = action
        }
        let toolbar = NSStackView(views: [backButton, forwardButton, reloadButton, addressField])
        toolbar.spacing = 6
        addressField.setContentHuggingPriority(.defaultLow, for: .horizontal)

        let view = page.view
        view.setContentHuggingPriority(NSLayoutConstraint.Priority(1), for: .vertical)
        view.setContentHuggingPriority(NSLayoutConstraint.Priority(1), for: .horizontal)
        view.wantsLayer = true
        view.layer?.borderWidth = 1
        view.layer?.borderColor = NSColor.separatorColor.cgColor
        view.setAccessibilityLabel("Page \(index + 1)")
        addArrangedSubview(toolbar)
        addArrangedSubview(view)

        refresh()
        observe()
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        nil
    }

    isolated deinit {
        observer?.cancel()
    }

    private static func button(_ symbol: String, _ label: String) -> NSButton {
        let image = NSImage(systemSymbolName: symbol, accessibilityDescription: label)
        let button = NSButton(image: image ?? NSImage(), target: nil, action: nil)
        button.bezelStyle = .texturedRounded
        button.setAccessibilityLabel(label)
        return button
    }

    // MARK: Page events

    private func observe() {
        observer = Task { @MainActor [weak self] in
            guard let events = self?.page.events else { return }
            for await event in events {
                guard let self else { return }
                if case .focusChanged(true) = event { onFocus(self) }
                if case .rendererGone = event { recoverRenderer() }
                refresh()
                onChange()
            }
        }
    }

    /// How many times the renderer went away and the page was reloaded.
    private(set) var rendererRecoveries = 0

    /// The page's renderer crashed or was killed: it shows nothing until it loads again. A browser decides
    /// what to do; this one reloads at once.
    private func recoverRenderer() {
        rendererRecoveries += 1
        do {
            try page.navigation.reload()
        } catch {
            NSSound.beep()
        }
    }

    /// Brings the buttons and the address field up to date with the page.
    func refresh() {
        guard page.isAlive else { return }
        backButton.isEnabled = (try? page.navigation.canGoBack) ?? false
        forwardButton.isEnabled = (try? page.navigation.canGoForward) ?? false
        // Do not overwrite what the person is typing.
        if addressField.currentEditor() == nil {
            addressField.stringValue = page.url?.absoluteString ?? ""
        }
    }

    // MARK: Actions

    func load(_ url: URL) {
        Task { await navigate(to: url) }
    }

    /// Loads the URL and returns when it committed or failed. A failure is logged, not thrown: the page
    /// shows what the runtime made of it.
    func navigate(to url: URL) async {
        do {
            try await page.navigate(to: url)
        } catch {
            log.error("page \(self.index) could not load \(url.absoluteString): \(error)")
            NSSound.beep()
        }
        refresh()
        onChange()
    }

    /// Returns once the page has stopped loading, or when `timeout` has passed even if no further
    /// event arrives.
    func waitUntilLoaded(timeout: Duration = .seconds(30)) async {
        let page = page
        let wait = Task { @MainActor in
            // Subscribe before reading the state, so the end of the load cannot fall in between.
            var events = page.events.makeAsyncIterator()
            guard (try? page.state().isLoading) == true else { return }
            // Cancelling this task ends the wait for the next event.
            while let event = await events.next() {
                if case .navigationFinished = event { return }
                if case .navigationFailed = event, (try? page.state().isLoading) != true { return }
            }
        }
        let deadline = Task {
            try? await Task.sleep(for: timeout)
            wait.cancel()
        }
        await withTaskCancellationHandler {
            await wait.value
        } onCancel: {
            wait.cancel()
        }
        deadline.cancel()
    }

    @objc private func addressEntered() {
        onFocus(self)
        guard let url = AddressInput.url(from: addressField.stringValue) else {
            NSSound.beep()
            return
        }
        window?.makeFirstResponder(page.view)
        load(url)
    }

    @objc private func goBack() {
        perform { try page.navigation.goBack() }
    }

    @objc private func goForward() {
        perform { try page.navigation.goForward() }
    }

    @objc private func reload() {
        perform { try page.navigation.reload() }
    }

    private func perform(_ action: () throws -> Void) {
        onFocus(self)
        do {
            try action()
        } catch {
            log.error("page \(self.index): \(error)")
            NSSound.beep()
        }
    }

    func controlTextDidBeginEditing(_ notification: Notification) {
        onFocus(self)
    }

    // MARK: Report

    var status: AppStatus.Page {
        let view = page.view
        let entries = (try? page.navigation.entries) ?? []
        return AppStatus.Page(
            index: index, url: page.url?.absoluteString ?? "", title: page.title,
            currentIndex: (try? page.navigation.currentIndex) ?? -1,
            entries: entries.map { .init(url: $0.url?.absoluteString ?? "", title: $0.title) },
            isVisible: view.window?.isVisible == true && !view.isHiddenOrHasHiddenAncestor
                && view.bounds.width > 0 && view.bounds.height > 0,
            isLoading: (try? page.state().isLoading) ?? false,
            rendererRecoveries: rendererRecoveries)
    }
}
