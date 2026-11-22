import CBDK
import Foundation

/// Translation of one page (swift-api.md section 6.7). Get it from `BDKWebContents.translation`.
/// The provider, the languages and the preferences belong to the profile (`profile.translation`);
/// this is the page's state and the commands that act on it. Every call throws `contents.stale`
/// once the page is closed.
@MainActor
public struct BDKPageTranslation {
    private struct Arguments: Encodable {
        var from: String?
        var to: String?
    }

    let contents: BDKWebContents

    /// Where the page is: `unavailable` until its language is known, then `available` and so on.
    /// `BDKWebContents.Event.translation` reports each change.
    public func state() async throws -> BDKTranslationState {
        try await call("translation.state", Arguments())
    }

    /// Translates the page and returns the state it ends in. A language left out is the detected
    /// one (`from`) or the profile's target (`to`). Cancelling the task cancels the provider's
    /// work.
    @discardableResult
    public func translate(to: String? = nil, from: String? = nil) async throws
        -> BDKTranslationState
    {
        try await call("translation.translate", Arguments(from: from, to: to))
    }

    /// Shows the original text again.
    public func revert() async throws {
        _ = try await invoke("translation.revert", Arguments())
    }

    /// Asks for the translation surface: the application's `translation` handler, else the default
    /// presenter (ui-requests.md section 5.10).
    public func present() async throws {
        _ = try await invoke("translation.present", Arguments())
    }

    private func call(_ name: String, _ arguments: Arguments) async throws -> BDKTranslationState {
        try BDKServiceCoding.decode(
            BDKTranslationState.self, from: try await invoke(name, arguments))
    }

    private func invoke(_ name: String, _ arguments: Arguments) async throws -> String {
        _ = try contents.liveHandle()
        return try await contents.profile.runtime.commands.invoke(
            name, arguments: try BDKServiceCoding.encode(arguments), contents: contents)
    }
}

extension BDKWebContents {
    /// Translation of this page.
    public var translation: BDKPageTranslation {
        BDKPageTranslation(contents: self)
    }
}
