import CBDK
import Foundation

extension BDKRuntime {
    /// Asked before a navigation of a page's main frame sends its request, and again at each
    /// server redirect (swift-api.md 6.3). The navigation waits for the answer; there is no
    /// timeout. The page is nil when the application holds no object for it yet: a page the web or
    /// an extension opened whose new-window request has not reached the application.
    public typealias NavigationPolicy =
        @MainActor (BDKWebContents?, BDKWebContents.NavigationAction) async ->
        BDKWebContents.NavigationDecision
}

extension BDKWebContents {
    /// What the navigation policy answers for a navigation.
    public enum NavigationDecision: Sendable, Equatable {
        /// The navigation goes on.
        case allow
        /// No request is sent; the page stays where it is. `navigate(to:)` throws
        /// `navigation.cancelledByPolicy`.
        case cancel
        /// Cancel, then load this URL in the same page, as the application's own load: a GET with
        /// no body or referrer. Its start is not asked again; its server redirects are.
        /// `navigate(to:)` returns the commit of this URL.
        case redirect(URL)
    }

    /// A navigation the policy is asked about.
    public struct NavigationAction: Sendable, Equatable {
        public enum Kind: String, Sendable {
            case load
            case reload
            case history
        }

        public enum Initiator: Sendable, Equatable {
            /// The application: a load, reload or history navigation it asked for.
            case application
            /// The page: a link, a script or a form, with its origin when the runtime knows it.
            case page(origin: URL?)
            /// The browser, for anything else.
            case browser
        }

        /// As the application sees it: branded for an internal page.
        public let url: URL?
        public let kind: Kind
        /// "GET", or "POST" for a form submission.
        public let method: String
        public let isUserInitiated: Bool
        /// The URL a server redirect came from; nil when this is not a server redirect.
        public let redirectedFrom: URL?
        public let initiator: Initiator
        /// A prerender's navigation. A prerender that is not allowed is discarded.
        public let isPrerender: Bool

        public var isRedirect: Bool {
            redirectedFrom != nil
        }

        init?(json: String) {
            guard let fields = BDKJSValue(jsonString: json)?.objectValue,
                let url = fields["url"]?.stringValue
            else { return nil }
            self.url = URL(string: url)
            kind = fields["type"]?.stringValue.flatMap(Kind.init(rawValue:)) ?? .load
            method = fields["method"]?.stringValue ?? "GET"
            isUserInitiated = fields["isUserInitiated"]?.boolValue ?? false
            redirectedFrom = fields["redirectedFrom"]?.stringValue.flatMap(URL.init(string:))
            let initiator = fields["initiator"]?.objectValue
            switch initiator?["kind"]?.stringValue {
            case "application":
                self.initiator = .application
            case "page":
                self.initiator = .page(
                    origin: initiator?["origin"]?.stringValue.flatMap(URL.init(string:)))
            default:
                self.initiator = .browser
            }
            isPrerender = fields["isPrerender"]?.boolValue ?? false
        }
    }
}

extension BDKRuntime {
    /// Installs the ABI handler while a policy is set, and removes it when none is.
    func installNavigationPolicy() {
        guard let handle = try? liveHandle() else { return }
        if navigationPolicy == nil {
            guard isNavigationPolicyInstalled else { return }
            try? abiCall(.runtime) { bdk_navigation_policy_set_handler(handle, nil, nil, nil, $0) }
            isNavigationPolicyInstalled = false
            return
        }
        guard !isNavigationPolicyInstalled else { return }
        let box = Unmanaged.passRetained(NavigationPolicyBox(self))
        do {
            try abiCall(.runtime) {
                bdk_navigation_policy_set_handler(
                    handle, policyCallback, box.toOpaque(), policyRelease, $0)
            }
            isNavigationPolicyInstalled = true
        } catch {
            // A refused handler leaves the context with the caller.
            box.release()
        }
    }

    /// Asks the policy set now, and answers when it returns. A navigation the policy cannot be
    /// shown, or asked after it was removed, is allowed, as with no policy.
    fileprivate func askNavigationPolicy(
        _ decision: bdk_context_t, contents: bdk_contents_t, json: String
    ) {
        guard let policy = navigationPolicy,
            let action = BDKWebContents.NavigationAction(json: json)
        else {
            Self.answer(decision, .allow)
            return
        }
        let page = webContents(handle: contents)
        Task { @MainActor in
            Self.answer(decision, await policy(page, action))
        }
    }

    /// A decision whose navigation ended first is stale and needs no answer. A redirect URL the
    /// runtime refuses cancels the navigation, the safe reading.
    fileprivate static func answer(
        _ decision: bdk_context_t, _ verdict: BDKWebContents.NavigationDecision
    ) {
        defer { EventLoop.kick() }
        let action: UInt32
        let url: String
        switch verdict {
        case .allow: (action, url) = (UInt32(BDK_NAVIGATION_ALLOW), "")
        case .cancel: (action, url) = (UInt32(BDK_NAVIGATION_CANCEL), "")
        case .redirect(let target):
            (action, url) = (UInt32(BDK_NAVIGATION_REDIRECT), target.absoluteString)
        }
        let status = withBDKString(url) { bdk_navigation_policy_decide(decision, action, $0, nil) }
        if status == BDK_ERR_INVALID_ARGUMENT {
            _ = withBDKString("") {
                bdk_navigation_policy_decide(decision, UInt32(BDK_NAVIGATION_CANCEL), $0, nil)
            }
        }
    }
}

/// What the policy callback holds: the runtime, weakly, so the runtime keeps nothing alive.
private final class NavigationPolicyBox: @unchecked Sendable {
    weak var runtime: BDKRuntime?

    init(_ runtime: BDKRuntime) {
        self.runtime = runtime
    }
}

private let policyCallback: bdk_navigation_policy_cb = { context, decision, contents, json in
    guard let context else { return }
    let box = Unmanaged<NavigationPolicyBox>.fromOpaque(context).takeUnretainedValue()
    let text = swiftString(json)
    MainActor.assumeIsolated {
        guard let runtime = box.runtime else {
            BDKRuntime.answer(decision, .allow)
            return
        }
        runtime.askNavigationPolicy(decision, contents: contents, json: text)
    }
}

private let policyRelease: bdk_release_cb = { context in
    guard let context else { return }
    Unmanaged<NavigationPolicyBox>.fromOpaque(context).release()
}
