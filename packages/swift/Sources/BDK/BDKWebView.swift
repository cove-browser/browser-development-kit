import AppKit
import SwiftUI

/// The view of one page. `BDKWebContents.view` always returns the same view.
///
/// It hosts the engine's native view and fills its bounds. When the runtime has no native view (the
/// stub runtime), it shows the page's URL and title instead. Adding it to a window registers that
/// window with the runtime, once per window; moving it to another window keeps the page, its
/// navigation state and its renderer.
@MainActor
public final class BDKWebView: NSView {
    /// The page. Nil once the page was released.
    public private(set) weak var contents: BDKWebContents?
    public weak var keyboardDelegate: (any BDKKeyboardDelegate)? {
        get { contents?.keyboardDelegate }
        set { contents?.keyboardDelegate = newValue }
    }

    private let urlLabel = BDKWebView.label(size: 13, weight: .medium)
    private let titleLabel = BDKWebView.label(size: 11, weight: .regular)
    private let placeholder = NSStackView()
    private var hostedView: NSView?
    private var observer: Task<Void, Never>?

    /// `nativeView` replaces the engine's view; tests use it to stand in for one.
    init(contents: BDKWebContents, nativeView: NSView? = nil) {
        self.contents = contents
        super.init(frame: .zero)
        placeholder.orientation = .vertical
        placeholder.spacing = 4
        placeholder.addArrangedSubview(urlLabel)
        placeholder.addArrangedSubview(titleLabel)
        addSubview(placeholder)
        if let native = nativeView ?? (try? contents.nativeView()) {
            host(native)
        }
        refreshPlaceholder()
        observeContents(contents)
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        nil
    }

    isolated deinit {
        observer?.cancel()
    }

    private static func label(size: CGFloat, weight: NSFont.Weight) -> NSTextField {
        let field = NSTextField(labelWithString: "")
        field.font = .systemFont(ofSize: size, weight: weight)
        field.textColor = .secondaryLabelColor
        field.lineBreakMode = .byTruncatingMiddle
        return field
    }

    /// The text of the placeholder, for tests and accessibility.
    var placeholderText: (url: String, title: String) {
        (urlLabel.stringValue, titleLabel.stringValue)
    }

    var isShowingPlaceholder: Bool {
        !placeholder.isHidden
    }

    // MARK: Hosting

    private func host(_ native: NSView) {
        hostedView?.removeFromSuperview()
        hostedView = native
        addSubview(native, positioned: .below, relativeTo: placeholder)
        placeholder.isHidden = true
        needsLayout = true
    }

    public override func layout() {
        super.layout()
        hostedView?.frame = bounds
        placeholder.frame = bounds.insetBy(dx: 12, dy: 12)
    }

    // MARK: Placeholder

    private func observeContents(_ contents: BDKWebContents) {
        // Subscribe now, not when the task first runs, so that no event is missed in between.
        let stream = contents.events.makeStream()
        observer = Task { @MainActor [weak self] in
            for await _ in stream { self?.refreshPlaceholder() }
        }
    }

    private func refreshPlaceholder() {
        guard let contents else { return }
        urlLabel.stringValue = contents.url?.absoluteString ?? "about:blank"
        titleLabel.stringValue = contents.title
    }

    func contentsWasInvalidated() {
        urlLabel.stringValue = "closed"
        titleLabel.stringValue = ""
    }

    // MARK: Windows

    public override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        guard let window, let runtime = contents?.profile.runtime else { return }
        // A runtime that is no longer running has nothing to register with.
        _ = try? runtime.windows.register(window)
    }

    // MARK: Focus

    public override var acceptsFirstResponder: Bool {
        true
    }

    public override func becomeFirstResponder() -> Bool {
        try? contents?.focus()
        return super.becomeFirstResponder()
    }

    /// Makes the page the first responder of its window and the focused page of the runtime.
    public func focusContent() {
        window?.makeFirstResponder(self)
        try? contents?.focus()
    }
}

/// SwiftUI host of a page's view. Several containers may show the same page over time, but only one
/// at a time: SwiftUI moves the one view. Passing a different page replaces the view shown.
public struct BDKWebContentsView: View {
    private let contents: BDKWebContents

    public init(_ contents: BDKWebContents) {
        self.contents = contents
    }

    public var body: some View {
        // Each page is its own SwiftUI identity, so a new page gets a new host instead of an update
        // to the old page's view, which belongs to that page and cannot show another.
        Host(contents: contents).id(contents.id)
    }

    private struct Host: NSViewRepresentable {
        let contents: BDKWebContents

        func makeNSView(context: Context) -> BDKWebView {
            contents.view
        }

        func updateNSView(_ nsView: BDKWebView, context: Context) {}
    }
}
