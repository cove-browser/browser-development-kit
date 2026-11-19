import Foundation
import MinimalBrowserCore
import Testing

/// Launches the built application for real. Needs a window server session (a logged-in Mac), and the
/// bundle from `scripts/bundle.sh`. The runtime behind the pages is the stub, so these tests prove the
/// application's own behaviour (windows, wiring, session save and restore, quit) and nothing about Chromium.
@Suite("Minimal browser end to end", .serialized, .timeLimit(.minutes(2)))
struct E2ETests {
    private static let reserved = "https://www.iana.org/domains/reserved"

    /// Runs `body` with a fresh support directory and always cleans up.
    private func withSupportDirectory(_ body: (URL) async throws -> Void) async throws {
        let directory = try AppRun.makeSupportDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        try await body(directory)
    }

    @Test("two pages are visible in one window, and a graceful quit exits with status 0")
    func launchSmoke() async throws {
        try await withSupportDirectory { directory in
            let app = try AppRun(supportDirectory: directory)
            defer { app.kill() }

            let status = try await app.waitForStatus { $0.showsPages(2, loaded: true) }
            #expect(status.runtime == "stub")
            #expect(status.restored == false)
            #expect(status.windows.count == 1)
            let pages = status.windows[0].pages
            #expect(pages.map(\.url) == ["https://example.com/", "https://example.org/"])
            #expect(pages.map(\.title) == ["Example Domain", "Example Domain (org)"])
            // The window title follows the focused page, which is the first.
            #expect(status.windows[0].title == "Example Domain")

            let exit = try await app.quitGracefully()
            #expect(exit == 0)
            let final = try AppStatus.read(from: app.statusFile)
            #expect(final.phase == .terminating)
            #expect(
                FileManager.default.fileExists(
                    atPath: directory.appendingPathComponent("session.json").path))
        }
    }

    @Test("a relaunch restores the navigation entries each page had at quit")
    func relaunch() async throws {
        try await withSupportDirectory { directory in
            let first = try AppRun(
                supportDirectory: directory,
                arguments: [
                    "--navigate", "0=\(Self.reserved)", "--navigate", "1=https://example.net/",
                ])
            defer { first.kill() }
            let before = try await first.waitForStatus {
                $0.showsPages(2, loaded: true)
                    && $0.windows[0].pages.allSatisfy { $0.entries.count == 2 }
            }
            #expect(before.restored == false)
            let pagesBefore = before.windows[0].pages
            #expect(pagesBefore[0].entries.map(\.url) == ["https://example.com/", Self.reserved])
            #expect(
                pagesBefore[1].entries.map(\.url) == [
                    "https://example.org/", "https://example.net/",
                ])
            #expect(try await first.quitGracefully() == 0)

            let second = try AppRun(supportDirectory: directory)
            defer { second.kill() }
            let after = try await second.waitForStatus { $0.showsPages(2, loaded: true) }
            #expect(after.restored == true)
            let pagesAfter = after.windows[0].pages
            #expect(pagesAfter.map(\.entries) == pagesBefore.map(\.entries))
            #expect(pagesAfter.map(\.currentIndex) == pagesBefore.map(\.currentIndex))
            #expect(pagesAfter.map(\.url) == pagesBefore.map(\.url))
            #expect(pagesAfter.map(\.title) == pagesBefore.map(\.title))
            #expect(try await second.quitGracefully() == 0)
        }
    }

    @Test(
        "the application survives a renderer crash",
        .disabled(
            "Not testable until the real runtime exists. The stub has no renderer to crash, and the C ABI has no process-gone event: the BDK_CONTENTS_EVENT_* kinds in packages/core/include/bdk/bdk_ids.h are navigation started, committed and failed, load finished, title, focus and closed only (T-ABI-03 adds the process-gone event, and swift-api.md section 18 reacts to .processGone with a reload)."
        ))
    func rendererCrashRecovery() async throws {}
}
