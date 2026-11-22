import ArcBrowserCore
import Foundation
import Testing

/// Launches the built application and drives it with `--do` actions, as a person would through
/// the sidebar and menus. Needs a window server session and the bundle from `scripts/bundle.sh`.
/// With the stub runtime these prove the application's own behaviour; with the real runtime they
/// also show Chromium reporting each page's visibility, and the automation and settings checks
/// run. The real runtime loads the initial layout's sites, so it needs the network.
@Suite("Arc-style browser end to end", .serialized, .timeLimit(.minutes(3)))
struct E2ETests {
    private static let onChromium = AppRun.runtime == "chromium"

    private func withSupportDirectory(_ body: (URL) async throws -> Void) async throws {
        let directory = try AppRun.makeSupportDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        try await body(directory)
    }

    private static func onScreen(_ status: AppStatus) -> [AppStatus.Page] {
        status.pages.filter(\.onScreen)
    }

    @Test("the first launch shows the Personal space's pinned page, and a quit saves the session")
    func launchSmoke() async throws {
        try await withSupportDirectory { directory in
            let app = try AppRun(supportDirectory: directory)
            defer { app.kill() }
            let status = try await app.waitForStatus { $0.actionsDone }
            #expect(status.runtime == AppRun.runtime)
            #expect(!status.restored)
            #expect(status.windows.map(\.space) == ["Personal"])
            #expect(status.pages.count == 4)
            let shown = Self.onScreen(status)
            #expect(
                shown.count == 1 && shown.first?.pinned == true && shown.first?.space == "Personal")
            #expect(status.pages.filter { $0.window == nil }.allSatisfy { !$0.onScreen })
            if Self.onChromium {
                // Chromium reports every page whose view is in no window as hidden.
                #expect(status.pages.filter { !$0.onScreen }.allSatisfy { !$0.runtimeVisible })
            }
            #expect(try await app.quitGracefully() == 0)
            #expect(
                FileManager.default.fileExists(
                    atPath: SessionFile.fileURL(in: directory).path))
        }
    }

    @Test("split view, a space switch, a second window and a move between windows")
    func layoutActions() async throws {
        try await withSupportDirectory { directory in
            let app = try AppRun(
                supportDirectory: directory,
                arguments: [
                    "--do", "split-next", "--do", "new-window=Work", "--do", "focus-window=0",
                    "--do", "move-to-window=1",
                ])
            defer { app.kill() }
            let status = try await app.waitForStatus { $0.actionsDone }
            #expect(status.errors.isEmpty)
            #expect(status.windows.map(\.space) == ["Personal", "Work"])
            // The second Personal page moved into the Work window's split, beside Work's own page.
            let moved = try #require(status.pages.first { $0.window == 1 && $0.pane == 1 })
            #expect(moved.space == "Work" && moved.onScreen)
            #expect(status.windows[1].panes == 2 && status.windows[0].panes == 1)
            #expect(Self.onScreen(status).count == 3)
            #expect(try await app.quitGracefully() == 0)
        }
    }

    @Test("a relaunch restores the layout and each page's history from the saved session")
    func relaunch() async throws {
        try await withSupportDirectory { directory in
            let first = try AppRun(
                supportDirectory: directory,
                arguments: ["--do", "split-next", "--do", "pin", "--do", "switch-space=Work"])
            defer { first.kill() }
            _ = try await first.waitForStatus { $0.actionsDone }
            #expect(try await first.quitGracefully() == 0)
            // What the application showed as it quit, which is what it saved.
            let before = try AppStatus.read(from: first.statusFile)
            #expect(before.phase == .terminating)

            let second = try AppRun(supportDirectory: directory)
            defer { second.kill() }
            let after = try await second.waitForStatus { $0.actionsDone }
            #expect(after.restored)
            #expect(after.windows == before.windows)
            #expect(after.pages.map(\.id) == before.pages.map(\.id))
            #expect(after.pages.map(\.pinned) == before.pages.map(\.pinned))
            #expect(after.pages.map(\.window) == before.pages.map(\.window))
            #expect(after.pages.map(\.url) == before.pages.map(\.url))
            #expect(after.pages.map(\.entries) == before.pages.map(\.entries))
            #expect(try await second.quitGracefully() == 0)
        }
    }

    @Test("the bundled settings page opens at its branded URL", .enabled(if: onChromium))
    func settings() async throws {
        try await withSupportDirectory { directory in
            let app = try AppRun(supportDirectory: directory, arguments: ["--do", "settings"])
            defer { app.kill() }
            let status = try await app.waitForStatus {
                $0.actionsDone && $0.settingsURL?.hasPrefix("arc:") == true
            }
            #expect(status.settingsURL == "arc://settings/")
            #expect(try await app.quitGracefully() == 0)
        }
    }

    @Test("the automation script reads one page in each space", .enabled(if: onChromium))
    func automation() async throws {
        try await withSupportDirectory { directory in
            let app = try AppRun(supportDirectory: directory, arguments: ["--do", "automation"])
            defer { app.kill() }
            let status = try await app.waitForStatus(timeout: .seconds(60)) {
                $0.actionsDone && $0.automationResult != nil
            }
            let result = try #require(status.automationResult)
            #expect(result.contains("\"session\":\"spaces\""))
            #expect(result.contains("\"profile\""))
            #expect(try await app.quitGracefully() == 0)
        }
    }
}
