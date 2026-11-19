import Foundation
import MinimalBrowserCore
import Testing

/// Launches the built application for real. Needs a window server session (a logged-in Mac), and the
/// bundle from `scripts/bundle.sh`. The bundle holds the real runtime when one was resolved at .bdk/runtime
/// (Package.swift); otherwise the stub, and then these tests prove the application's own behaviour and
/// nothing about Chromium. The renderer-crash and private-profile tests need the real runtime; with it, the
/// home pages and the navigation tests load real sites, so they need the network.
@Suite("Minimal browser end to end", .serialized, .timeLimit(.minutes(2)))
struct E2ETests {
    private static let reserved = "https://www.iana.org/domains/reserved"
    private static let onChromium = AppRun.runtime == "chromium"

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

            // The stub titles each site from a script; Chromium shows what the sites serve.
            let titles =
                Self.onChromium
                ? ["Example Domain", "Example Domain"] : ["Example Domain", "Example Domain (org)"]
            let status = try await app.waitForStatus {
                $0.showsPages(2, loaded: true) && $0.windows[0].pages.map(\.title) == titles
            }
            #expect(status.runtime == AppRun.runtime)
            #expect(status.restored == false)
            #expect(status.windows.count == 1)
            let pages = status.windows[0].pages
            #expect(pages.map(\.url) == ["https://example.com/", "https://example.org/"])
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
        "the application survives a renderer crash: the page reloads and the other page is untouched",
        .enabled(if: onChromium, "needs the real runtime: the stub has no renderer"))
    func rendererCrashRecovery() async throws {
        try await withSupportDirectory { directory in
            let app = try AppRun(supportDirectory: directory, arguments: ["--crash-page", "0"])
            defer { app.kill() }
            let status = try await app.waitForStatus(timeout: .seconds(40)) {
                $0.showsPages(2, loaded: true) && $0.recovered(pages: [0])
            }
            let pages = status.windows[0].pages
            #expect(pages[0].rendererRecoveries == 1)
            #expect(pages[0].url == "https://example.com/")
            #expect(pages[0].title == "Example Domain")
            #expect(pages[1].rendererRecoveries == 0)
            #expect(pages[1].url == "https://example.org/")
            #expect(try await app.quitGracefully() == 0)
        }
    }

    /// Page 0 loads /set (a cookie, local storage and a cacheable image); then page 1 loads /get, which
    /// titles itself with the cookie and the storage it can see and fetches the same image.
    private func crossPageVisibility(private: Bool) async throws -> (title: String, fetches: Int) {
        let server = try LocalServer()
        var result = ("", 0)
        try await withSupportDirectory { directory in
            let app = try AppRun(
                supportDirectory: directory,
                arguments: (`private` ? ["--private"] : []) + [
                    "--navigate", "0=\(server.url("/set"))", "--navigate",
                    "1=\(server.url("/get"))",
                ])
            defer { app.kill() }
            let status = try await app.waitForStatus(timeout: .seconds(40)) {
                $0.showsPages(2, loaded: true) && $0.windows[0].pages[1].title.contains("|")
            }
            #expect(status.windows[0].pages[0].title == "set-done")
            result = (status.windows[0].pages[1].title, server.cachedImageFetches)
            #expect(try await app.quitGracefully() == 0)
        }
        return result
    }

    @Test(
        "pages in one profile share cookies, storage and the HTTP cache",
        .enabled(if: onChromium, "needs the real runtime"))
    func sharedProfileSharesState() async throws {
        let seen = try await crossPageVisibility(private: false)
        #expect(seen.title == "bdk=1|1")
        #expect(seen.fetches == 1)
    }

    @Test(
        "private profiles keep cookies, storage and the HTTP cache separate",
        .enabled(if: onChromium, "needs the real runtime"))
    func privateProfilesAreSeparate() async throws {
        let seen = try await crossPageVisibility(private: true)
        #expect(seen.title == "no-cookie|no-storage")
        #expect(seen.fetches == 2)
    }
}
