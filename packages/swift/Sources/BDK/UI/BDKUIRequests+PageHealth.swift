import AppKit

// Page health (ui-requests.md 5.19, T-UIR-15): a page whose renderer stopped answering, and the
// popups Chromium's blocker stopped. Both arrive on the `page_health` surface and are told apart
// by the payload's `kind`. The surface cannot be disabled; with no handler the default presenter
// shows them.
//
// Completions: an unresponsive page is answered by "accept" with `{"action":"wait"}` or
// `{"action":"terminate"}` ("deny", `.reject` and a dropped request wait). Blocked popups are
// answered by "grant" with `{"popup": id}` (opens it, as a new-window request) or
// `{"always": true}` (allows the site's popups), each with the confirming event, or by "deny".

public final class UnresponsivePageRequest: BDKUIRequest {
    /// The page's origin.
    public let origin: URL?
    /// The page's title, which the question names so the person knows which page is stuck.
    public let pageTitle: String

    override init(_ delivery: BDKUIDelivery, runtime: BDKRuntime) {
        let fields = BDKUIPayload(delivery)
        origin = fields.url("origin")
        pageTitle = fields.string("pageTitle") ?? ""
        super.init(delivery, runtime: runtime)
    }

    /// Keeps the page. Chromium asks again if it stays unresponsive.
    public func wait() {
        try? submit("accept", ["action": .string("wait")])
    }

    /// Ends the page's renderer; the page then reports `.rendererGone(reason: "killed")`.
    public func terminate() {
        try? submit("accept", ["action": .string("terminate")])
    }

    /// What a presenter shows, in Chromium's wording: the default presenter shows the same.
    public var presentation: BDKPageHealthPresentation {
        BDKPageHealthPresentation(
            title: pageTitle.isEmpty ? "Page Unresponsive" : "\(pageTitle) is unresponsive",
            message: "You can wait for it to become responsive or exit the page.",
            items: [], actions: [.terminate("Exit page"), .wait("Wait")])
    }
}

public final class BlockedPopupRequest: BDKUIRequest {
    public struct BlockedPopup: Sendable, Hashable, Identifiable {
        public let id: String
        public let url: URL
    }

    /// The page's origin, which "always allow" applies to.
    public let origin: URL?
    /// Every popup blocked since the last notice was answered. A new one withdraws this request
    /// and raises another with the whole list.
    public let popups: [BlockedPopup]

    /// The popups' addresses, in the order they were blocked.
    public var urls: [URL] { popups.map(\.url) }

    override init(_ delivery: BDKUIDelivery, runtime: BDKRuntime) {
        let fields = BDKUIPayload(delivery)
        origin = fields.url("origin")
        popups = fields.objects("popups").compactMap { popup in
            guard let id = popup.string("id"), let url = popup.url("url") else { return nil }
            return BlockedPopup(id: id, url: url)
        }
        super.init(delivery, runtime: runtime)
    }

    /// Opens one blocked popup, which then raises a new-window request. `event` is the click that
    /// chose it: without one it throws `ui.gestureRequired`. Throws `ui.popupNotListed` for a popup
    /// the notice does not list.
    public func allowOnce(_ popup: BlockedPopup, confirmedBy event: NSEvent) throws {
        guard popups.contains(popup) else {
            throw BDKError(
                domain: .ui, code: "popupNotListed", message: "the notice does not list this popup")
        }
        try submit("grant", ["popup": .string(popup.id)], gesture: event)
    }

    /// Allows the site's popups in this profile from now on, a site setting. Needs the confirming
    /// event, as every grant does.
    public func alwaysAllow(confirmedBy event: NSEvent) throws {
        try submit("grant", ["always": .bool(true)], gesture: event)
    }

    public func dismiss() {
        try? submit("deny")
    }

    public var presentation: BDKPageHealthPresentation {
        let site = origin?.host ?? origin?.absoluteString ?? "this site"
        return BDKPageHealthPresentation(
            title: "Pop-ups blocked", message: "Blocked on this page:",
            items: popups.map { .popup($0) },
            actions: [
                .alwaysAllow("Always allow pop-ups and redirects from \(site)"), .done("Done"),
            ])
    }
}

/// What a page-health presenter shows, whatever draws it: a title, a message, the blocked popups
/// to choose from and the buttons in order. `perform(_:confirmedBy:)` on the request answers with
/// one of them.
public struct BDKPageHealthPresentation: Sendable, Equatable {
    public enum Item: Sendable, Hashable {
        case popup(BlockedPopupRequest.BlockedPopup)
    }

    public enum Action: Sendable, Hashable {
        case wait(String)
        case terminate(String)
        case alwaysAllow(String)
        case done(String)

        public var label: String {
            switch self {
            case .wait(let label), .terminate(let label), .alwaysAllow(let label),
                .done(let label):
                label
            }
        }
    }

    public let title: String
    public let message: String
    public let items: [Item]
    public let actions: [Action]
}

extension UnresponsivePageRequest {
    /// Answers with an action of the presentation.
    public func perform(_ action: BDKPageHealthPresentation.Action) {
        if case .terminate = action { terminate() } else { wait() }
    }
}

extension BlockedPopupRequest {
    /// Answers with an item or action of the presentation. Opening a popup and always allowing
    /// the site need `event`, the click that chose them.
    public func perform(_ item: BDKPageHealthPresentation.Item, confirmedBy event: NSEvent) throws {
        switch item {
        case .popup(let popup): try allowOnce(popup, confirmedBy: event)
        }
    }

    public func perform(_ action: BDKPageHealthPresentation.Action, confirmedBy event: NSEvent?)
        throws
    {
        switch action {
        case .alwaysAllow:
            guard let event else {
                throw BDKError(
                    domain: .ui, code: "gestureRequired",
                    message: "always allowing popups needs the event that chose it")
            }
            try alwaysAllow(confirmedBy: event)
        case .done, .wait, .terminate:
            dismiss()
        }
    }
}
