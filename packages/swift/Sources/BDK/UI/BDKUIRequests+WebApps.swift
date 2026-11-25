import AppKit
import Foundation

/// The install prompt of an installable web app (web-apps.md 4.1). A presenter shows `app.origin`
/// beside `app.name`: the name and icons are chosen by the site and can imitate another product.
public final class WebAppInstallRequest: BDKUIRequest {
    public enum Source: String, Sendable {
        /// The page called `prompt()` on its `beforeinstallprompt` event.
        case page
        /// A page or a session called `webApps.install`.
        case command
    }

    /// The app as its manifest declares it. `installedAt` is not meaningful until it is installed.
    public let app: BDKWebAppInfo
    public let source: Source
    /// The manifest's screenshots, when it has any.
    public let screenshots: [BDKWebAppIcon]

    override init(_ delivery: BDKUIDelivery, runtime: BDKRuntime) {
        let fields = BDKUIPayload(delivery)
        app =
            fields.fields["app"].flatMap {
                try? BDKServiceCoding.decode(BDKWebAppInfo.self, from: $0.jsonString)
            } ?? Self.unreadable
        source = fields.choice("source", default: .page)
        screenshots =
            fields.fields["screenshots"].flatMap {
                try? BDKServiceCoding.decode([BDKWebAppIcon].self, from: $0.jsonString)
            } ?? []
        super.init(delivery, runtime: runtime)
    }

    /// Installs into the page's profile. `event` is the click that confirmed it: without a gesture
    /// it throws `ui.gestureRequired`.
    public func install(confirmedBy event: NSEvent) throws {
        try submit("grant", gesture: event)
    }

    /// The page's `prompt()` resolves with "dismissed"; a command fails with `webApps.cancelled`.
    public func cancel() {
        try? submit("deny")
    }

    /// What a request from a newer runtime whose app this version cannot read shows: nothing to
    /// install from, so a presenter should cancel it.
    private static let unreadable = BDKWebAppInfo(
        id: "", profile: "", name: "", startUrl: URL(string: "about:blank")!,
        scope: URL(string: "about:blank")!, origin: "", displayMode: .standalone, icons: [],
        shortcuts: [], installedAt: Date(timeIntervalSince1970: 0), hasPendingUpdate: false)
}
