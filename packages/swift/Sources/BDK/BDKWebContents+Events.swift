import AppKit
import CBDK
import Foundation

extension BDKWebContents {
    public struct NavigationInfo: Sendable, Equatable {
        public let url: URL?
        public let isMainFrame: Bool
        public let isSameDocument: Bool
        public let isUserInitiated: Bool
        public let isErrorPage: Bool

        init(url: String, flags: UInt32) {
            self.url = URL(string: url)
            isMainFrame = flags & BDK_EVENT_FLAG_MAIN_FRAME != 0
            isSameDocument = flags & BDK_EVENT_FLAG_SAME_DOCUMENT != 0
            isUserInitiated = flags & BDK_EVENT_FLAG_USER_INITIATED != 0
            isErrorPage = flags & BDK_EVENT_FLAG_ERROR_PAGE != 0
        }
    }

    public typealias Commit = NavigationInfo

    public struct NavigationFailure: Sendable, Equatable {
        public let url: URL?
        /// The network error, or 0 when the navigation was stopped or superseded.
        public let netError: Int32
        /// "superseded", "aborted", "handledNatively", "cancelledByPolicy", "redirectedByPolicy"
        /// or "netError".
        public let reason: String
        public let isErrorPage: Bool
        /// For "redirectedByPolicy": the URL the navigation policy loads instead.
        public let redirectURL: URL?

        /// The failure as the error `load` throws.
        public var error: BDKError {
            switch reason {
            case "superseded":
                BDKError(
                    domain: .navigation, code: "superseded",
                    message: "another navigation replaced this one")
            case "aborted":
                BDKError(
                    domain: .navigation, code: "aborted", message: "the navigation was stopped")
            case "handledNatively":
                BDKError(
                    domain: .navigation, code: "handledNatively",
                    message: "the route's native handler showed the page")
            case "cancelledByPolicy":
                BDKError(
                    domain: .navigation, code: "cancelledByPolicy",
                    message: "the application's navigation policy cancelled the navigation")
            default:
                BDKError(
                    domain: .navigation, code: "netError",
                    message: "the navigation failed with network error \(netError)",
                    details: ["netError": .number(Double(netError))])
            }
        }
    }

    public enum Event: Sendable, Equatable {
        case navigationStarted(NavigationInfo)
        case navigationCommitted(Commit)
        /// The load finished.
        case navigationFinished(Commit)
        case navigationFailed(NavigationFailure)
        case titleChanged(String)
        case focusChanged(Bool)
        case closed
        /// The page's renderer process ended (it crashed, was killed or ran out of memory). The page
        /// shows nothing until it is reloaded or navigated; the application decides which.
        case rendererGone(reason: String)
        /// The page became visible or hidden: its view entered or left a visible window.
        case visibilityChanged(Bool)
        /// A result of the search `find.start` began.
        case findResult(FindSession.Result)
        /// The page's translation changed phase or progress.
        case translation(BDKTranslationState)
        /// A page launched as an installed web app left its app's scope or came back into it
        /// (`webApp.isInScope`).
        case webAppScope(appID: String, isInScope: Bool)
        /// A kind this version does not know, from a newer runtime.
        case unknown(kind: UInt32)
    }
}

extension BDKJSValue {
    fileprivate func string(_ key: String) -> String? {
        guard case .object(let object) = self, case .string(let value) = object[key] else {
            return nil
        }
        return value
    }

    fileprivate func bool(_ key: String) -> Bool? {
        guard case .object(let object) = self, case .bool(let value) = object[key] else {
            return nil
        }
        return value
    }
}

extension BDKWebContents.Event {
    init(_ event: bdk_contents_event_t) {
        let detail = BDKJSValue(jsonString: swiftString(event.detail_json))
        let info = BDKWebContents.NavigationInfo(url: swiftString(event.url), flags: event.flags)
        switch event.kind {
        case BDK_CONTENTS_EVENT_NAVIGATION_STARTED:
            self = .navigationStarted(info)
        case BDK_CONTENTS_EVENT_NAVIGATION_COMMITTED:
            self = .navigationCommitted(info)
        case BDK_CONTENTS_EVENT_NAVIGATION_FAILED:
            self = .navigationFailed(
                BDKWebContents.NavigationFailure(
                    url: info.url, netError: event.net_error,
                    reason: detail?.string("code") ?? "netError", isErrorPage: info.isErrorPage,
                    redirectURL: detail?.string("redirectUrl").flatMap(URL.init(string:))))
        case BDK_CONTENTS_EVENT_LOAD_FINISHED:
            self = .navigationFinished(info)
        case BDK_CONTENTS_EVENT_TITLE_CHANGED:
            self = .titleChanged(detail?.string("title") ?? "")
        case BDK_CONTENTS_EVENT_FOCUS_CHANGED:
            self = .focusChanged(detail?.bool("focused") ?? false)
        case BDK_CONTENTS_EVENT_CLOSED:
            self = .closed
        case BDK_CONTENTS_EVENT_RENDERER_GONE:
            self = .rendererGone(reason: detail?.string("reason") ?? "crashed")
        case BDK_CONTENTS_EVENT_VISIBILITY_CHANGED:
            self = .visibilityChanged(detail?.bool("visible") ?? false)
        default:
            self = Self.laterKind(event.kind, detail)
        }
    }

    /// The kinds beyond navigation and lifecycle, kept apart so that `init` stays simple.
    private static func laterKind(_ kind: UInt32, _ detail: BDKJSValue?) -> Self {
        switch kind {
        case BDK_CONTENTS_EVENT_FIND_RESULT:
            .findResult(FindSession.Result(detail))
        case BDK_CONTENTS_EVENT_TRANSLATION:
            detail.flatMap {
                try? BDKServiceCoding.decode(BDKTranslationState.self, from: $0.jsonString)
            }
            .map { .translation($0) } ?? .unknown(kind: kind)
        case BDK_CONTENTS_EVENT_WEB_APP:
            .webAppScope(
                appID: detail?.string("appId") ?? "", isInScope: detail?.bool("isInScope") ?? true)
        default:
            .unknown(kind: kind)
        }
    }
}

/// What the event callback needs. It holds the page weakly, so the engine retaining it until the
/// release callback cannot keep the page alive.
final class ContentsEventSink {
    let broadcaster: EventBroadcaster<BDKWebContents.Event>
    weak var contents: BDKWebContents?

    init(broadcaster: EventBroadcaster<BDKWebContents.Event>, contents: BDKWebContents) {
        self.broadcaster = broadcaster
        self.contents = contents
    }
}

extension BDKWebContents {
    /// Subscribes to the page's events and installs the keyboard handler.
    func installBridges() {
        let sink = Unmanaged.passRetained(
            ContentsEventSink(broadcaster: broadcaster, contents: self))
        _ = bdk_contents_subscribe(
            handle,
            { context, event in
                guard let context, let event else { return }
                let sink = Unmanaged<ContentsEventSink>.fromOpaque(context).takeUnretainedValue()
                let mapped = Event(event.pointee)
                MainActor.assumeIsolated { sink.contents?.noteEvent(mapped) }
                sink.broadcaster.send(mapped)
            }, sink.toOpaque(),
            { context in
                guard let context else { return }
                Unmanaged<ContentsEventSink>.fromOpaque(context).release()
            })
        installKeyboardHandler()
    }
}
