import AppKit
import CBDK
import Foundation

// What `profile.webApps` adds to the generated commands (web-apps.md sections 2 and 5): the
// installability of a page, the install and uninstall the application's own user confirmed, and
// launching an app as a page.

/// What to install: a page's own manifest, or the manifest a start URL links, loaded in the
/// profile without a visible page.
public enum BDKWebAppInstallSource: Sendable {
    case page(BDKWebContents)
    case url(URL)
}

/// A page launched as an installed web app: the app, and whether the page's last committed URL
/// is inside the app's scope. A presenter that shows no address bar must show the page's origin
/// while `isInScope` is false (web-apps.md 4.2).
public struct BDKWebAppPageState: Sendable, Equatable {
    public let app: BDKWebAppInfo
    public let isInScope: Bool

    public init(app: BDKWebAppInfo, isInScope: Bool) {
        self.app = app
        self.isInScope = isInScope
    }
}

extension BDKWebApps {
    /// Whether the page can be installed, and what would be. Raises no prompt.
    public func installability(of page: BDKWebContents) async throws -> BDKWebAppInstallability {
        _ = try page.liveHandle()
        let json = try await scope.runtime.commands.invoke(
            "webApps.installability", arguments: "{}", profile: scope.profile, contents: page)
        return try BDKServiceCoding.decode(BDKWebAppInstallability.self, from: json)
    }

    /// Installs into this profile. The application has asked its user, so no `web_app_installs`
    /// request is raised: `event` is the event that confirmed it, and without one the call throws
    /// `ui.gestureRequired`. `install(url:)` asks through the request instead.
    @discardableResult
    public func install(_ source: BDKWebAppInstallSource, confirmedBy event: NSEvent)
        async throws -> BDKWebAppInfo
    {
        var page: BDKWebContents?
        var arguments = "{}"
        switch source {
        case .page(let contents): page = contents
        case .url(let url):
            arguments = try BDKServiceCoding.encode(["url": url.absoluteString])
        }
        let json = try await confirmed(
            bdk_web_app_install, arguments: arguments, page: page, event: event)
        return try BDKServiceCoding.decode(BDKWebAppInfo.self, from: json)
    }

    /// Uninstalls the app from this profile, confirmed by `event` as `install` is.
    public func uninstall(_ id: String, clearSiteData: Bool = false, confirmedBy event: NSEvent)
        async throws
    {
        struct Arguments: Encodable {
            var id: String
            var clearSiteData: Bool
        }
        _ = try await confirmed(
            bdk_web_app_uninstall,
            arguments: try BDKServiceCoding.encode(
                Arguments(id: id, clearSiteData: clearSiteData)),
            page: nil, event: event)
    }

    /// Launches the app in this profile. The page loads the start URL, or the shortcut's, and is
    /// also announced as a new-window request of kind `.webApp`, which places it.
    @discardableResult
    public func launch(_ id: String, shortcut: BDKWebAppShortcut? = nil) async throws
        -> BDKWebContents
    {
        guard let profile = scope.profile else { throw BDKError.stale(.profile) }
        let app = try await get(id)
        let launched = try await launchContents(id, shortcutUrl: shortcut?.url)
        guard let handle = UInt64(launched.contents) else {
            throw BDKError(
                domain: .runtime, code: "malformedResult",
                message: "webApps.launch answered no page handle")
        }
        let page = BDKWebContents.adopt(handle: handle, in: profile)
        page.noteWebApp(app)
        return page
    }

    private typealias ConfirmedCall = (
        bdk_runtime_t, bdk_string_t, UnsafePointer<bdk_invoke_options_t>?, bdk_native_handle_t,
        bdk_result_cb?, UnsafeMutableRawPointer?, bdk_release_cb?
    ) -> bdk_call_t

    private func confirmed(
        _ function: ConfirmedCall, arguments: String, page: BDKWebContents?, event: NSEvent
    ) async throws -> String {
        let runtime = try scope.runtime.liveHandle()
        var filled = bdk_invoke_options_t()
        filled.struct_size = MemoryLayout<bdk_invoke_options_t>.size
        filled.profile = try scope.profile?.liveHandle() ?? 0
        filled.contents = try page?.liveHandle() ?? 0
        let options = filled
        let gesture = gestureHandle(event)
        return try await abiAsync(.runtime) { context, result, release in
            withBDKString(arguments) { lent in
                withUnsafePointer(to: options) {
                    function(runtime, lent, $0, gesture, result, context, release)
                }
            }
        }
    }
}
