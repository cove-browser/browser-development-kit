import AppKit
import ArcBrowserCore
import ArcBrowserKit
import BDK
import BDKTestSupport
import Foundation
import Testing

/// The application running on the stub runtime in this process, with its real configuration (the
/// `bdk.resolved.json` that `bdk build config` writes from `bdk.toml`; the `test` target runs it
/// first) and its real windows.
@MainActor
struct BrowserFixture {
    let runtime: BDKRuntime
    let controller: BrowserController
    let placement: ExtensionPlacement
    let pages: PagePlacement

    /// The sites of the initial layout, which the stub answers with these titles.
    static let urls = Layout.InitialURLs(
        personalPinned: "https://personal-pinned.example/", personal: "https://personal.example/",
        workPinned: "https://work-pinned.example/", work: "https://work.example/")

    static var resolvedJSON: String {
        get throws {
            let file = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
                .appendingPathComponent("../../bdk.resolved.json").standardizedFileURL
            guard FileManager.default.fileExists(atPath: file.path) else {
                throw Failure(
                    description:
                        "\(file.path) is missing; run `bdk build config --project apps/examples/arc-style-browser`"
                )
            }
            return try String(contentsOf: file, encoding: .utf8)
        }
    }

    struct Failure: Error, CustomStringConvertible {
        let description: String
    }

    /// The focused window's AppKit window.
    var focusedNSWindow: NSWindow? {
        controller.windows[controller.focusedWindow]?.window
    }

    func page(_ id: Layout.PageID) throws -> BDKWebContents {
        try #require(controller.registry.pages[id])
    }

    func space(_ name: String) throws -> Layout.Space {
        try controller.layout.space(try controller.space(named: name))
    }
}

/// Starts the application, runs `body`, then closes its windows and shuts the runtime down.
/// `configure` adds what a test needs to the application's configuration, such as a handler for
/// a surface the application leaves to BDK.
@MainActor
func withBrowser(
    session: SessionFile? = nil, configure: (BDKConfiguration) throws -> Void = { _ in },
    _ body: (BrowserFixture) async throws -> Void
) async throws {
    let configuration = BDKConfiguration(resolvedJSON: try BrowserFixture.resolvedJSON)
    try configure(configuration)
    let placement = ExtensionPlacement()
    try placement.install(in: configuration)
    let pages = PagePlacement()
    try pages.install(in: configuration)
    let runtime = try BDKRuntime.startStub(configuration)
    let controller = BrowserController(
        runtime: runtime, session: session, initial: .initial(urls: BrowserFixture.urls),
        extensions: placement, pages: pages)
    let fixture = BrowserFixture(
        runtime: runtime, controller: controller, placement: placement, pages: pages)
    do {
        try await controller.start()
        try await body(fixture)
    } catch {
        controller.closeWindows()
        await runtime.shutdown()
        throw error
    }
    controller.closeWindows()
    await runtime.shutdown()
}

/// Polls until `condition` holds, letting the stub and AppKit run in between.
@MainActor
func waitUntil(
    _ what: String, timeout: Duration = .seconds(5), _ condition: () throws -> Bool
) async throws {
    let deadline = ContinuousClock.now + timeout
    while try !condition() {
        if ContinuousClock.now > deadline {
            throw BrowserFixture.Failure(description: "timed out waiting for \(what)")
        }
        try await Task.sleep(for: .milliseconds(5))
    }
}

extension BDKWebContents {
    /// The page's view is in a visible window, not hidden, with a size.
    @MainActor
    var isOnScreen: Bool {
        view.window?.isVisible == true && !view.isHiddenOrHasHiddenAncestor && view.bounds.width > 0
    }
}
