import ArcBrowserCore
import BDK
import Foundation

/// Puts the pages that pages open in the layout (ui-requests.md 5.6): a `target="_blank"` link, a
/// middle click, "Open link in new page", `window.open` without features, an extension's options
/// page or a link in its popup. Each becomes a page of the opener's space, shown in the opener's
/// window unless it asked for the background. A popup (`window.open` with features, as sign-in
/// and payment pages use) and every other kind keep BDK's default: a window of its own, which
/// closes when the page does.
@MainActor
public final class PagePlacement {
    weak var controller: BrowserController?

    public init() {}

    /// Registers the handler. Before the runtime starts: a surface gains no handler after.
    public func install(in configuration: BDKConfiguration) throws {
        try configuration.ui.newWindows.setHandler { [weak self] request in
            self?.place(request) ?? .useDefault
        }
    }

    func place(_ request: NewWindowRequest) -> BDKUIResponse {
        guard request.kind == .newPage, let controller, let page = request.newContents else {
            return .useDefault
        }
        do {
            try controller.adopt(
                page, url: request.targetURL, openedBy: request.opener,
                placement: Self.placement(of: request.disposition))
        } catch {
            controller.log.error("new page not placed: \(error)")
            return .useDefault
        }
        request.accept()
        return .handled
    }

    static func placement(of disposition: NewWindowRequest.Disposition)
        -> BrowserController.NewPagePlacement
    {
        switch disposition {
        case .background: .background
        case .newWindow: .newWindow
        case .foreground, .popup, .privateWindow: .foreground
        }
    }
}
