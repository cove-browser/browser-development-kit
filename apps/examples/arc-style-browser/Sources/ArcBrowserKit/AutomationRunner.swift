import ArcBrowserCore
import BDK
import Foundation

/// Runs the bundled script `Automation/spaces.ts` (M8, "Automation") over one page in each of two
/// spaces. The session may read, drive and script exactly those pages and the two profiles they
/// belong to; it sees nothing else the application has open.
@MainActor
public struct AutomationRunner {
    public static let script = "spaces"
    public static let capabilities = ["contents.read", "contents.control", "contents.script"]

    let controller: BrowserController

    public init(controller: BrowserController) {
        self.controller = controller
    }

    /// The page each of the first two spaces shows (its first page when no window shows it).
    public var defaultPages: [Layout.PageID] {
        controller.layout.spaces.prefix(2).compactMap { space in
            controller.layout.windows.first { $0.space == space.id }?.focusedPage
                ?? space.allPages.first
        }
    }

    /// Runs the script; `account`, when given, is the URL each page opens first. Returns the
    /// script's result, which the status report also shows.
    @discardableResult
    public func run(pages: [Layout.PageID]? = nil, account: URL? = nil) async throws -> BDKJSValue {
        let chosen = (pages ?? defaultPages).compactMap { controller.registry.pages[$0] }
        var profiles: [BDKProfile] = []
        for page in chosen where !profiles.contains(where: { $0 === page.profile }) {
            profiles.append(page.profile)
        }
        let session = try controller.runtime.automation.makeSession(
            .init(
                name: Self.script, capabilities: Self.capabilities,
                scope: .init(profiles: profiles, contents: .explicit(chosen))))
        defer { session.revoke() }
        let arguments: BDKJSValue =
            account.map { .object(["account": .string($0.absoluteString)]) } ?? .null
        let value = try await session.run(Self.script, arguments: arguments).value
        controller.automationResult = Self.json(value)
        controller.onChange()
        return value
    }

    static func json(_ value: BDKJSValue) -> String {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys, .withoutEscapingSlashes]
        return (try? encoder.encode(value)).map { String(decoding: $0, as: UTF8.self) } ?? "null"
    }
}
